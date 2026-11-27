// Immich photo frame for ESP32 boards with an SPI TFT (see board.h).
//
// loop() (core 1) runs the slideshow: it fetches batches of asset ids from
// Immich, streams each preview JPEG straight into the panel and handles the
// buttons and touch screen. The web portal runs in its own task on core 0.
//
// With an album selected and an SD card fitted, the album is stored on the
// card instead (sd_cache.h) and played from there. Once every photo is stored
// and nobody is using the portal, WiFi goes off and the chip light-sleeps
// between photos. BOOT: press to come back online, hold to shut down.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <rom/crc.h>
#include <sys/time.h>

#include <atomic>
#include <deque>

#include "app_config.h"
#include "app_state.h"
#include "board.h"
#include "display_ui.h"
#include "immich_client.h"
#include "jpeg_stream.h"
#include "portal.h"
#include "power.h"
#include "frame_buffer.h"
#include "sd_cache.h"
#include "transitions.h"

namespace {

using board::kPinBoot;
using board::kPinBtn1;
using board::kPinBtn2;

void setLed(bool on) { power::setBusy(on); }

constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kApRetryStaMs = 60000;
constexpr uint32_t kRetryAfterErrorMs = 30000;
constexpr uint32_t kRoamCheckMs = 5 * 60 * 1000;
constexpr int kRoamWeakRssi = -70;  // dBm; only look for a better AP below this
constexpr int kRoamMinGain = 8;     // dB stronger before it is worth reconnecting
constexpr size_t kBatchSize = 20;
constexpr size_t kHistorySize = 20;
constexpr int kMaxQuickSkips = 8;  // consecutive unusable previews tried back to back
constexpr uint32_t kShutdownHoldMs = 2000;          // hold BOOT this long to shut down
constexpr uint32_t kOnlineIdleMs = 3 * 60 * 1000;   // online this long after BOOT is pressed
constexpr uint32_t kPortalIdleMs = 60 * 1000;       // and this long after the last portal request
constexpr uint32_t kApIdleMs = 10 * 60 * 1000;      // unused setup network gives way to the stored album
constexpr uint32_t kInputQuietMs = 600;             // stay awake this long after a press or touch
constexpr uint32_t kDoubleTapMs = 400;              // a second BOOT tap within this shows the battery

AppConfig gCfg;
bool gHaveShown = false;    // a photo is on screen, so a loading badge makes sense
bool gLoadingShown = false;  // the loading badge is on screen
uint32_t gNextAt = 0;        // millis() when the next photo is due
uint32_t gNoticeUntil = 0;   // the "Web portal online" notice is up until then (0 = not shown)

void showLoading(int step) {
  if (!gHaveShown || gLoadingShown) return;
  gLoadingShown = true;
  ui::loadingBadge(step < 0 ? "Loading previous photo..." : "Loading next photo...");
}

// ---------------------------------------------------------------- inputs ----

struct Button {
  int pin;
  bool down = false;
  bool raw = false;        // last sampled level
  uint32_t rawSince = 0;   // when `raw` last changed
  uint32_t pressedAt = 0;
  bool longFired = false;
};

enum class Press { None, Short, Long };

Button gBtn1{kPinBtn1}, gBtn2{kPinBtn2}, gBtnBoot{kPinBoot};
uint32_t gLastInputAt = 0;  // last time any button or the touch screen was down
uint32_t gBootTapAt = 0;    // a single BOOT tap waiting to see if a second follows (0 = none)

uint32_t since(uint32_t t) { return millis() - t; }

// GPIO36/39 see short low glitches while WiFi switches its radio, so a level
// only counts once it has read the same, three times over, for 30 ms.
bool readPressed(int pin) {
  if (pin < 0) return false;  // button not fitted on this board
  return digitalRead(pin) == LOW && digitalRead(pin) == LOW && digitalRead(pin) == LOW;
}

// Debounced; Short fires on release, Long fires once while still held.
Press pollButton(Button& b, uint32_t longMs) {
  const uint32_t now = millis();
  const bool raw = readPressed(b.pin);
  if (raw) gLastInputAt = now;
  if (raw != b.raw) {
    b.raw = raw;
    b.rawSince = now;
  }
  if (raw != b.down && now - b.rawSince >= 30) {
    b.down = raw;
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
bool gTouchCandidate = false;  // pressed on the previous poll; a tap needs two in a row
bool gTouchCalibrated = false;

// Uncalibrated press check. A bad read on the shared SPI bus gives a pressure
// of 4095 or more (the second pressure sample reads 0) and coordinates at the
// rails; a finger gives neither.
bool rawTouchPressed() {
  const uint16_t z = tft.getTouchRawZ();
  if (z <= 600 || z >= 4095) return false;
  uint16_t x = 0, y = 0;
  tft.getTouchRaw(&x, &y);
  return x > 50 && x < 4045 && y > 50 && y < 4045;
}

// Left third = previous, middle = pause, right third = next. Without a
// calibration any tap means "next".
void pollTouch() {
  const uint32_t now = millis();
  if (now - gLastTouchPoll < 80) return;
  gLastTouchPoll = now;

  uint16_t x = 0, y = 0;
  bool pressed = gTouchCalibrated ? tft.getTouch(&x, &y) : rawTouchPressed();
  if (!pressed) {
    if (gTouchDown) gTouchReleasedAt = now;
    gTouchDown = gTouchCandidate = false;
    return;
  }
  gLastInputAt = now;
  if (!gTouchCandidate) {  // a single noisy reading isn't a tap
    gTouchCandidate = true;
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

// BTN1 = previous, BTN2 = next (holding next pauses), unless swapped in the portal.
void pollInputs() {
  Button& next = gCfg.swapButtons ? gBtn1 : gBtn2;
  Button& prev = gCfg.swapButtons ? gBtn2 : gBtn1;
  switch (pollButton(next, 1200)) {
    case Press::Short: state::post(state::Cmd::Next); break;
    case Press::Long: state::post(state::Cmd::TogglePause); break;
    default: break;
  }
  if (pollButton(prev, 1200) == Press::Short) state::post(state::Cmd::Prev);
  // BOOT: tap to go online, double-tap for the battery, hold to shut down.
  switch (pollButton(gBtnBoot, kShutdownHoldMs)) {
    case Press::Short:
      if (gBootTapAt && since(gBootTapAt) < kDoubleTapMs) {
        gBootTapAt = 0;
        state::post(state::Cmd::ShowBattery);
      } else {
        gBootTapAt = millis() | 1;
      }
      break;
    case Press::Long:
      gBootTapAt = 0;
      state::post(state::Cmd::Shutdown);
      break;
    default: break;
  }
  if (gBootTapAt && !gBtnBoot.down && since(gBootTapAt) >= kDoubleTapMs) {
    gBootTapAt = 0;
    state::post(state::Cmd::GoOnline);
  }
  pollTouch();
}

// While prefetching, only commands that need the screen or change what's
// shown interrupt; "next" simply waits for the photo being prepared. The
// portal waiting for the connection also interrupts, so its preview of the
// photo on screen isn't held up; retryPrefetch() picks the download up again.
bool abortPrefetch() {
  pollInputs();
  if (immich::othersWaiting()) return true;
  state::Cmd c = state::peek();
  if (c == state::Cmd::Next) showLoading(1);  // decoding off-screen, so the panel is free
  return c != state::Cmd::None && c != state::Cmd::Next && c != state::Cmd::TogglePause;
}

// Called between decoded JPEG blocks: lets next/prev interrupt a slow image.
bool abortDecode() {
  pollInputs();
  state::Cmd c = state::peek();
  return c == state::Cmd::Next || c == state::Cmd::Prev || c == state::Cmd::Reload ||
         c == state::Cmd::Reboot || c == state::Cmd::FactoryReset || c == state::Cmd::Shutdown ||
         c == state::Cmd::FormatSd;
}



// ----------------------------------------------------------------- wifi ----

bool gApMode = false;
uint32_t gLastStaAttempt = 0;
uint32_t gApStartedAt = 0;
String gApName;
bool gOffline = false;      // WiFi switched off to save power
uint32_t gOnlineSince = 0;  // when BOOT last brought the frame online
bool gUserOnline = false;   // online because BOOT was pressed (not just to check the album)

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

  // Stop a failed station connection from retrying in the background: its
  // channel hopping knocks phones off the AP. Then scan once, before the AP
  // exists, so the setup page can list networks without another scan.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  const int found = WiFi.scanNetworks();
  Serial.printf("[wifi] %d networks found\n", found);

  WiFi.mode(WIFI_AP_STA);
  bool up = WiFi.softAP(name, pass.c_str());
  gApMode = true;
  gLastStaAttempt = gApStartedAt = millis();
  const String ip = WiFi.softAPIP().toString();
  state::setAp(true, ip);
  Serial.printf("[wifi] setup AP %s %s (pass %s) at %s\n", name, up ? "up" : "FAILED", pass.c_str(), ip.c_str());

  ui::setupScreen(gApName, pass, "http://" + ip, cfg.wifiSsid.length() ? "Could not join " + cfg.wifiSsid : "");
}

// `quiet`: a photo is on screen, so say it in a small notice instead of the splash.
bool connectStation(const AppConfig& cfg, bool quiet = false) {
  if (quiet) {
    ui::loadingBadge("Connecting to " + cfg.wifiSsid + "...");
  } else {
    ui::splash("Connecting to " + cfg.wifiSsid + "...");
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFi.setAutoReconnect(true);
  // No modem sleep: it slows downloads badly, and with an album on the card
  // WiFi is switched off altogether as soon as the photos are stored.
  WiFi.setSleep(false);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiConnectTimeoutMs) {
    setLed((millis() / 250) % 2);
    delay(50);
  }
  setLed(false);
  const bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[wifi] %s %s\n", cfg.wifiSsid.c_str(),
                ok ? ("connected, " + WiFi.localIP().toString()).c_str() : "connect failed");
  return ok;
}

// Repeaters and mesh nodes share one SSID, and once joined the ESP32 never
// leaves its AP. While the signal is weak, look for a clearly stronger one;
// reconnecting then picks it, since joining scans every channel by signal.
uint32_t gLastRoamCheck = 0;
void maybeRoam(const AppConfig& cfg) {
  if (millis() - gLastRoamCheck < kRoamCheckMs) return;
  gLastRoamCheck = millis();
  const int rssi = WiFi.RSSI();
  if (rssi > kRoamWeakRssi) return;
  const int n = WiFi.scanNetworks();
  int best = -1;
  for (int i = 0; i < n; i++)
    if (WiFi.SSID(i) == cfg.wifiSsid && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  if (best >= 0 && WiFi.RSSI(best) >= rssi + kRoamMinGain && WiFi.BSSIDstr(best) != WiFi.BSSIDstr()) {
    Serial.printf("[wifi] roaming from %s (%d dBm) to %s (%d dBm)\n", WiFi.BSSIDstr().c_str(), rssi,
                  WiFi.BSSIDstr(best).c_str(), WiFi.RSSI(best));
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  }
  WiFi.scanDelete();
}

// In setup mode, periodically retry the saved network while nobody is using
// the AP; once it connects, close the AP and start the slideshow.
void serviceAccessPoint(const AppConfig& cfg) {
  setLed((millis() / 500) % 2);
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
    setLed(false);
    gOnlineSince = millis();
    gHaveShown = false;  // the setup screen is up; show a photo straight away
    gNextAt = millis();
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
uint32_t gCfgVersion = 0;
int gFailures = 0;
bool gShowingPattern = false;
String gScreenMessage;  // what's currently on screen if it isn't a photo

void showMessageOnce(const String& title, const String& l1 = "", const String& l2 = "",
                     uint16_t color = TFT_WHITE) {
  String key = title + "|" + l1 + "|" + l2;
  if (key == gScreenMessage) return;
  gScreenMessage = key;
  gLoadingShown = false;
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
  state::setNextReady(false);
}

void resetSlideshow() {
  gQueueLen = gQueuePos = 0;
  gPage = 1;
  gHistory.clear();
  gBack = 0;
  gHaveShown = gLoadingShown = false;
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
  setLed(true);
  state::setLoading(true);
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
  setLed(false);
  state::setLoading(false);
  return ok;
}

void finishShowing(const Asset& a, bool fromBuffer) {
  gHaveShown = true;
  gLoadingShown = false;
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
      state::setNextReady(true);
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

// Resumes a prefetch that stepped aside, once the connection is free.
uint32_t gPrefetchRetryAt = 0;
void retryPrefetch() {
  if (gHaveReady || !gHavePending || immich::othersWaiting() || millis() - gPrefetchRetryAt < 2000) return;
  gPrefetchRetryAt = millis();
  prefetchNext();
}

// step: +1 next, -1 previous, 0 redraw the current photo. `requested`: the
// user asked for it, so say so on screen if the photo still has to download.
void advance(int step, bool requested = false) {
  gShowingPattern = false;

  // Fast path: the next photo is already decoded.
  if (step > 0 && gBack == 0 && gHaveReady && fb::active()) {
    gHaveReady = false;
    state::setNextReady(false);
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
  if (requested && step != 0) showLoading(step);
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
      gLoadingShown = false;
    } else if (!a2) {
      log_w("redraw: %s", redrawErr.c_str());
    }
  } else if (gHistory.empty()) {
    showWaiting();
  }
  // A preview rejected at its header costs little, so try the next one right
  // away: otherwise next/prev presses seem to do nothing on a library with
  // many broken previews. Back off once nothing is working.
  if (fresh && !touched && gFailures < kMaxQuickSkips) return advance(1, requested);
  gNextAt = millis() + (gFailures >= kMaxQuickSkips ? kRetryAfterErrorMs : 1000);
}

// ---------------------------------------------------- album on SD card ----

// Where the slideshow is in the album. Kept in RTC memory, so it carries on
// from the same place after a shutdown.
struct Playlist {
  uint32_t magic;
  uint32_t album;  // crc of the album id
  uint32_t count;  // photos in the album when the order was drawn
  uint32_t pos;    // position in playing order of the photo on screen
  uint32_t seed;   // shuffle of this pass
};
constexpr uint32_t kPlayMagic = 0x504c4131;
RTC_DATA_ATTR Playlist gPlay;

// Seconds on the RTC clock (which keeps counting through deep sleep) of the
// last album check or attempt, for syncHours. Written by both tasks.
RTC_DATA_ATTR volatile uint32_t gLastSyncTime = 0;
void markSynced() { gLastSyncTime = (uint32_t)time(nullptr); }
uint32_t secsSinceSync() { return (uint32_t)time(nullptr) - gLastSyncTime; }
bool syncDue() { return gCfg.syncHours && secsSinceSync() >= gCfg.syncHours * 3600UL; }

// ---- storing task --------------------------------------------------------
//
// A FreeRTOS task on core 1 lists the album and stores its photos on the card
// while the slideshow (loop(), core 1) keeps showing what is already there.
// The slideshow never downloads in this mode; it only reads frames.
//
//   kSyncRun   set by the slideshow while the task may use WiFi and the card
//   kSyncIdle  set by the task whenever it is not mid-download or mid-listing
//
// The slideshow bumps gSyncGen to make the task start over (list the album
// again and retry photos that failed); the task reports which generation it
// has listed and finished.
constexpr EventBits_t kSyncRun = BIT0;
constexpr EventBits_t kSyncIdle = BIT1;
EventGroupHandle_t gSyncEvents;
std::atomic<uint32_t> gSyncGen{1};
std::atomic<uint32_t> gListedGen{0}, gDoneGen{0};
std::atomic<bool> gListChanged{false};  // new album list; the slideshow re-reads it before the task goes on

bool syncRunning() { return xEventGroupGetBits(gSyncEvents) & kSyncRun; }
bool syncListed() { return gListedGen == gSyncGen; }
bool syncComplete() { return gDoneGen == gSyncGen; }
void restartSync() { gSyncGen++; }
void allowSync(bool on) {
  if (on) {
    xEventGroupSetBits(gSyncEvents, kSyncRun);
  } else {
    xEventGroupClearBits(gSyncEvents, kSyncRun);
  }
}

// Stops the task and waits until it has let go of WiFi and the card.
void stopSync() {
  if (!gSyncEvents) return;
  xEventGroupClearBits(gSyncEvents, kSyncRun);
  if (!(xEventGroupWaitBits(gSyncEvents, kSyncIdle, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) & kSyncIdle))
    log_w("storing task did not stop");
}

bool cacheWanted() {
  return cache::mounted() && gCfg.sdCache && gCfg.source == Source::Album && gCfg.sourceId.length() == 36;
}

void checkPlaylist() {
  const uint32_t album = crc32_le(0, (const uint8_t*)gCfg.sourceId.c_str(), gCfg.sourceId.length());
  const uint32_t n = cache::count();
  if (gPlay.magic == kPlayMagic && gPlay.album == album && gPlay.count == n && gPlay.pos < max<uint32_t>(n, 1))
    return;
  gPlay = {kPlayMagic, album, n, 0, esp_random()};
}

// Opens or closes the SD copy to match the settings and the screen size.
void syncCacheState() {
  if (!cacheWanted()) {
    if (cache::isOpen()) {
      stopSync();
      cache::close();
    }
    state::setCache(false, false);
    return;
  }
  if (cache::matches(gCfg.sourceId, tft.width(), tft.height(), gCfg.fill)) return;
  stopSync();
  if (cache::open(gCfg.sourceId, tft.width(), tft.height(), gCfg.fill)) {
    checkPlaylist();
    restartSync();
  }
}

uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

// A random permutation of 0..n-1 that needs no memory: a small Feistel
// network over the next even power of two, walking until it lands below n.
size_t shuffled(size_t i, size_t n, uint32_t seed) {
  if (n < 2) return 0;
  int bits = 2;
  while ((1u << bits) < n) bits++;
  if (bits & 1) bits++;
  const int half = bits / 2;
  const uint32_t mask = (1u << half) - 1;
  uint32_t x = i;
  do {
    uint32_t l = x >> half, r = x & mask;
    for (uint32_t round = 0; round < 4; round++) {
      const uint32_t t = l ^ (mix32(r ^ seed ^ (round * 0x9e3779b9u)) & mask);
      l = r;
      r = t;
    }
    x = (l << half) | r;
  } while (x >= n);
  return x;
}

// Album index (oldest first) of the photo at position k (< n) in playing order.
size_t slotAt(size_t k) {
  const size_t n = cache::count();
  switch (gCfg.order) {
    case Order::Oldest: return k;
    case Order::Newest: return n - 1 - k;
    default: return shuffled(k, n, gPlay.seed);
  }
}

size_t stepPos(size_t k, int dir, size_t n) {
  if (dir > 0) {
    if (++k >= n) {
      k = 0;
      gPlay.seed = esp_random();  // a fresh shuffle each pass
    }
  } else if (dir < 0) {
    k = k == 0 ? n - 1 : k - 1;
  }
  return k;
}

void cacheSkip(size_t idx, const String& err) {
  Asset a;
  cache::asset(idx, a);
  log_w("asset %s: %s", a.id, err.c_str());
  state::setError(String("Skipped ") + a.id + ": " + err);
  state::countSkipped();
  cache::markFailed(idx);  // not retried until the frame is next online
}

// Next photo to store: the first missing one in playing order after the one
// on screen, so the slideshow rarely has to wait. Runs in the storing task;
// gPlay belongs to the slideshow, but only its position is read here.
bool nextMissing(size_t& idx) {
  const size_t n = cache::count();
  if (n == 0) return false;
  size_t k = gPlay.pos % n;
  for (size_t i = 0; i < n; i++) {
    k = k + 1 >= n ? 0 : k + 1;
    const size_t s = slotAt(k);
    if (!cache::has(s) && !cache::failed(s)) {
      idx = s;
      return true;
    }
  }
  return false;
}

// Decoder callback in the storing task: gives way when told to stop or when
// the portal wants the connection. FreeRTOS shares core 1 with the slideshow.
bool abortStoring() {
  // The slideshow is reading a frame: hold the download until it is done.
  while (cache::reading() && syncRunning()) vTaskDelay(pdMS_TO_TICKS(5));
  return !syncRunning() || immich::othersWaiting();
}

// Polled per photo while listing the album: stops only when told to. (The
// portal gets its turn between pages; restarting a long listing for it
// would never finish.)
bool abortListing() {
  while (cache::reading() && syncRunning()) vTaskDelay(pdMS_TO_TICKS(5));
  return !syncRunning();
}

// Idle for a while, available to stopSync() throughout.
void syncRest(uint32_t ms) {
  xEventGroupSetBits(gSyncEvents, kSyncIdle);
  vTaskDelay(pdMS_TO_TICKS(ms));
}

void syncTask(void*) {
  int listFailures = 0;
  for (;;) {
    xEventGroupSetBits(gSyncEvents, kSyncIdle);
    xEventGroupWaitBits(gSyncEvents, kSyncRun, pdFALSE, pdTRUE, portMAX_DELAY);
    xEventGroupClearBits(gSyncEvents, kSyncIdle);
    // stopSync() may have cleared kSyncRun while we woke; it only waits for
    // kSyncIdle, so check again now that we have claimed it.
    if (!syncRunning() || !cache::isOpen()) continue;
    if (gListChanged) {
      syncRest(50);  // the slideshow re-reads the list first
      continue;
    }

    if (cache::reading()) {  // the card is busy showing a photo
      syncRest(20);
      continue;
    }
    const uint32_t gen = gSyncGen;
    if (gListedGen != gen) {
      const AppConfig cfg = config::get();
      String err;
      bool changed = false;
      if (cache::refresh(cfg, changed, err, abortListing)) {
        listFailures = 0;
        cache::clearFailed();  // new session: try them again
        markSynced();
        if (changed) gListChanged = true;
        gListedGen = gen;
      } else if (syncRunning()) {
        state::setError("Loading the album: " + err);
        log_w("album list: %s", err.c_str());
        if (++listFailures >= 3) {  // the server is away; play what's stored and try next time
          listFailures = 0;
          markSynced();
          gListedGen = gen;
          gDoneGen = gen;
        } else {
          syncRest(kRetryAfterErrorMs);
        }
      }
      continue;
    }
    if (gDoneGen == gen) {
      syncRest(500);
      continue;
    }
    if (immich::othersWaiting()) {
      syncRest(200);  // the portal first
      continue;
    }

    size_t idx;
    if (!nextMissing(idx)) {
      Serial.printf("[cache] album stored: %u photos\n", cache::info().cached);
      gDoneGen = gen;
      continue;
    }
    const AppConfig cfg = config::get();
    String err;
    bool aborted = false;
    state::setLoading(true);
    const uint32_t t0 = millis();
    const bool ok = cache::store(cfg, idx, abortStoring, err, aborted);
    state::setLoading(false);
    if (ok) {
      const cache::Info ci = cache::info();
      Serial.printf("[cache] stored %u/%u in %lu ms\n", ci.cached, ci.total, millis() - t0);
    } else if (aborted) {
      syncRest(300);
    } else {
      Serial.printf("[cache] could not store photo %u: %s\n", (unsigned)idx, err.c_str());
      cacheSkip(idx, err);
    }
  }
}

void startSyncTask() {
  gSyncEvents = xEventGroupCreate();
  xEventGroupSetBits(gSyncEvents, kSyncIdle);
  // Stack covers a TLS handshake plus the JSON of an album page.
  // Core 1, next to the slideshow, which sleeps most of the time: the task
  // gets nearly the whole core for decoding, and WiFi and the portal keep
  // core 0 to themselves. (Only core 0's idle task is watchdog-checked.)
  xTaskCreatePinnedToCore(syncTask, "store", 12288, nullptr, 1, nullptr, 1);
}

void showCacheWaiting() {
  if (gHaveShown) return;
  if (gOffline) {
    showMessageOnce("No photos stored yet", "Press BOOT to go online");
  } else {
    showWaiting();
  }
}

// Like advance(), for the album on the card: shows only photos already
// stored. While the rest are still being stored it waits for the next one
// rather than wrapping round to the start.
void cacheAdvance(int step, bool requested = false) {
  gShowingPattern = false;
  const size_t n = cache::count();
  const bool storing = !gOffline && !syncComplete();
  if (n == 0) {
    showCacheWaiting();
    gNextAt = millis() + (storing ? 2000 : kRetryAfterErrorMs);
    return;
  }
  if (gPlay.pos >= n) gPlay.pos = 0;
  size_t k = gPlay.pos;
  int dir = step;
  for (size_t tries = 0; tries < n; tries++) {
    if (dir > 0 && storing && gHaveShown && k + 1 >= n) break;  // the rest are on their way
    if (dir != 0) k = stepPos(k, dir, n);
    dir = dir == 0 ? 1 : dir;  // a redraw falls forward past photos it can't show
    const size_t idx = slotAt(k);
    if (!cache::has(idx)) continue;
    String err;
    // Redraws go straight up; new photos get the transition.
    const fx::Effect effect = step == 0 ? fx::Effect::None : fx::resolveRows(fx::fromName(gCfg.transition));
    if (cache::show(tft, idx, err, effect)) {
      Asset a;
      cache::asset(idx, a);
      gPlay.pos = k;
      gNoticeUntil = 0;
      gScreenMessage = "";
      gHaveShown = true;
      gLoadingShown = false;
      if (gCfg.showDate) ui::caption(ui::formatDate(a.date));
      ui::pausedBadge(gPaused);
      state::setShowing(a.id, a.date);
      gFailures = 0;
      gNextAt = millis() + gCfg.intervalSec * 1000UL;
      return;
    }
    log_w("show %u: %s", (unsigned)idx, err.c_str());
  }
  if (gHaveShown && storing) {
    // Keep the photo on screen and look again shortly.
    if (requested && step != 0) showLoading(step);
    gNextAt = millis() + 2000;
    return;
  }
  showCacheWaiting();
  gNextAt = millis() + (storing ? 2000 : kRetryAfterErrorMs);
}

void redraw() {
  if (cache::isOpen()) {
    cacheAdvance(0);
  } else {
    advance(0);
  }
}

// ------------------------------------------------------------- power ----

constexpr uint32_t kNoticeMs = 6000;

// "Battery 82% - charging", or "" without a battery.
String batteryLine() {
  const power::Battery b = power::measureBattery();
  if (!board::kHasBattery || b.state == power::BatteryState::None || b.percent < 0) return "";
  String s = "Battery " + String(b.percent) + "%";
  if (b.state == power::BatteryState::Charging) s += " - charging";
  else if (b.state == power::BatteryState::Full) s += " - full";
  return s;
}

// Says on screen that the portal is reachable, and where.
void showOnlineBadge() {
  if (!gHaveShown || gApMode || WiFi.status() != WL_CONNECTED) return;
  gLoadingShown = true;  // the next photo paints over it
  ui::notice("Web portal online", "http://" + WiFi.localIP().toString(), "http://" + gCfg.hostname + ".local",
             batteryLine());
  gNoticeUntil = millis() + kNoticeMs;
  if (!gNoticeUntil) gNoticeUntil = 1;
}

void showBatteryNotice() {
  const power::Battery b = power::measureBattery();
  String title, l1, l2;
  if (!board::kHasBattery || b.state == power::BatteryState::None) {
    title = "No battery";
    l1 = "Running on USB power";
  } else {
    title = "Battery " + (b.percent >= 0 ? String(b.percent) + "%" : String("?"));
    l1 = String(b.volts, 2) + " V";
    if (b.state == power::BatteryState::Charging) l2 = "Charging";
    else if (b.state == power::BatteryState::Full) l2 = "Full, on USB power";
    else if (b.state == power::BatteryState::Discharging) l2 = "On battery";
  }
  gLoadingShown = true;  // the next photo paints over it
  ui::notice(title, l1, l2);
  gNoticeUntil = (millis() + kNoticeMs) | 1;
}

// Takes the notice down again: a stored photo redraws in a moment. A
// streamed one would need downloading again, so it waits for the next photo.
void expireNotice() {
  if (!gNoticeUntil || (int32_t)(millis() - gNoticeUntil) < 0) return;
  gNoticeUntil = 0;
  if (!cache::isOpen() || !gHaveShown) return;
  const uint32_t due = gNextAt;
  cacheAdvance(0);
  gNextAt = due;  // a redraw doesn't restart the photo's time
}

void wifiOff() {
  if (WiFi.getMode() == WIFI_OFF) return;
  MDNS.end();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
}

void goOffline(const char* why) {
  if (gOffline || gApMode) return;
  Serial.printf("[wifi] going offline: %s\n", why);
  stopSync();
  wifiOff();
  gOffline = true;
  gUserOnline = false;
  if (!syncListed()) markSynced();  // the next scheduled check counts from now
  state::setAp(false, "");
}

// Brings WiFi back for the portal and an album check. `user`: BOOT was
// pressed, so open the setup network if the saved one can't be joined.
void goOnline(bool user) {
  if (user) {
    gOnlineSince = millis();
    gUserOnline = true;
  }
  if (!gOffline) {
    if (user) showOnlineBadge();  // already online: just say where
    return;
  }
  Serial.printf("[wifi] going online (%s)\n", user ? "BOOT" : "album check");
  const bool ok = gCfg.wifiSsid.length() && connectStation(gCfg, gHaveShown);
  if (ok) {
    gOffline = false;
    restartSync();
    state::setAp(false, WiFi.localIP().toString());
    startMdns(gCfg);
    showOnlineBadge();
    return;
  }
  if (user || cache::info().cached == 0) {
    gOffline = false;
    startAccessPoint(gCfg);
    portal::setApMode(true);
    return;
  }
  wifiOff();  // try again at the next scheduled check
  markSynced();
  if (gHaveShown) cacheAdvance(0);  // clear the "Connecting" notice
}

// Back to the stored album once it is complete and the portal has gone quiet.
void maybeGoOffline() {
  if (cache::info().cached == 0 || portal::setupActive()) return;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !syncComplete()) return;  // still storing
  if (gUserOnline && since(gOnlineSince) < kOnlineIdleMs) return;  // BOOT was pressed to use the portal
  if (portal::lastActivity() && since(portal::lastActivity()) < kPortalIdleMs) return;  // in use
  goOffline(connected ? "album stored" : "WiFi lost");
}

// The setup network was opened (BOOT with the saved WiFi out of reach) but
// nobody joined it: give up and play the stored album.
void maybeLeaveAp() {
  if (!cache::isOpen() || cache::info().cached == 0 || portal::setupActive()) return;
  if (WiFi.softAPgetStationNum() > 0) gApStartedAt = millis();
  if (min(since(gApStartedAt), since(portal::lastActivity())) < kApIdleMs) return;
  Serial.println("[wifi] setup network unused; back to the stored album");
  WiFi.softAPdisconnect(true);
  gApMode = false;
  portal::setApMode(false);
  wifiOff();
  gOffline = true;
  markSynced();
  state::setAp(false, "");
  gHaveShown = false;
  gNextAt = millis();
}

bool inputBusy() {
  return since(gLastInputAt) < kInputQuietMs || gBootTapAt || gBtnBoot.down || gBtnBoot.raw || gBtn1.down || gBtn2.down ||
         gTouchDown || gTouchCandidate || state::peek() != state::Cmd::None;
}

// ------------------------------------------------------- power saver ----

// Kept through deep sleep: when the next photo is due (RTC clock, ms) and
// whether the slideshow is paused.
RTC_DATA_ATTR int64_t gNextDueMs = 0;
RTC_DATA_ATTR bool gSaverPaused = false;

int64_t wallMs() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool cacheUsable() { return cache::isOpen() && cache::info().cached > 0; }

// Deep sleep with the photo on screen until the next photo, the next album
// check, BOOT or a touch.
[[noreturn]] void sleepUntilNextPhoto() {
  uint32_t ms = 0;  // 0 = no timer
  if (!gPaused) ms = max<int32_t>((int32_t)(gNextAt - millis()), 50);
  if (gCfg.syncHours) {
    const uint32_t syncSecs = gCfg.syncHours * 3600UL, done = secsSinceSync();
    const uint32_t toSync = done < syncSecs ? (syncSecs - done) * 1000 : 50;
    ms = ms ? min(ms, toSync) : toSync;
  }
  gNextDueMs = wallMs() + (int32_t)(gNextAt - millis());
  gSaverPaused = gPaused;
  stopSync();
  wifiOff();
  power::saverSleep(ms, true);
}

// Offline in the loop: once nothing needs the chip awake, sleep.
void offlineIdle() {
  if (syncDue()) return goOnline(false);
  if (inputBusy() || gNoticeUntil) {  // a press being read, or a notice up
    delay(10);
    return;
  }
  if (!gPaused && (int32_t)(gNextAt - millis()) <= 0) return;  // the loop shows the next photo first
  sleepUntilNextPhoto();
}

// BOOT pressed while online: WiFi off now and power saver from here on.
void enterSaver() {
  if (!cacheUsable()) {
    showOnlineBadge();  // nothing stored to play offline: stay online
    return;
  }
  if (gApMode) {
    WiFi.softAPdisconnect(true);
    gApMode = false;
    portal::setApMode(false);
  }
  goOffline("power saver (BOOT)");
  gOffline = true;
  if (gHaveShown) {
    ui::notice("Power saver", "WiFi off - press BOOT to go online");
    delay(1500);
    const uint32_t due = gNextAt;
    cacheAdvance(0);
    gNextAt = due;
  } else {
    gNextAt = millis();
  }
}

[[noreturn]] void powerOff() {
  Serial.println("[power] shutting down");
  stopSync();
  wifiOff();
  cache::close();
  tft.fillScreen(TFT_BLACK);
  tft.writecommand(0x28);  // DISPOFF
  tft.writecommand(0x10);  // SLPIN: the panel drops to microamps
  delay(120);
  power::shutdown();
}

[[noreturn]] void shutdownNow() {
  gScreenMessage = "";
  ui::message("Shutting down", "Press BOOT to start again");
  while (digitalRead(kPinBoot) == LOW) delay(10);  // a held button would wake it at once
  delay(300);
  powerOff();
}

// ---- battery protection ----
constexpr int kLowBatteryPct = 5;
RTC_DATA_ATTR uint8_t gLowStrikes = 0;  // low readings in a row (WiFi bursts dip the cell briefly)
uint32_t gLastBatteryCheck = 0;

// Nearly flat and not on the charger.
bool batteryCritical(int& pct) {
  if (!board::kHasBattery) return false;
  const power::Battery b = power::measureBattery();
  pct = b.percent;
  return b.percent >= 0 && b.percent < kLowBatteryPct && b.state != power::BatteryState::Charging &&
         b.state != power::BatteryState::Full && b.state != power::BatteryState::None;
}

// The charge-me screen for a few seconds, then off. BOOT then shows it
// again until the charger is connected.
[[noreturn]] void lowBatteryOff(int pct) {
  Serial.printf("[power] battery at %d%%: shutting down\n", pct);
  gLowStrikes = 0;
  stopSync();
  wifiOff();
  ui::lowBattery(pct);
  delay(3000);
  powerOff();
}

// `strikes`: low readings in a row needed before shutting down.
void checkBattery(int strikes) {
  int pct = -1;
  if (!batteryCritical(pct)) {
    gLowStrikes = 0;
    return;
  }
  log_w("battery low: %d%% (%u)", pct, gLowStrikes + 1);
  if (++gLowStrikes >= strikes) lowBatteryOff(pct);
}

// ---- woken from power saver ----

// What a touch did, read straight after waking (the finger is often still down).
void touchWake() {
  uint16_t x = 0, y = 0;
  bool located = false;
  for (const uint32_t t0 = millis(); since(t0) < 250; delay(15)) {
    if (gTouchCalibrated ? tft.getTouch(&x, &y) : rawTouchPressed()) {
      located = gTouchCalibrated;
      break;
    }
  }
  if (located && x < tft.width() / 3) {
    cacheAdvance(-1, true);
  } else if (located && x <= tft.width() * 2 / 3) {
    gPaused = !gPaused;
    ui::pausedBadge(gPaused);
    if (!gPaused) gNextAt = millis() + 1500;
  } else {
    cacheAdvance(1, true);
  }
  // Wait for the finger to lift, so the touch doesn't wake us straight away.
  for (const uint32_t t0 = millis(); since(t0) < 2000 && digitalRead(board::kPinTouchIrq) == LOW;) delay(10);
}

// Woken from power saver: the photo is still on screen and WiFi is off. Does
// what woke the frame and sleeps again. Returns only to go online: true for
// a BOOT press, false for the scheduled album check.
bool saverWakeup(power::Boot boot) {
  gOffline = true;
  gHaveShown = true;
  gPaused = gSaverPaused;
  gNextAt = millis() + (uint32_t)max<int64_t>(0, gNextDueMs - wallMs());

  if (boot == power::Boot::SaverButton) {
    // Hold: shut down. Double tap: battery. Single tap: online.
    for (const uint32_t t0 = millis(); digitalRead(kPinBoot) == LOW; delay(10))
      if (since(t0) >= kShutdownHoldMs) shutdownNow();
    bool second = false;
    for (const uint32_t t0 = millis(); since(t0) < kDoubleTapMs && !second; delay(5))
      second = digitalRead(kPinBoot) == LOW;
    if (!second) return true;
    while (digitalRead(kPinBoot) == LOW) delay(10);
    showBatteryNotice();
    delay(kNoticeMs);
    const uint32_t due = gNextAt;
    cacheAdvance(0);
    gNextAt = due;
  } else if (boot == power::Boot::SaverTouch) {
    touchWake();
  }

  if (syncDue()) return false;
  if (!gPaused && (int32_t)(millis() - gNextAt) >= 0) cacheAdvance(1);
  sleepUntilNextPhoto();
}

// Loop body while the album plays from the SD card.
void cacheLoop() {
  const bool connected = !gOffline && WiFi.status() == WL_CONNECTED;
  allowSync(connected);
  state::setCache(true, connected && !syncComplete());
  if (gListChanged) {  // the storing task has a new album list
    checkPlaylist();
    gListChanged = false;
  }
  expireNotice();

  if (!gPaused && (int32_t)(millis() - gNextAt) >= 0) {
    cacheAdvance(gHaveShown ? 1 : 0);
  } else if (gOffline) {
    offlineIdle();
    return;
  } else {
    maybeGoOffline();
    if (connected && syncComplete()) maybeRoam(gCfg);  // a scan would stall a download
  }
  delay(gOffline ? 0 : 10);
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
  redraw();
}

void handleCommands() {
  for (state::Cmd cmd = state::take(); cmd != state::Cmd::None; cmd = state::take()) {
    switch (cmd) {
      case state::Cmd::Next:
        if (gApMode) break;
        if (cache::isOpen()) {
          cacheAdvance(1, true);
        } else {
          advance(1, true);
        }
        break;
      case state::Cmd::Prev:
        if (gApMode) break;
        if (cache::isOpen()) {
          cacheAdvance(-1, true);
        } else {
          advance(-1, true);
        }
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
        restartSync();  // and check the album again
        break;
      case state::Cmd::GoOnline:
        if (gOffline) {
          goOnline(true);
        } else {
          enterSaver();  // online already: BOOT turns the power saver on
        }
        break;
      case state::Cmd::ShowBattery:
        showBatteryNotice();
        break;
      case state::Cmd::GoOffline:
        if (cache::isOpen() && cache::info().cached > 0) {
          delay(500);  // let the HTTP response go out
          goOffline("asked in the portal");
        } else {
          state::setError("Nothing stored on the SD card yet, so the frame stays online");
        }
        break;
      case state::Cmd::Shutdown:
        delay(500);
        shutdownNow();
        break;
      case state::Cmd::FormatSd: {
        delay(500);
        gScreenMessage = "";
        ui::message("Formatting SD card", "This takes a few seconds");
        stopSync();
        const bool ok = cache::format();
        if (!ok) state::setError("Formatting the SD card failed");
        resetSlideshow();
        syncCacheState();
        break;
      }
      case state::Cmd::TestPattern:
        ui::testPattern();
        gLoadingShown = false;
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
      case state::Cmd::SetupMode: {
        // One-shot flag: next boot opens the setup AP, settings are kept.
        Preferences prefs;
        prefs.begin("boot", false);
        prefs.putBool("setup", true);
        prefs.end();
        ui::message("Restarting...", "into setup mode");
        delay(800);
        ESP.restart();
        break;
      }
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
                             next.apiKey != gCfg.apiKey || next.sdCache != gCfg.sdCache;
  const bool lookChanged = next.fill != gCfg.fill || next.showDate != gCfg.showDate;
  const bool rotated = next.rotation != gCfg.rotation;
  const bool colorChanged = next.colorBgr != gCfg.colorBgr || next.colorInvert != gCfg.colorInvert;
  const bool intervalChanged = next.intervalSec != gCfg.intervalSec;
  const String prevTransition = gCfg.transition;
  gCfg = next;

  power::setBacklight(gCfg.backlight);

  const bool transitionChanged = next.transition != prevTransition;
  if (rotated || lookChanged || transitionChanged) dropReady(true);  // decoded for the old settings
  if (rotated || !wantBuffer()) fb::end();
  if (rotated || colorChanged) {
    ui::applyPanel(gCfg.rotation, gCfg.colorBgr, gCfg.colorInvert);
    if (rotated) gTouchCalibrated = false;
    gScreenMessage = "";
  }
  syncCacheState();  // album, scaling or screen size may have changed
  if (gShowingPattern && (rotated || colorChanged)) {
    ui::testPattern();  // keep the pattern up while colours are being tuned
  } else if (sourceChanged) {
    resetSlideshow();
  } else if (rotated || colorChanged || lookChanged) {
    redraw();
  } else if (intervalChanged) {
    gNextAt = millis() + gCfg.intervalSec * 1000UL;
  }
}

}  // namespace

void setup() {
  power::releaseSleepHolds();
  const power::Boot wake = power::bootReason();
  const bool saver = power::saverWake(wake);
  Serial.begin(115200);
  static const char* const kWhy[] = {"", " (woken by BOOT)", " (saver: next photo)", " (saver: BOOT)",
                                     " (saver: touch)"};
  Serial.printf("[boot] Immich Frame %s on %s%s\n", FW_VERSION, board::kName, kWhy[(int)wake]);

  pinMode(board::kPinSdCs, OUTPUT);
  digitalWrite(board::kPinSdCs, HIGH);
  if (board::kHasButtons) {
    pinMode(kPinBtn1, INPUT);  // GPIO36/39 are input-only with no internal pull-ups
    pinMode(kPinBtn2, INPUT);
  }
  pinMode(kPinBoot, INPUT_PULLUP);
  if (board::kPinTouchIrq >= 0) pinMode(board::kPinTouchIrq, INPUT);
  // The press that woke the frame from shutdown is not a "go online" press.
  if (power::wokeFromShutdown()) {
    while (digitalRead(kPinBoot) == LOW) delay(10);
    delay(50);
  }

  config::begin();
  state::begin();
  immich::begin();
  gCfg = config::get();
  gCfgVersion = config::version();

  if (saver) {
    ui::attach(gCfg.rotation, gCfg.colorBgr, gCfg.colorInvert);  // the photo stays up
  } else {
    ui::begin(gCfg.rotation, gCfg.colorBgr, gCfg.colorInvert);
  }
  power::begin(gCfg.backlight);
  if (gCfg.touchCalValid) {
    tft.setTouch(gCfg.touchCal);
    gTouchCalibrated = true;
  }
  // Too flat to run: the charge-me screen, then off. A saver wake-up wants a
  // second low reading first; a start (WiFi not on yet) reads true at once.
  checkBattery(saver ? 2 : 1);

  // Holding both buttons while powering up wipes all settings.
  if (!saver && board::kHasButtons && digitalRead(kPinBtn1) == LOW && digitalRead(kPinBtn2) == LOW) {
    ui::message("Factory reset?", "Keep holding both buttons for 3 s", "", "", TFT_RED);
    uint32_t start = millis();
    while (digitalRead(kPinBtn1) == LOW && digitalRead(kPinBtn2) == LOW) {
      if (millis() - start > 3000) {
        config::erase();
        ui::message("Settings erased", "Release the buttons");
        while (digitalRead(kPinBtn1) == LOW || digitalRead(kPinBtn2) == LOW) delay(20);
        ESP.restart();
      }
      delay(20);
    }
  }

  WiFi.persistent(false);
  // Default fast scan joins the first AP heard with the SSID, not the strongest.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  Preferences boot;
  boot.begin("boot", false);
  const bool forceSetup = boot.getBool("setup", false);
  if (forceSetup) boot.remove("setup");
  boot.end();

  startSyncTask();
  // The frame claims its card the first time it sees it.
  const cache::Card card = cache::begin();
  if (card == cache::Card::NeedsFormat) {
    ui::splash("Preparing the SD card...");
    if (!cache::format()) state::setError("Could not format the SD card");
  } else if (card == cache::Card::Unreadable) {
    // Its photos may still be there: never erase it on our own.
    state::setError("SD card can't be read. It was not erased; use Power > Format SD card to start it afresh");
  }
  syncCacheState();
  gOnlineSince = millis();

  if (saver && cacheUsable()) {
    const bool user = saverWakeup(wake);  // sleeps again unless going online
    power::takeBacklight();
    goOnline(user);
    portal::begin(gApMode);
    return;
  }
  if (saver) power::takeBacklight();  // card gone: start normally

  bool startOffline = false;
  if (!forceSetup && cache::isOpen() && cache::info().cached > 0) {
    // Straight from the card, then online in the background of the photo.
    cacheAdvance(0);
    if (gCfg.wifiSsid.length() && connectStation(gCfg, true)) {
      state::setAp(false, WiFi.localIP().toString());
      startMdns(gCfg);
      showOnlineBadge();
    } else {
      startOffline = true;
    }
  } else if (!forceSetup && gCfg.wifiSsid.length() && connectStation(gCfg)) {
    state::setAp(false, WiFi.localIP().toString());
    startMdns(gCfg);
    ui::splashStatus("Connected - " + WiFi.localIP().toString(), TFT_GREEN);
    delay(1500);
    ui::splashStatus("Loading photos...");  // the first photo replaces the splash
  } else {
    startAccessPoint(gCfg);
  }

  portal::begin(gApMode);
  if (startOffline) {
    goOffline("saved WiFi not reachable");
    markSynced();
    if (gHaveShown) cacheAdvance(0);  // clear the "Connecting" notice
  }
}

void loop() {
  pollInputs();
  handleCommands();
  applyConfigChanges();
  if (since(gLastBatteryCheck) >= 30000) {
    gLastBatteryCheck = millis();
    checkBattery(3);
  }
  // A card left mid-write by a reset may not answer at start-up: try again
  // for the first two minutes, and pick it up if it is ours (never
  // formatting it here). Each try blocks for a moment, so not for ever.
  static uint32_t lastCardTry = 0;
  if (board::kHasSd && !cache::mounted() && millis() < 120000 && since(lastCardTry) >= 15000 && !gApMode) {
    lastCardTry = millis();
    if (cache::begin() == cache::Card::Ready) {
      Serial.println("[cache] SD card found");
      resetSlideshow();
      syncCacheState();
    }
  }

  if (gApMode) {
    serviceAccessPoint(gCfg);
    maybeLeaveAp();
    delay(10);
    return;
  }
  if (cache::isOpen()) return cacheLoop();
  if (gOffline) return goOnline(false);  // no stored album to play any more

  if (gCfg.serverUrl.isEmpty() || gCfg.apiKey.isEmpty()) {
    showMessageOnce("Almost there", "Open http://" + WiFi.localIP().toString(), "to set Immich server + key",
                    TFT_YELLOW);
  } else if (WiFi.status() != WL_CONNECTED) {
    if (gHistory.empty()) showMessageOnce("WiFi disconnected", "Reconnecting to " + gCfg.wifiSsid);
  } else if (!gPaused && (int32_t)(millis() - gNextAt) >= 0) {
    advance(1);
  } else {
    retryPrefetch();
    maybeRoam(gCfg);
  }
  delay(10);
}
