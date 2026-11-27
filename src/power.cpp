#include "power.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/rtc_io.h>
#include <esp32/ulp.h>
#include <esp_sleep.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_io_reg.h>
#include <soc/soc.h>

#include "board.h"

namespace power {
namespace {

// The backlight PWM is clocked from the RTC 8 MHz oscillator, which (unlike
// the APB clock) keeps running in light sleep, so a dimmed screen stays dimmed
// while the chip sleeps between photos.
constexpr ledc_mode_t kBlMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBlTimer = LEDC_TIMER_3;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_7;
constexpr uint32_t kBlFreq = 20000;  // above hearing; 8-bit at 20 kHz fits the 8 MHz clock
bool gBlInSleep = false;            // the PWM survives light sleep
bool gBlReady = false;

constexpr uint32_t kSampleMs = 100;
constexpr int kWindowSamples = 50;   // one averaged reading every 5 s awake
constexpr int kHistory = 36;         // 36 readings for the trend
constexpr int kNoBatteryJitterMv = 150;  // charger hunting with no cell fitted (~190 mV seen)
constexpr int kNoBatteryMv = 4300;
constexpr int kStepMv = 30;   // USB plugged in / pulled out shifts the cell at once
constexpr int kTrendMv = 8;   // over kHistory windows
constexpr int kFullMv = 4150;
constexpr int kChargeRiseMv = 100;  // charge current lifts the reading this much (measured on unplug)

volatile uint8_t gBacklight = 100;

// ---- sleep bookkeeping, kept in RTC memory across deep sleep ----
enum : uint8_t { kSleptNone, kSleptShutdown, kSleptSaver };
RTC_DATA_ATTR uint8_t gSleepKind = kSleptNone;
RTC_DATA_ATTR bool gUlpBacklight = false;  // the ULP is driving the backlight
Boot gBoot = Boot::Cold;

// ---- backlight PWM on the ULP, for deep sleep ----
// The ULP loops: set the pin, wait, clear it, wait; it halts once the stop
// word (in its reserved RTC memory) is non-zero.
constexpr int kUlpStopWord = 120;     // of the 128 reserved words; the program uses ~12
constexpr uint32_t kUlpPeriod = 4000;  // ULP cycles (~8.5 MHz): about 2 kHz

void ulpBacklightStop() {
  RTC_SLOW_MEM[kUlpStopWord] = 1;
  CLEAR_PERI_REG_MASK(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN);  // no restarts
  delay(2);                                                               // a period or two to halt
  if (board::kHasBacklight) rtc_gpio_deinit((gpio_num_t)board::kPinBacklight);
  gUlpBacklight = false;
}

uint32_t blDuty(uint8_t pct);

bool ulpBacklightStart(uint8_t pct) {
  const gpio_num_t pin = (gpio_num_t)board::kPinBacklight;
  const int bit = rtc_io_number_get(pin);
  if (bit < 0) return false;
  uint32_t on = kUlpPeriod * blDuty(pct) / 256;
  on = constrain(on, 20u, kUlpPeriod);
  const uint32_t off = kUlpPeriod - on;

  RTC_SLOW_MEM[kUlpStopWord] = 0;
  const ulp_insn_t pwm[] = {
      M_LABEL(1),
      I_MOVI(R3, kUlpStopWord),
      I_LD(R0, R3, 0),
      M_BGE(2, 1),
      I_WR_REG(RTC_GPIO_OUT_W1TS_REG, RTC_GPIO_OUT_DATA_W1TS_S + bit, RTC_GPIO_OUT_DATA_W1TS_S + bit, 1),
      I_DELAY((uint16_t)on),
      I_WR_REG(RTC_GPIO_OUT_W1TC_REG, RTC_GPIO_OUT_DATA_W1TC_S + bit, RTC_GPIO_OUT_DATA_W1TC_S + bit, 1),
      I_DELAY((uint16_t)max<uint32_t>(off, 1)),
      M_BX(1),
      M_LABEL(2),
      I_HALT(),
  };
  const ulp_insn_t full[] = {  // 100%: just on, still watching the stop word
      M_LABEL(1),
      I_MOVI(R3, kUlpStopWord),
      I_LD(R0, R3, 0),
      M_BGE(2, 1),
      I_WR_REG(RTC_GPIO_OUT_W1TS_REG, RTC_GPIO_OUT_DATA_W1TS_S + bit, RTC_GPIO_OUT_DATA_W1TS_S + bit, 1),
      I_DELAY((uint16_t)kUlpPeriod),
      M_BX(1),
      M_LABEL(2),
      I_HALT(),
  };
  const bool isFull = off < 20;
  size_t size = isFull ? sizeof(full) / sizeof(full[0]) : sizeof(pwm) / sizeof(pwm[0]);

  if (gBlReady) ledc_stop(kBlMode, kBlChannel, 1);
  rtc_gpio_init(pin);
  rtc_gpio_set_direction(pin, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level(pin, 1);
  if (ulp_process_macros_and_load(0, isFull ? full : pwm, &size) != ESP_OK || ulp_run(0) != ESP_OK) {
    rtc_gpio_deinit(pin);
    return false;
  }
  gUlpBacklight = true;
  return true;
}

portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
Battery gBattery;

// Resting LiPo voltage (mV) to state of charge.
struct Point { int mv, pct; };
constexpr Point kCurve[] = {{4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75},
                            {3950, 70},  {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45},
                            {3800, 40},  {3790, 35}, {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15},
                            {3690, 10},  {3610, 5},  {3300, 0}};

int percentFor(int mv) {
  if (mv >= kCurve[0].mv) return 100;
  for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
    const Point a = kCurve[i - 1], b = kCurve[i];
    if (mv >= b.mv) return b.pct + (a.pct - b.pct) * (mv - b.mv) / (a.mv - b.mv);
  }
  return 0;
}

}  // namespace

namespace {

// Perceived brightness is roughly quadratic in duty. 256 = always on.
uint32_t blDuty(uint8_t pct) {
  if (pct >= 100) return 256;
  return max<uint32_t>(1, 256u * pct * pct / 10000);
}

void writeBacklight(uint32_t duty) {
  if (!gBlReady) return;
  ledc_set_duty(kBlMode, kBlChannel, duty);
  ledc_update_duty(kBlMode, kBlChannel);
}

void backlightBegin() {
  ledc_timer_config_t t = {};
  t.speed_mode = kBlMode;
  t.duty_resolution = LEDC_TIMER_8_BIT;
  t.timer_num = kBlTimer;
  t.freq_hz = kBlFreq;
  t.clk_cfg = LEDC_USE_RTC8M_CLK;
  gBlInSleep = ledc_timer_config(&t) == ESP_OK;
  if (!gBlInSleep) {
    log_w("backlight: RTC8M clock unavailable, PWM stops in light sleep");
    t.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) != ESP_OK) return;
  }
  ledc_channel_config_t c = {};
  c.gpio_num = board::kPinBacklight;
  c.speed_mode = kBlMode;
  c.channel = kBlChannel;
  c.timer_sel = kBlTimer;
  c.duty = blDuty(gBacklight);
  gBlReady = ledc_channel_config(&c) == ESP_OK;
  gpio_sleep_sel_dis((gpio_num_t)board::kPinBacklight);  // keep the pad as is in light sleep
}

