// Immich photo frame for the "ESP32 RJ9 Adapter" board.
//
// loop() (core 1) runs the slideshow: it fetches batches of asset ids from
// Immich, streams each preview JPEG straight into the ILI9341 and handles the
// buttons and touch screen. The web portal runs in its own task on core 0.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <deque>

#include "app_config.h"
#include "app_state.h"
#include "display_ui.h"
#include "immich_client.h"
#include "jpeg_stream.h"
#include "portal.h"

namespace {

// Board pins (see ESP32-EasyEDA schematic, sheets "MCU" and "Buttons and LEDs").
constexpr int kPinLed = 2;       // LED2, active high
constexpr int kPinBtnNext = 39;  // BTN1, active low, external 10k pull-up
constexpr int kPinBtnPrev = 36;  // BTN2, active low, external 10k pull-up
constexpr int kPinBoot = 0;      // BOOT1, active low
constexpr int kPinSdCs = 33;     // SD card on the display module; keep it off the bus

constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kApRetryStaMs = 60000;
constexpr uint32_t kRetryAfterErrorMs = 30000;
constexpr size_t kBatchSize = 20;
constexpr size_t kHistorySize = 20;

// ---------------------------------------------------------------- inputs ----

struct Button {
  int pin;
  bool down = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool longFired = false;
};

enum class Press { None, Short, Long };

Button gBtnNext{kPinBtnNext}, gBtnPrev{kPinBtnPrev}, gBtnBoot{kPinBoot};

// Debounced; Short fires on release, Long fires once while still held.
Press pollButton(Button& b, uint32_t longMs) {
  const uint32_t now = millis();
  const bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.down && now - b.changedAt > 30) {
    b.down = raw;
    b.changedAt = now;
    if (raw) {
      b.pressedAt = now;
      b.longFired = false;
    } else if (!b.longFired) {
      return Press::Short;
    }
  }
  if (b.down && !b.longFired && now - b.pressedAt >= longMs) {
    b.longFired = true;
    return Press::Long;
  }
  return Press::None;
}

uint32_t gLastTouchPoll = 0;
uint32_t gTouchReleasedAt = 0;
bool gTouchDown = false;
bool gTouchCalibrated = false;

// Left third = previous, middle = pause, right third = next. Without a
// calibration any tap means "next".
void pollTouch() {
  const uint32_t now = millis();
  if (now - gLastTouchPoll < 80) return;
  gLastTouchPoll = now;

  uint16_t x = 0, y = 0;
  bool pressed = gTouchCalibrated ? tft.getTouch(&x, &y) : tft.getTouchRawZ() > 600;
  if (!pressed) {
    if (gTouchDown) gTouchReleasedAt = now;
    gTouchDown = false;
    return;
  }
  if (gTouchDown || now - gTouchReleasedAt < 150) return;
  gTouchDown = true;

  if (!gTouchCalibrated) {
    state::post(state::Cmd::Next);
  } else if (x < tft.width() / 3) {
    state::post(state::Cmd::Prev);
  } else if (x > tft.width() * 2 / 3) {
    state::post(state::Cmd::Next);
  } else {
    state::post(state::Cmd::TogglePause);
  }
}

void pollInputs() {
  switch (pollButton(gBtnNext, 1200)) {
    case Press::Short: state::post(state::Cmd::Next); break;
    case Press::Long: state::post(state::Cmd::TogglePause); break;
    default: break;
  }
  if (pollButton(gBtnPrev, 1200) == Press::Short) state::post(state::Cmd::Prev);
  if (pollButton(gBtnBoot, 6000) == Press::Long) state::post(state::Cmd::FactoryReset);
  pollTouch();
}

// Called between decoded JPEG blocks: lets next/prev interrupt a slow image.
bool abortDecode() {
  pollInputs();
  state::Cmd c = state::peek();
  return c == state::Cmd::Next || c == state::Cmd::Prev || c == state::Cmd::Reload ||
         c == state::Cmd::Reboot || c == state::Cmd::FactoryReset;
}

// ----------------------------------------------------------------- wifi ----

bool gApMode = false;
uint32_t gLastStaAttempt = 0;
String gApName;

void startMdns(const AppConfig& cfg) {
  if (MDNS.begin(cfg.hostname.c_str())) MDNS.addService("http", "tcp", 80);
}

