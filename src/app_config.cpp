#include "app_config.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace config {
namespace {

constexpr const char* kNamespace = "frame";
constexpr const char* kKey = "cfg";

SemaphoreHandle_t gMutex;
AppConfig gCfg;
volatile uint32_t gVersion = 1;

void toJson(const AppConfig& c, JsonDocument& d) {
  d["wifiSsid"] = c.wifiSsid;
  d["wifiPass"] = c.wifiPass;
  d["hostname"] = c.hostname;
  d["serverUrl"] = c.serverUrl;
  d["apiKey"] = c.apiKey;
  d["insecureTls"] = c.insecureTls;
  d["source"] = sourceToStr(c.source);
  d["sourceId"] = c.sourceId;
  d["sourceName"] = c.sourceName;
  d["order"] = orderToStr(c.order);
  d["intervalSec"] = c.intervalSec;
  d["fill"] = c.fill;
  d["showDate"] = c.showDate;
  d["rotation"] = c.rotation;
  d["transition"] = c.transition;
  d["colorBgr"] = c.colorBgr;
  d["colorInvert"] = c.colorInvert;
  d["adminPass"] = c.adminPass;
  d["touchCalValid"] = c.touchCalValid;
  JsonArray cal = d["touchCal"].to<JsonArray>();
  for (uint16_t v : c.touchCal) cal.add(v);
}

void fromJson(const JsonDocument& d, AppConfig& c) {
  c.wifiSsid = d["wifiSsid"] | c.wifiSsid;
  c.wifiPass = d["wifiPass"] | c.wifiPass;
  c.hostname = d["hostname"] | c.hostname;
  c.serverUrl = d["serverUrl"] | c.serverUrl;
  c.apiKey = d["apiKey"] | c.apiKey;
  c.insecureTls = d["insecureTls"] | c.insecureTls;
  c.source = sourceFromStr(d["source"] | sourceToStr(c.source));
  c.sourceId = d["sourceId"] | c.sourceId;
  c.sourceName = d["sourceName"] | c.sourceName;
  c.order = orderFromStr(d["order"] | orderToStr(c.order));
  c.intervalSec = d["intervalSec"] | c.intervalSec;
  c.fill = d["fill"] | c.fill;
  c.showDate = d["showDate"] | c.showDate;
  c.rotation = d["rotation"] | c.rotation;
  c.transition = d["transition"] | c.transition;
  c.colorBgr = d["colorBgr"] | c.colorBgr;
  c.colorInvert = d["colorInvert"] | c.colorInvert;
  c.adminPass = d["adminPass"] | c.adminPass;
  c.touchCalValid = d["touchCalValid"] | c.touchCalValid;
  JsonArrayConst cal = d["touchCal"];
  for (size_t i = 0; i < 5 && i < cal.size(); i++) c.touchCal[i] = cal[i];
}

void applyDefaults(AppConfig& c) {
#ifdef SECRET_WIFI_SSID
  c.wifiSsid = SECRET_WIFI_SSID;
#endif
#ifdef SECRET_WIFI_PASS
  c.wifiPass = SECRET_WIFI_PASS;
#endif
#ifdef SECRET_IMMICH_URL
  c.serverUrl = SECRET_IMMICH_URL;
#endif
#ifdef SECRET_IMMICH_API_KEY
  c.apiKey = SECRET_IMMICH_API_KEY;
#endif
}

}  // namespace

void begin() {
  gMutex = xSemaphoreCreateMutex();
  applyDefaults(gCfg);

  Preferences prefs;
  prefs.begin(kNamespace, true);
  String raw = prefs.getString(kKey, "");
  prefs.end();

  if (raw.length()) {
    JsonDocument d;
    if (deserializeJson(d, raw) == DeserializationError::Ok) fromJson(d, gCfg);
  }
  gCfg.serverUrl = normalizeUrl(gCfg.serverUrl);
  if (gCfg.intervalSec < 5) gCfg.intervalSec = 5;
  if (gCfg.rotation > 3) gCfg.rotation = 0;
}

AppConfig get() {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  AppConfig copy = gCfg;
  xSemaphoreGive(gMutex);
  return copy;
}

void set(const AppConfig& cfg) {
  JsonDocument d;
  toJson(cfg, d);
  String raw;
  serializeJson(d, raw);

  xSemaphoreTake(gMutex, portMAX_DELAY);
  gCfg = cfg;
  gCfg.serverUrl = normalizeUrl(gCfg.serverUrl);
  gVersion = gVersion + 1;
  xSemaphoreGive(gMutex);

  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.putString(kKey, raw);
  prefs.end();
}

uint32_t version() { return gVersion; }

void erase() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.clear();
  prefs.end();
}

const char* sourceToStr(Source s) {
  switch (s) {
    case Source::Album: return "album";
    case Source::Person: return "person";
    case Source::Tag: return "tag";
    case Source::Favorites: return "favorites";
    default: return "all";
  }
}

Source sourceFromStr(const String& s) {
  if (s == "album") return Source::Album;
  if (s == "person") return Source::Person;
  if (s == "tag") return Source::Tag;
  if (s == "favorites") return Source::Favorites;
  return Source::All;
}

const char* orderToStr(Order o) {
  switch (o) {
    case Order::Newest: return "newest";
    case Order::Oldest: return "oldest";
    default: return "random";
  }
}

Order orderFromStr(const String& s) {
  if (s == "newest") return Order::Newest;
  if (s == "oldest") return Order::Oldest;
  return Order::Random;
}

String normalizeUrl(String url) {
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (url.endsWith("/api")) url.remove(url.length() - 4);
  while (url.endsWith("/")) url.remove(url.length() - 1);
  if (url.length() && !url.startsWith("http://") && !url.startsWith("https://")) url = "http://" + url;
  return url;
}

}  // namespace config