// ---- battery state ----
int gSum = 0, gCount = 0, gMin = INT_MAX, gMax = 0;
int gHist[kHistory];
int gHistLen = 0, gHistPos = 0;
int gPrevMv = -1;
BatteryState gState = BatteryState::Unknown;
int gNoisyWindows = 0;  // consecutive windows that looked like no cell

void window(int avg, int jitter) {
  BatteryState st = gState;
  // WiFi bursts dip a real cell briefly; only a sustained swing means no cell.
  gNoisyWindows = jitter > kNoBatteryJitterMv ? gNoisyWindows + 1 : 0;
  if (gNoisyWindows >= 2 || avg > kNoBatteryMv) {
    st = BatteryState::None;
    gHistLen = 0;  // trend restarts once a cell is back
  } else {
    if (st == BatteryState::None) st = BatteryState::Unknown;
    if (gPrevMv > 0 && avg - gPrevMv > kStepMv) st = BatteryState::Charging;
    else if (gPrevMv > 0 && gPrevMv - avg > kStepMv) st = BatteryState::Discharging;
    else if (gHistLen == kHistory) {
      const int oldest = gHist[gHistPos];  // the slot about to be overwritten
      if (avg - oldest > kTrendMv) st = BatteryState::Charging;
      else if (oldest - avg > kTrendMv) st = BatteryState::Discharging;
    }
    // Not charging and near the top: the charger has finished (or it was just unplugged full).
    if (st != BatteryState::Charging && avg >= kFullMv) st = BatteryState::Full;
    if (st == BatteryState::Full && avg < kFullMv - 30) st = BatteryState::Discharging;
    gHist[gHistPos] = avg;
    gHistPos = (gHistPos + 1) % kHistory;
    if (gHistLen < kHistory) gHistLen++;
  }
  if (st != gState) Serial.printf("[battery] %s, %d mV (jitter %d)\n", stateName(st), avg, jitter);
  gState = st;
  gPrevMv = st == BatteryState::None ? -1 : avg;

  Battery b;
  b.state = st;
  b.volts = avg / 1000.0f;
  // The curve is for a resting cell, so take out the lift while charging.
  b.percent = st == BatteryState::None ? -1 : percentFor(st == BatteryState::Charging ? avg - kChargeRiseMv : avg);
  portENTER_CRITICAL(&gMux);
  gBattery = b;
  portEXIT_CRITICAL(&gMux);
}