void startAccessPoint(const AppConfig& cfg) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "ImmichFrame-%02X%02X", mac[4], mac[5]);
  gApName = name;

  // Fresh WPA2 password each boot, shown on screen and in the QR code, so a
  // neighbour can't walk onto the setup network. No look-alike characters.
  static const char kChars[] = "abcdefghjkmnpqrstuvwxyz23456789";
  String pass;
  for (int i = 0; i < 8; i++) pass += kChars[esp_random() % (sizeof(kChars) - 1)];

  WiFi.mode(WIFI_AP_STA);
  bool up = WiFi.softAP(name, pass.c_str());
  gApMode = true;
  gLastStaAttempt = millis();
  const String ip = WiFi.softAPIP().toString();
  state::setAp(true, ip);
  Serial.printf("[wifi] setup AP %s %s (pass %s) at %s\n", name, up ? "up" : "FAILED", pass.c_str(), ip.c_str());

  ui::setupScreen(gApName, pass, "http://" + ip, cfg.wifiSsid.length() ? "Could not join " + cfg.wifiSsid : "");
}

bool connectStation(const AppConfig& cfg) {
  ui::message("Connecting", "WiFi: " + cfg.wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiConnectTimeoutMs) {
    digitalWrite(kPinLed, (millis() / 250) % 2);
    delay(50);
  }
  digitalWrite(kPinLed, LOW);
  const bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[wifi] %s %s\n", cfg.wifiSsid.c_str(),
                ok ? ("connected, " + WiFi.localIP().toString()).c_str() : "connect failed");
  return ok;
}

// In setup mode, periodically retry the saved network while nobody is using
// the AP; once it connects, close the AP and start the slideshow.
void serviceAccessPoint(const AppConfig& cfg) {
  digitalWrite(kPinLed, (millis() / 500) % 2);
  // The setup page drives the station itself and restarts when it's done.
  if (cfg.wifiSsid.isEmpty() || portal::setupActive()) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (WiFi.softAPgetStationNum() > 0) return;  // don't yank the AP from someone mid-setup
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    gApMode = false;
    portal::setApMode(false);
    state::setAp(false, WiFi.localIP().toString());
    startMdns(cfg);
    digitalWrite(kPinLed, LOW);
    return;
  }
  if (WiFi.softAPgetStationNum() == 0 && millis() - gLastStaAttempt > kApRetryStaMs) {
    gLastStaAttempt = millis();
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  }
}

// ------------------------------------------------------------ slideshow ----

Asset gQueue[kBatchSize];
size_t gQueueLen = 0, gQueuePos = 0;
int gPage = 1;

std::deque<Asset> gHistory;  // oldest first; back() is the newest shown
size_t gBack = 0;            // 0 = showing the newest, n = n steps back

bool gPaused = false;
uint32_t gNextAt = 0;
uint32_t gCfgVersion = 0;
AppConfig gCfg;
int gFailures = 0;
bool gShowingPattern = false;
String gScreenMessage;  // what's currently on screen if it isn't a photo

void showMessageOnce(const String& title, const String& l1 = "", const String& l2 = "",
                     uint16_t color = TFT_WHITE) {
  String key = title + "|" + l1 + "|" + l2;
  if (key == gScreenMessage) return;
  gScreenMessage = key;
  ui::message(title, l1, l2, "", color);
}

// Neutral placeholder while nothing has been shown yet; details are in the portal.
void showWaiting() {
  if (!gHistory.empty()) return;
  showMessageOnce("Waiting for photos", "Status and details at", "http://" + WiFi.localIP().toString());
}

void resetSlideshow() {
  gQueueLen = gQueuePos = 0;
  gPage = 1;
  gHistory.clear();
  gBack = 0;
  gFailures = 0;
  gNextAt = 0;
}

bool drawAsset(const Asset& a, String& err, bool& aborted, bool& touched) {
  aborted = false;
  touched = false;
  digitalWrite(kPinLed, HIGH);
  bool ok = immich::fetchPreview(gCfg, a.id, [&](Stream& in, int len, String& e) {
    bool drawn = jpeg::draw(tft, in, len, gCfg.fill, abortDecode, e, aborted, touched);
    if (touched) gScreenMessage = "";
    return drawn;
  }, err);
  digitalWrite(kPinLed, LOW);
  if (ok) {
    if (gCfg.showDate) ui::caption(ui::formatDate(a.date));
    ui::pausedBadge(gPaused);
  }
  return ok;
}

