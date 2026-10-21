#include "portal.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "app_config.h"
#include "app_state.h"
#include "captive_dns.h"
#include "immich_client.h"
#include "portal_html.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace portal {
namespace {

WebServer server(80);
CaptiveDns dns;
volatile bool gApMode = false;
bool gDnsRunning = false;

// First-run setup: credentials are saved, then tried while the AP stays up so
// the setup page can report the result before the device restarts.
enum class SetupState : uint8_t { Idle, Connecting, Connected, Failed };
constexpr uint32_t kSetupConnectTimeoutMs = 20000;
constexpr uint32_t kSetupRebootDelayMs = 30000;
volatile SetupState gSetup = SetupState::Idle;
uint32_t gSetupStartedAt = 0;
uint32_t gSetupRebootAt = 0;
String gSetupSsid, gSetupIp, gSetupError, gImmichResult;
bool gImmichOk = false;

String apUrl() { return "http://" + WiFi.softAPIP().toString() + "/"; }

void redirectToPortal() {
  server.sendHeader("Location", apUrl(), true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

// Requests for some other host name (captive-portal checks, or any site the
// user typed) are bounced to the AP address so the OS shows its login sheet.
bool foreignHost() {
  if (!gApMode) return false;
  const String host = server.hostHeader();
  return host.length() && host != WiFi.softAPIP().toString();
}

// Build with -DCAPTIVE_DEBUG to trace what phones probe during setup.
void logRequest() {
#ifdef CAPTIVE_DEBUG
  Serial.printf("[http] %s %s%s\n", server.method() == HTTP_GET ? "GET" : "POST", server.hostHeader().c_str(),
                server.uri().c_str());
#endif
}

bool authorized() {
  // The setup AP has its own random WPA2 password shown only on the screen,
  // and phone captive-portal sheets can't show a basic-auth prompt.
  if (gApMode) return true;
  AppConfig cfg = config::get();
  if (cfg.adminPass.isEmpty() || server.authenticate("admin", cfg.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Immich Frame");
  return false;
}

void sendJson(int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

void sendError(int code, const String& msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  sendJson(code, doc);
}

// Immich ids are UUIDs: 36 characters of hex digits and dashes.
bool isUuid(const String& s) {
  if (s.length() != 36) return false;
  for (size_t i = 0; i < s.length(); i++) {
    const char ch = s[i];
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? ch != '-' : !isxdigit((unsigned char)ch)) return false;
  }
  return true;
}

bool parseBody(JsonDocument& doc) {
  if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
    sendError(400, "Invalid JSON");
    return false;
  }
  return true;
}

void handleRoot() {
  logRequest();
  if (foreignHost()) return redirectToPortal();
  if (!authorized()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", gApMode ? kSetupHtml : kPortalHtml);
}

void handleSettings() {
  if (!authorized()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", kPortalHtml);
}

void handleSetupGet() {
  if (!authorized()) return;
  AppConfig c = config::get();
  JsonDocument d;
  static const char* kStates[] = {"idle", "connecting", "connected", "failed"};
  d["state"] = kStates[(int)gSetup];
  d["apMode"] = (bool)gApMode;
  d["ssid"] = gSetupSsid.length() ? gSetupSsid : c.wifiSsid;
  d["ip"] = gSetupIp;
  d["error"] = gSetupError;
  d["hostname"] = c.hostname;
  d["serverUrl"] = c.serverUrl;
  d["apiKeySet"] = !c.apiKey.isEmpty();
  d["immichOk"] = gImmichOk;
  d["immich"] = gImmichResult;
  if (gSetup == SetupState::Connected) d["rebootIn"] = (int32_t)(gSetupRebootAt - millis()) / 1000;
  sendJson(200, d);
}

void handleSetupPost() {
  if (!authorized()) return;
  if (!gApMode) return sendError(409, "Already connected; use the settings page");
  if (gSetup == SetupState::Connecting) return sendError(409, "Already connecting");
  JsonDocument in;
  if (!parseBody(in)) return;
  const String ssid = in["ssid"] | "";
  const String pass = in["pass"] | "";
  if (ssid.isEmpty()) return sendError(400, "Choose a WiFi network");
  if (pass.length() && pass.length() < 8) return sendError(400, "WiFi passwords are at least 8 characters");

  AppConfig c = config::get();
  c.wifiSsid = ssid;
  c.wifiPass = pass;  // empty is valid for open networks
  if (in["serverUrl"].is<const char*>() && strlen(in["serverUrl"]))
    c.serverUrl = config::normalizeUrl(in["serverUrl"].as<String>());
  if (in["apiKey"].is<const char*>() && strlen(in["apiKey"])) c.apiKey = in["apiKey"].as<String>();
    c.apiKey.trim();
  config::set(c);

  gSetupSsid = ssid;
  gSetupIp = gSetupError = gImmichResult = "";
  gImmichOk = false;
  gSetupStartedAt = millis();
  gSetup = SetupState::Connecting;
  WiFi.disconnect(false, false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[setup] trying %s\n", ssid.c_str());

  JsonDocument out;
  out["ok"] = true;
  sendJson(200, out);
}

void handleSetupFinish() {
  if (!authorized()) return;
  JsonDocument out;
  out["ok"] = gSetup == SetupState::Connected;
  sendJson(200, out);
  if (gSetup == SetupState::Connected) gSetupRebootAt = millis() + 1500;
}

// Advances the setup attempt; runs in the portal task between requests.
void serviceSetup() {
  if (gSetup == SetupState::Connecting) {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      gSetupIp = WiFi.localIP().toString();
      Serial.printf("[setup] connected to %s as %s\n", gSetupSsid.c_str(), gSetupIp.c_str());
      // Check Immich from the new network while the phone is still attached.
      AppConfig c = config::get();
      if (c.serverUrl.length() && c.apiKey.length()) {
        gImmichOk = immich::testConnection(c, gImmichResult);
      } else {
        gImmichResult = "Server or API key not set yet - add them in settings later";
      }
      gSetupRebootAt = millis() + kSetupRebootDelayMs;
      gSetup = SetupState::Connected;
    } else if (millis() - gSetupStartedAt > kSetupConnectTimeoutMs) {
      gSetupError = st == WL_NO_SSID_AVAIL ? "Network not found - is it in range and 2.4 GHz?"
                                           : "Could not connect - check the password";
      Serial.printf("[setup] %s: %s (status %d)\n", gSetupSsid.c_str(), gSetupError.c_str(), st);
      WiFi.disconnect(false, false);  // stop channel-hopping retries that upset the AP
      gSetup = SetupState::Failed;
    }
  } else if (gSetup == SetupState::Connected && (int32_t)(millis() - gSetupRebootAt) >= 0) {
    gSetupRebootAt = millis() + 60000;  // only post once
    state::post(state::Cmd::Reboot);
  }
}

void handleGetConfig() {
  if (!authorized()) return;
  AppConfig c = config::get();
  JsonDocument d;
  d["wifiSsid"] = c.wifiSsid;
  d["wifiPassSet"] = !c.wifiPass.isEmpty();
  d["hostname"] = c.hostname;
  d["serverUrl"] = c.serverUrl;
  d["apiKeySet"] = !c.apiKey.isEmpty();
  d["insecureTls"] = c.insecureTls;
  d["source"] = config::sourceToStr(c.source);
  d["sourceId"] = c.sourceId;
  d["sourceName"] = c.sourceName;
  d["order"] = config::orderToStr(c.order);
  d["intervalSec"] = c.intervalSec;
  d["fill"] = c.fill;
  d["showDate"] = c.showDate;
  d["rotation"] = c.rotation;
  d["transition"] = c.transition;
  d["colorBgr"] = c.colorBgr;
  d["colorInvert"] = c.colorInvert;
  d["adminPassSet"] = !c.adminPass.isEmpty();
  d["touchCalValid"] = c.touchCalValid;
  sendJson(200, d);
}

void handlePostConfig() {
  if (!authorized()) return;
  JsonDocument in;
  if (!parseBody(in)) return;

  AppConfig c = config::get();
  const String oldSsid = c.wifiSsid, oldPass = c.wifiPass, oldHost = c.hostname;
  const uint8_t oldRotation = c.rotation;

  if (in["wifiSsid"].is<const char*>()) c.wifiSsid = in["wifiSsid"].as<String>();
  // Secrets: an empty string means "keep the stored value".
  if (in["wifiPass"].is<const char*>() && strlen(in["wifiPass"])) c.wifiPass = in["wifiPass"].as<String>();
  if (in["apiKey"].is<const char*>() && strlen(in["apiKey"])) c.apiKey = in["apiKey"].as<String>();
    c.apiKey.trim();
  if (in["adminPass"].is<const char*>() && strlen(in["adminPass"])) c.adminPass = in["adminPass"].as<String>();
  if (in["clearAdminPass"] | false) c.adminPass = "";
  if (in["hostname"].is<const char*>() && strlen(in["hostname"])) c.hostname = in["hostname"].as<String>();
  if (in["serverUrl"].is<const char*>()) c.serverUrl = config::normalizeUrl(in["serverUrl"].as<String>());
  if (in["insecureTls"].is<bool>()) c.insecureTls = in["insecureTls"];
  if (in["source"].is<const char*>()) c.source = config::sourceFromStr(in["source"].as<String>());
  if (in["sourceId"].is<const char*>()) c.sourceId = in["sourceId"].as<String>();
  if (in["sourceName"].is<const char*>()) c.sourceName = in["sourceName"].as<String>();
  if (in["order"].is<const char*>()) c.order = config::orderFromStr(in["order"].as<String>());
  if (in["intervalSec"].is<int>()) c.intervalSec = constrain(in["intervalSec"].as<int>(), 5, 86400);
  if (in["fill"].is<bool>()) c.fill = in["fill"];
  if (in["showDate"].is<bool>()) c.showDate = in["showDate"];
  if (in["rotation"].is<int>()) c.rotation = constrain(in["rotation"].as<int>(), 0, 3);
  if (in["transition"].is<const char*>()) c.transition = in["transition"].as<String>();
  if (in["colorBgr"].is<bool>()) c.colorBgr = in["colorBgr"];
  if (in["colorInvert"].is<bool>()) c.colorInvert = in["colorInvert"];
  if (c.rotation != oldRotation) c.touchCalValid = false;  // calibration is per orientation

  if (c.source == Source::All || c.source == Source::Favorites) {
    c.sourceId = "";
    c.sourceName = "";
  } else if ((in["source"].is<const char*>() || in["sourceId"].is<const char*>()) && !isUuid(c.sourceId)) {
    return sendError(400, "Pick an album, person or tag from the list (wait for it to load)");
  }

  config::set(c);

  const bool reboot = c.wifiSsid != oldSsid || c.wifiPass != oldPass || c.hostname != oldHost || gApMode;
  JsonDocument out;
  out["ok"] = true;
  out["reboot"] = reboot;
  sendJson(200, out);
  if (reboot) state::post(state::Cmd::Reboot);
}

void handleStatus() {
  if (!authorized()) return;
  state::Status s = state::get();
  AppConfig c = config::get();
  JsonDocument d;
  d["version"] = FW_VERSION;
  d["apMode"] = s.apMode;
  d["ip"] = s.ip;
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["paused"] = s.paused;
  d["assetId"] = s.assetId;
  d["assetDate"] = s.assetDate;
  d["lastError"] = s.lastError;
  if (s.lastError.length()) d["lastErrorAgo"] = (millis() - s.lastErrorAt) / 1000;
  d["shown"] = s.shown;
  d["skipped"] = s.skipped;
  d["loading"] = s.loading;
  d["nextReady"] = s.nextReady;
  d["serverUrl"] = c.serverUrl;
  d["uptime"] = millis() / 1000;
  d["freeHeap"] = ESP.getFreeHeap();
  d["minFreeHeap"] = ESP.getMinFreeHeap();
  sendJson(200, d);
}

// Asynchronous so the AP keeps serving while the radio scans. The first call
// starts a scan and returns {"scanning":true}; poll until results arrive.
void handleScan() {
  if (!authorized()) return;
  JsonDocument d;
  if (gSetup == SetupState::Connecting) {
    d["scanning"] = true;
    return sendJson(200, d);
  }
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED || (n >= 0 && server.hasArg("refresh"))) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    d["scanning"] = true;
    return sendJson(200, d);
  }
  if (n == WIFI_SCAN_RUNNING) {
    d["scanning"] = true;
    return sendJson(200, d);
  }
  JsonArray arr = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).isEmpty()) continue;
    // Keep the strongest entry per SSID (mesh systems advertise several).
    bool dup = false;
    for (JsonObject o : arr) {
      if (o["ssid"] == WiFi.SSID(i)) {
        if (WiFi.RSSI(i) > (int)o["rssi"]) o["rssi"] = WiFi.RSSI(i);
        dup = true;
        break;
      }
    }
    if (dup) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  sendJson(200, d);
}

void handleTest() {
  if (!authorized()) return;
  JsonDocument in;
  if (!parseBody(in)) return;
  AppConfig c = config::get();
  if (in["serverUrl"].is<const char*>() && strlen(in["serverUrl"]))
    c.serverUrl = config::normalizeUrl(in["serverUrl"].as<String>());
  if (in["apiKey"].is<const char*>() && strlen(in["apiKey"])) c.apiKey = in["apiKey"].as<String>();
    c.apiKey.trim();
  if (in["insecureTls"].is<bool>()) c.insecureTls = in["insecureTls"];

  if (WiFi.status() != WL_CONNECTED) return sendError(200, "Not connected to WiFi yet - save WiFi settings first");
  String info;
  bool ok = immich::testConnection(c, info);
  JsonDocument out;
  out["ok"] = ok;
  out[ok ? "info" : "error"] = info;
  sendJson(200, out);
}

void handleSources() {
  if (!authorized()) return;
  Source type = config::sourceFromStr(server.arg("type"));
  if (type != Source::Album && type != Source::Person && type != Source::Tag)
    return sendError(400, "type must be album, person or tag");
  if (WiFi.status() != WL_CONNECTED) return sendError(503, "Not connected to WiFi");
  String json, err;
  if (!immich::listSources(config::get(), type, json, err)) return sendError(502, err);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (!authorized()) return;
  const String c = server.arg("c");
  state::Cmd cmd = state::Cmd::None;
  if (c == "next") cmd = state::Cmd::Next;
  else if (c == "prev") cmd = state::Cmd::Prev;
  else if (c == "pause") cmd = state::Cmd::TogglePause;
  else if (c == "reload") cmd = state::Cmd::Reload;
  else if (c == "pattern") cmd = state::Cmd::TestPattern;
  else if (c == "calibrate") cmd = state::Cmd::CalibrateTouch;
  else if (c == "reboot") cmd = state::Cmd::Reboot;
  else if (c == "factory") cmd = state::Cmd::FactoryReset;
  else if (c == "setup") cmd = state::Cmd::SetupMode;
  if (cmd == state::Cmd::None) return sendError(400, "Unknown command");
  const state::Status s = state::get();  // before the command changes it
  state::post(cmd);
  JsonDocument out;
  out["ok"] = true;
  out["nextReady"] = s.nextReady;
  out["loading"] = s.loading;
  out["assetId"] = s.assetId;
  sendJson(200, out);
}

void handleUpdateDone() {
  if (!authorized()) return;
  JsonDocument out;
  out["ok"] = !Update.hasError();
  if (Update.hasError()) out["error"] = Update.errorString();
  sendJson(200, out);
  if (!Update.hasError()) state::post(state::Cmd::Reboot);
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    AppConfig cfg = config::get();
    if (!cfg.adminPass.isEmpty() && !server.authenticate("admin", cfg.adminPass.c_str())) return;
    log_i("OTA start: %s", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.isRunning() && !Update.end(true)) Update.printError(Serial);
    log_i("OTA done: %u bytes", up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

void handleNotFound() {
  logRequest();
  // In setup mode every unknown URL, including OS connectivity probes, lands
  // on the setup page.
  if (gApMode) return redirectToPortal();
  server.send(404, "text/plain", "Not found");
}

// Connectivity-check URLs. Answering anything other than the expected
// "internet is fine" reply makes the OS pop up its captive-portal browser.
void handleProbe() {
  logRequest();
  if (gApMode && gSetup != SetupState::Connected) return redirectToPortal();
  // Outside setup (or once done) give the expected answers so phones stay quiet.
  const String uri = server.uri();
  if (uri.indexOf("204") >= 0) return server.send(204);
  if (uri.indexOf("hotspot") >= 0 || uri.indexOf("success.html") >= 0)
    return server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  if (uri.indexOf("connecttest") >= 0) return server.send(200, "text/plain", "Microsoft Connect Test");
  if (uri.indexOf("ncsi") >= 0) return server.send(200, "text/plain", "Microsoft NCSI");
  server.send(200, "text/plain", "success\n");
}

void task(void*) {
  for (;;) {
    if (gApMode && !gDnsRunning) {
      dns.start(WiFi.softAPIP());
      gDnsRunning = true;
    } else if (!gApMode && gDnsRunning) {
      dns.stop();
      gDnsRunning = false;
    }
    if (gDnsRunning) dns.process();
    server.handleClient();
    serviceSetup();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

}  // namespace

void begin(bool apMode) {
  gApMode = apMode;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/api/setup", HTTP_GET, handleSetupGet);
  server.on("/api/setup", HTTP_POST, handleSetupPost);
  server.on("/api/setup/finish", HTTP_POST, handleSetupFinish);
  for (const char* probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                            "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt"})
    server.on(probe, handleProbe);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/sources", HTTP_GET, handleSources);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();

  // Core 0, next to the WiFi stack; the slideshow runs in loop() on core 1.
  // Stack covers a TLS handshake plus JSON parsing in /api/sources.
  xTaskCreatePinnedToCore(task, "portal", 12288, nullptr, 1, nullptr, 0);
}

void setApMode(bool apMode) { gApMode = apMode; }

bool setupActive() { return gSetup == SetupState::Connecting || gSetup == SetupState::Connected; }

}  // namespace portal