void sample() {
  const int mv = (int)(analogReadMilliVolts(board::kPinBattery) * board::kBatteryDivider);
  gSum += mv;
  gCount++;
  gMin = min(gMin, mv);
  gMax = max(gMax, mv);
  if (gCount < kWindowSamples) return;
  const int avg = gSum / gCount, jitter = gMax - gMin;
  gSum = gCount = 0;
  gMin = INT_MAX;
  gMax = 0;
  window(avg, jitter);
}

void task(void*) {
  for (;;) {
    sample();
    vTaskDelay(pdMS_TO_TICKS(kSampleMs));
  }
}

void holdOutput(int pin, int level) {
  if (pin < 0) return;
  gpio_hold_dis((gpio_num_t)pin);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level);
  gpio_hold_en((gpio_num_t)pin);
}

}  // namespace

void releaseSleepHolds() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (gSleepKind == kSleptShutdown && cause == ESP_SLEEP_WAKEUP_EXT0) {
    gBoot = Boot::FromShutdown;
  } else if (gSleepKind == kSleptSaver) {
    gBoot = cause == ESP_SLEEP_WAKEUP_TIMER  ? Boot::SaverTimer
            : cause == ESP_SLEEP_WAKEUP_EXT0 ? Boot::SaverButton
            : cause == ESP_SLEEP_WAKEUP_EXT1 ? Boot::SaverTouch
                                             : Boot::Cold;
  }
  gSleepKind = kSleptNone;
  // A reset that wasn't a saver wake-up may leave the ULP running.
  if (!saverWake(gBoot)) ulpBacklightStop();

  // Panel and SD chip-selects: drive them high before letting go of the hold.
  for (int pin : {TFT_CS, TOUCH_CS, board::kPinSdCs}) {
    if (pin < 0) continue;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_dis((gpio_num_t)pin);
  }
  gpio_deep_sleep_hold_dis();
  for (int pin : board::kPinLedOff)
    if (pin >= 0) gpio_hold_dis((gpio_num_t)pin);
  if (board::kPinLed >= 0) gpio_hold_dis((gpio_num_t)board::kPinLed);
  if (board::kHasBacklight && !gUlpBacklight) gpio_hold_dis((gpio_num_t)board::kPinBacklight);
  // Wake-up pins are left routed to the RTC domain.
  if (cause == ESP_SLEEP_WAKEUP_EXT0) rtc_gpio_deinit((gpio_num_t)board::kPinBoot);
  if (cause == ESP_SLEEP_WAKEUP_EXT1 && board::kPinTouchIrq >= 0) rtc_gpio_deinit((gpio_num_t)board::kPinTouchIrq);

  // Unused LEDs off from the start.
  for (int pin : board::kPinLedOff) {
    if (pin < 0) continue;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, board::kLedActiveHigh ? LOW : HIGH);
  }
  if (board::kPinLed >= 0) {
    pinMode(board::kPinLed, OUTPUT);
    digitalWrite(board::kPinLed, board::kLedActiveHigh ? LOW : HIGH);
  }
}

Boot bootReason() { return gBoot; }
bool wokeFromShutdown() { return gBoot == Boot::FromShutdown; }

void begin(uint8_t backlightPct) {
  gBacklight = constrain(backlightPct, 5, 100);
  if (board::kHasBacklight && !gUlpBacklight) backlightBegin();
  if (board::kHasBattery) {
    analogSetPinAttenuation(board::kPinBattery, ADC_11db);
    // Seed with one quick reading so the portal has a value before the first window.
    int sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(board::kPinBattery);
    const int mv = (int)(sum / 16 * board::kBatteryDivider);
    gBattery.state = BatteryState::Unknown;
    gBattery.volts = mv / 1000.0f;
    gBattery.percent = mv > kNoBatteryMv ? -1 : percentFor(mv);
    xTaskCreatePinnedToCore(task, "power", 2560, nullptr, 1, nullptr, 0);
  }
}