// step: +1 next, -1 previous, 0 redraw the current photo.
void advance(int step) {
  gShowingPattern = false;
  Asset asset;
  bool fresh = false;

  if (step < 0) {
    if (gBack + 1 >= gHistory.size()) return;  // nothing older
    gBack++;
    asset = gHistory[gHistory.size() - 1 - gBack];
  } else if (step == 0) {
    if (gHistory.empty()) return advance(1);
    asset = gHistory[gHistory.size() - 1 - gBack];
  } else if (gBack > 0) {
    gBack--;
    asset = gHistory[gHistory.size() - 1 - gBack];
  } else {
    if (gQueuePos >= gQueueLen) {
      String err;
      gQueuePos = 0;
      if (!immich::fetchBatch(gCfg, gPage, gQueue, kBatchSize, gQueueLen, err)) {
        gQueueLen = 0;
        state::setError("Loading photo list: " + err);
        log_w("fetchBatch: %s", err.c_str());
        showWaiting();
        gNextAt = millis() + kRetryAfterErrorMs;
        return;
      }
      if (gQueueLen == 0) {
        state::setError("No photos found for this selection");
        showWaiting();
        gNextAt = millis() + kRetryAfterErrorMs * 2;
        return;
      }
    }
    asset = gQueue[gQueuePos++];
    fresh = true;
  }

  String err;
  bool aborted, touched;
  if (drawAsset(asset, err, aborted, touched)) {
    gFailures = 0;
    if (fresh) {
      gHistory.push_back(asset);
      if (gHistory.size() > kHistorySize) gHistory.pop_front();
    }
    state::setShowing(asset.id, asset.date);
    gNextAt = millis() + gCfg.intervalSec * 1000UL;
    return;
  }
  if (aborted) return;  // a pending command takes over

  // Errors go to the portal only; the screen keeps the last good photo.
  log_w("asset %s: %s", asset.id, err.c_str());
  state::setError(String("Skipped ") + asset.id + ": " + err);
  state::countSkipped();
  gFailures++;
  if (touched && !gHistory.empty()) {
    // The failed image got partly drawn; put the current photo back.
    bool redrawAborted, redrawTouched;
    String redrawErr;
    if (!drawAsset(gHistory[gHistory.size() - 1 - gBack], redrawErr, redrawAborted, redrawTouched) &&
        !redrawAborted)
      log_w("redraw: %s", redrawErr.c_str());
  } else if (gHistory.empty()) {
    showWaiting();
  }
  // Skip ahead quickly, but back off if nothing is working.
  gNextAt = millis() + (gFailures >= 5 ? kRetryAfterErrorMs : 1000);
}

void calibrateTouch() {
  gScreenMessage = "";
  ui::message("Touch calibration", "Touch each corner arrow as it appears");
  delay(1500);
  AppConfig cfg = config::get();
  tft.fillScreen(TFT_BLACK);
  tft.calibrateTouch(cfg.touchCal, TFT_MAGENTA, TFT_BLACK, 15);
  tft.setTouch(cfg.touchCal);
  cfg.touchCalValid = true;
  gTouchCalibrated = true;
  config::set(cfg);
  gCfgVersion = config::version();  // nothing else changed
  gCfg = cfg;
  ui::message("Calibrated", "Left: prev  Mid: pause  Right: next");
  delay(1500);
  advance(0);
}

void handleCommands() {
  for (state::Cmd cmd = state::take(); cmd != state::Cmd::None; cmd = state::take()) {
    switch (cmd) {
      case state::Cmd::Next:
        if (!gApMode) advance(1);
        break;
      case state::Cmd::Prev:
        if (!gApMode) advance(-1);
        break;
      case state::Cmd::TogglePause:
        gPaused = !gPaused;
        state::setPaused(gPaused);
        if (gPaused) {
          ui::pausedBadge(true);
        } else {
          gNextAt = millis() + 1500;
        }
        break;
      case state::Cmd::Reload:
        resetSlideshow();
        break;
      case state::Cmd::TestPattern:
        ui::testPattern();
        gShowingPattern = true;
        gScreenMessage = "";
        gNextAt = millis() + 60000;  // hold it for a minute
        break;
      case state::Cmd::CalibrateTouch:
        calibrateTouch();
        break;
      case state::Cmd::Reboot:
        ui::message("Restarting...");
        delay(800);  // let the HTTP response go out
        ESP.restart();
        break;
      case state::Cmd::FactoryReset:
        ui::message("Factory reset", "Erasing settings...", "", "", TFT_RED);
        config::erase();
        WiFi.disconnect(true, true);
        delay(800);
        ESP.restart();
        break;
      case state::Cmd::None:
        break;
    }
  }
}

