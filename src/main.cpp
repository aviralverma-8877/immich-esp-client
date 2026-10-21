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
#include "frame_buffer.h"
#include "transitions.h"

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
constexpr int kMaxQuickSkips = 8;  // consecutive unusable previews tried back to back

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

// While prefetching, only commands that need the screen or change what's
// shown interrupt; "next" simply waits for the photo being prepared.
bool abortPrefetch() {
  pollInputs();
  state::Cmd c = state::peek();
  return c != state::Cmd::None && c != state::Cmd::Next && c != state::Cmd::TogglePause;
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
  ui::splash("Connecting to " + cfg.wifiSsid + "...");
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

// The next photo, decoded into fb:: while the current one is on screen, so
// next/timer only has to run the transition.
Asset gReady;
bool gHaveReady = false;
// A photo taken from the queue but not shown (its prefetch was interrupted or
// its decode discarded); picked again before the queue moves on.
Asset gPending;
bool gHavePending = false;

void dropReady(bool keepForLater) {
  if (gHaveReady && keepForLater) {
    gPending = gReady;
    gHavePending = true;
  }
  gHaveReady = false;
}

void resetSlideshow() {
  gQueueLen = gQueuePos = 0;
  gPage = 1;
  gHistory.clear();
  gBack = 0;
  gFailures = 0;
  gNextAt = 0;
  dropReady(false);
  gHavePending = false;
}

bool wantBuffer() { return fx::fromName(gCfg.transition) != fx::Effect::None; }

// Over HTTPS each TLS handshake needs ~40 KB, so the buffer can't stay
// allocated between photos and there is no prefetch.
bool keepBuffer() { return !gCfg.serverUrl.startsWith("https://"); }

bool ensureBuffer() {
  if (fb::active() && fb::width() == tft.width() && fb::height() == tft.height()) return true;
  fb::end();
  if (!wantBuffer()) return false;
  if (fb::begin(tft.width(), tft.height())) return true;
  log_w("no memory for transition buffer (%u free)", ESP.getFreeHeap());
  return false;
}

// Downloads and decodes one preview: into the buffer when one is available
// (screen untouched), otherwise straight onto the panel.
bool loadAsset(const Asset& a, bool prefetch, String& err, bool& aborted, bool& touched, bool& buffered) {
  aborted = touched = buffered = false;
  digitalWrite(kPinLed, HIGH);
  bool ok = immich::fetchPreview(gCfg, a.id, [&](Stream& in, int len, String& e) {
    // Allocated after the response headers, i.e. after any TLS handshake.
    buffered = ensureBuffer();
    if (prefetch && !buffered) {
      e = "no buffer";
      return false;
    }
    bool drawn = jpeg::draw(tft, in, len, gCfg.fill, prefetch ? abortPrefetch : abortDecode, e, aborted,
                            touched, buffered);
    if (touched) gScreenMessage = "";
    return drawn;
  }, err);
  digitalWrite(kPinLed, LOW);
  return ok;
}

void finishShowing(const Asset& a, bool fromBuffer) {
  if (fromBuffer) {
    gScreenMessage = "";
    fx::run(tft, fx::resolve(fx::fromName(gCfg.transition)));
    if (!keepBuffer()) fb::end();
  }
  if (gCfg.showDate) ui::caption(ui::formatDate(a.date));
  ui::pausedBadge(gPaused);
  state::setShowing(a.id, a.date);
  gFailures = 0;
  gNextAt = millis() + gCfg.intervalSec * 1000UL;
}

void recordSkip(const Asset& a, const String& err) {
  // Errors go to the portal only; the screen keeps the last good photo.
  log_w("asset %s: %s", a.id, err.c_str());
  state::setError(String("Skipped ") + a.id + ": " + err);
  state::countSkipped();
  gFailures++;
}

// Next unseen asset, refilling the queue from Immich when it runs out.
// `quiet` (prefetching) leaves the screen and the timer alone on errors.
bool pickFresh(Asset& out, bool quiet) {
  if (gHavePending) {
    out = gPending;
    gHavePending = false;
    return true;
  }
  if (gQueuePos >= gQueueLen) {
    String err;
    gQueuePos = 0;
    if (!immich::fetchBatch(gCfg, gPage, gQueue, kBatchSize, gQueueLen, err)) {
      gQueueLen = 0;
      state::setError("Loading photo list: " + err);
      log_w("fetchBatch: %s", err.c_str());
      if (!quiet) {
        showWaiting();
        gNextAt = millis() + kRetryAfterErrorMs;
      }
      return false;
    }
    if (gQueueLen == 0) {
      state::setError("No photos found for this selection");
      if (!quiet) {
        showWaiting();
        gNextAt = millis() + kRetryAfterErrorMs * 2;
      }
      return false;
    }
  }
  out = gQueue[gQueuePos++];
  return true;
}

void pushHistory(const Asset& a) {
  gHistory.push_back(a);
  if (gHistory.size() > kHistorySize) gHistory.pop_front();
}

// Decodes the upcoming photo off-screen, skipping unusable previews.
void prefetchNext() {
  if (gHaveReady || gBack > 0 || !fb::active() || !keepBuffer()) return;
  for (int attempt = 0; attempt < kMaxQuickSkips; attempt++) {
    Asset a;
    if (!pickFresh(a, true)) return;
    String err;
    bool aborted, touched, buffered;
    if (loadAsset(a, true, err, aborted, touched, buffered)) {
      gReady = a;
      gHaveReady = true;
      return;
    }
    if (aborted || !buffered) {  // a command wants the loop, or the buffer went away
      gPending = a;
      gHavePending = true;
      return;
    }
    recordSkip(a, err);
  }
}

// step: +1 next, -1 previous, 0 redraw the current photo.
void advance(int step) {
  gShowingPattern = false;

  // Fast path: the next photo is already decoded.
  if (step > 0 && gBack == 0 && gHaveReady && fb::active()) {
    gHaveReady = false;
    pushHistory(gReady);
    finishShowing(gReady, true);
    prefetchNext();
    return;
  }

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
    if (!pickFresh(asset, false)) return;
    fresh = true;
  }

  dropReady(true);  // the buffer is about to be overwritten
  String err;
  bool aborted, touched, buffered;
  if (loadAsset(asset, false, err, aborted, touched, buffered)) {
    if (fresh) pushHistory(asset);
    finishShowing(asset, buffered);
    prefetchNext();
    return;
  }
  if (aborted) {
    if (fresh) {
      gPending = asset;
      gHavePending = true;
    }
    return;  // a pending command takes over
  }

  recordSkip(asset, err);
  if (touched && !gHistory.empty()) {
    // Drawn straight to the panel and failed part way: put the current photo back.
    String redrawErr;
    bool a2, t2, b2;
    const Asset& cur = gHistory[gHistory.size() - 1 - gBack];
    if (loadAsset(cur, false, redrawErr, a2, t2, b2)) {
      if (b2) fx::run(tft, fx::Effect::WipeDown);
    } else if (!a2) {
      log_w("redraw: %s", redrawErr.c_str());
    }
  } else if (gHistory.empty()) {
    showWaiting();
  }
  // A preview rejected at its header costs little, so try the next one right
  // away: otherwise next/prev presses seem to do nothing on a library with
  // many broken previews. Back off once nothing is working.
  if (fresh && !touched && gFailures < kMaxQuickSkips) return advance(1);
  gNextAt = millis() + (gFailures >= kMaxQuickSkips ? kRetryAfterErrorMs : 1000);
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
  const String prevTransition = gCfg.transition;
  gCfg = next;

  const bool transitionChanged = next.transition != prevTransition;
  if (rotated || lookChanged || transitionChanged) dropReady(true);  // decoded for the old settings
  if (rotated || !wantBuffer()) fb::end();
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
    ui::splashStatus("Connected - " + WiFi.localIP().toString(), TFT_GREEN);
    delay(1500);
    ui::splashStatus("Loading photos...");  // the first photo replaces the splash
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