void takeBacklight() {
  if (!gUlpBacklight) return;
  ulpBacklightStop();
  if (board::kHasBacklight) backlightBegin();
}

void setBacklight(uint8_t pct) {
  gBacklight = constrain(pct, 5, 100);
  writeBacklight(blDuty(gBacklight));
}

void setBusy(bool busy) {
  if (board::kPinLed >= 0) digitalWrite(board::kPinLed, busy == board::kLedActiveHigh ? HIGH : LOW);
}

Battery battery() {
  portENTER_CRITICAL(&gMux);
  Battery b = gBattery;
  portEXIT_CRITICAL(&gMux);
  return b;
}

Battery measureBattery() {
  Battery b = battery();
  if (!board::kHasBattery || b.state == BatteryState::None) return b;
  int sum = 0;
  for (int i = 0; i < 32; i++) sum += analogReadMilliVolts(board::kPinBattery);
  const int mv = (int)(sum / 32 * board::kBatteryDivider);
  b.volts = mv / 1000.0f;
  b.percent = percentFor(b.state == BatteryState::Charging ? mv - kChargeRiseMv : mv);
  return b;
}

const char* stateName(BatteryState s) {
  switch (s) {
    case BatteryState::Discharging: return "on battery";
    case BatteryState::Charging: return "charging";
    case BatteryState::Full: return "full";
    case BatteryState::Unknown: return "measuring";
    default: return "none";
  }
}

void saverSleep(uint32_t ms, bool touchWakes) {
  if (board::kHasBacklight && !gUlpBacklight && !ulpBacklightStart(gBacklight)) {
    // No ULP: hold the backlight fully on instead.
    if (gBlReady) ledc_stop(kBlMode, kBlChannel, 1);
    holdOutput(board::kPinBacklight, HIGH);
  }
  // Nothing may reach the panel, touch controller or card while asleep.
  for (int pin : {TFT_CS, TOUCH_CS, board::kPinSdCs}) holdOutput(pin, HIGH);
  for (int pin : board::kPinLedOff) holdOutput(pin, board::kLedActiveHigh ? LOW : HIGH);
  holdOutput(board::kPinLed, board::kLedActiveHigh ? LOW : HIGH);
  gpio_deep_sleep_hold_en();

  const gpio_num_t boot = (gpio_num_t)board::kPinBoot;
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (ms) esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
  esp_sleep_enable_ext0_wakeup(boot, 0);
  rtc_gpio_pullup_en(boot);
  rtc_gpio_pulldown_dis(boot);
  // A finger still down (or an unarmed controller) would wake it at once.
  if (touchWakes && board::kPinTouchIrq >= 0 && digitalRead(board::kPinTouchIrq) == HIGH)
    esp_sleep_enable_ext1_wakeup(1ULL << board::kPinTouchIrq, ESP_EXT1_WAKEUP_ALL_LOW);
  if (gUlpBacklight) {
    // The ULP keeps running in deep sleep as long as its domains stay powered.
    // (Its wake-up source would keep them on too, but can't be combined with
    // ext0, BOOT's wake-up.)
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC8M, ESP_PD_OPTION_ON);
  }
  gSleepKind = kSleptSaver;
  Serial.flush();
  esp_deep_sleep_start();
}

void shutdown() {
  if (gUlpBacklight) ulpBacklightStop();
  if (gBlReady) ledc_stop(kBlMode, kBlChannel, 0);
  if (board::kHasBacklight) holdOutput(board::kPinBacklight, LOW);
  for (int pin : board::kPinLedOff) holdOutput(pin, board::kLedActiveHigh ? LOW : HIGH);
  holdOutput(board::kPinLed, board::kLedActiveHigh ? LOW : HIGH);
  gpio_deep_sleep_hold_en();

  const gpio_num_t boot = (gpio_num_t)board::kPinBoot;
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(boot, 0);
  rtc_gpio_pullup_en(boot);
  rtc_gpio_pulldown_dis(boot);
  gSleepKind = kSleptShutdown;
  Serial.println("[power] shut down; press BOOT to start");
  Serial.flush();
  esp_deep_sleep_start();
}

}  // namespace power