// Re-reads the config after the portal saved it and resets what's affected.
void applyConfigChanges() {
  if (config::version() == gCfgVersion) return;
  AppConfig next = config::get();
  gCfgVersion = config::version();

  const bool sourceChanged = next.source != gCfg.source || next.sourceId != gCfg.sourceId ||
                             next.order != gCfg.order || next.serverUrl != gCfg.serverUrl ||
                             next.apiKey != gCfg.apiKey;
  const bool lookChanged = next.fill != gCfg.fill || next.showDate != gCfg.showDate;
  const bool rotated = next.rotation != gCfg.rotation;
  const bool colorChanged = next.colorBgr != gCfg.colorBgr || next.colorInvert != gCfg.colorInvert;
  const bool intervalChanged = next.intervalSec != gCfg.intervalSec;
  gCfg = next;

  if (rotated || colorChanged) {
    ui::applyPanel(gCfg.rotation, gCfg.colorBgr, gCfg.colorInvert);
    if (rotated) gTouchCalibrated = false;
    gScreenMessage = "";
  }
  if (gShowingPattern && (rotated || colorChanged)) {
    ui::testPattern();  // keep the pattern up while colours are being tuned
  } else if (sourceChanged) {
    resetSlideshow();
  } else if (rotated || colorChanged || lookChanged) {
    advance(0);
  } else if (intervalChanged) {
    gNextAt = millis() + gCfg.intervalSec * 1000UL;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  pinMode(kPinSdCs, OUTPUT);
  digitalWrite(kPinSdCs, HIGH);
  pinMode(kPinLed, OUTPUT);
  digitalWrite(kPinLed, LOW);
  pinMode(kPinBtnNext, INPUT);  // GPIO36/39 are input-only with no internal pull-ups
  pinMode(kPinBtnPrev, INPUT);
  pinMode(kPinBoot, INPUT_PULLUP);

  config::begin();
  state::begin();
  immich::begin();
  gCfg = config::get();
  gCfgVersion = config::version();

  ui::begin(gCfg.rotation, gCfg.colorBgr, gCfg.colorInvert);
  if (gCfg.touchCalValid) {
    tft.setTouch(gCfg.touchCal);
    gTouchCalibrated = true;
  }

  // Holding both buttons while powering up wipes all settings.
  if (digitalRead(kPinBtnNext) == LOW && digitalRead(kPinBtnPrev) == LOW) {
    ui::message("Factory reset?", "Keep holding both buttons for 3 s", "", "", TFT_RED);
    uint32_t start = millis();
    while (digitalRead(kPinBtnNext) == LOW && digitalRead(kPinBtnPrev) == LOW) {
      if (millis() - start > 3000) {
        config::erase();
        ui::message("Settings erased", "Release the buttons");
        while (digitalRead(kPinBtnNext) == LOW || digitalRead(kPinBtnPrev) == LOW) delay(20);
        ESP.restart();
      }
      delay(20);
    }
  }

  WiFi.persistent(false);
  if (gCfg.wifiSsid.length() && connectStation(gCfg)) {
    state::setAp(false, WiFi.localIP().toString());
    startMdns(gCfg);
    ui::message("Connected", WiFi.localIP().toString(), "http://" + gCfg.hostname + ".local", "", TFT_GREEN);
    delay(2000);
  } else {
    startAccessPoint(gCfg);
  }

  portal::begin(gApMode);
}

void loop() {
  pollInputs();
  handleCommands();
  applyConfigChanges();

  if (gApMode) {
    serviceAccessPoint(gCfg);
    delay(10);
    return;
  }

  if (gCfg.serverUrl.isEmpty() || gCfg.apiKey.isEmpty()) {
    showMessageOnce("Almost there", "Open http://" + WiFi.localIP().toString(), "to set Immich server + key",
                    TFT_YELLOW);
  } else if (WiFi.status() != WL_CONNECTED) {
    if (gHistory.empty()) showMessageOnce("WiFi disconnected", "Reconnecting to " + gCfg.wifiSsid);
  } else if (!gPaused && (int32_t)(millis() - gNextAt) >= 0) {
    advance(1);
  }
  delay(10);
}
