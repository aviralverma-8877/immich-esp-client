#include "immich_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <memory>
#include <set>

namespace immich {
namespace {

// Only one Immich request at a time: two TLS sessions would not fit in the
// heap of a WROOM-32 without PSRAM, and the portal and slideshow share it.
SemaphoreHandle_t gNetMutex;

class NetLock {
 public:
  NetLock() { locked_ = xSemaphoreTake(gNetMutex, pdMS_TO_TICKS(30000)) == pdTRUE; }
  ~NetLock() {
    if (locked_) xSemaphoreGive(gNetMutex);
  }
  bool ok() const { return locked_; }

 private:
  bool locked_;
};

// Holds the transport and the HTTP client. The client is declared after the
// transport so it is destroyed first.
struct Request {
  std::unique_ptr<WiFiClient> transport;
  HTTPClient http;
};

bool open(Request& req, const AppConfig& cfg, const String& path, String& err) {
  if (cfg.serverUrl.isEmpty()) {
    err = "Immich server URL is not set";
    return false;
  }
  if (cfg.serverUrl.startsWith("https://")) {
    auto* secure = new WiFiClientSecure();
    if (cfg.insecureTls) secure->setInsecure();
    req.transport.reset(secure);
  } else {
    req.transport.reset(new WiFiClient());
  }

  // HTTP/1.0 disables chunked encoding so bodies can be streamed directly
  // into the JSON parser and the JPEG decoder.
  req.http.useHTTP10(true);
  req.http.setReuse(false);
  req.http.setConnectTimeout(8000);
  req.http.setTimeout(15000);
  req.http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!req.http.begin(*req.transport, cfg.serverUrl + "/api" + path)) {
    err = "Invalid server URL";
    return false;
  }
  req.http.addHeader("x-api-key", cfg.apiKey);
  return true;
}

// Builds a readable error from an HTTP status and Immich's JSON error body.
String describeError(Request& req, int code) {
  if (code < 0) return "Connection failed: " + HTTPClient::errorToString(code);
  String msg = "HTTP " + String(code);
  JsonDocument filter;
  filter["message"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, req.http.getStream(), DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
    // Validation errors come back as an array of messages.
    if (doc["message"].is<JsonArray>()) {
      msg += ": " + doc["message"][0].as<String>();
    } else if (doc["message"].is<const char*>()) {
      msg += ": " + doc["message"].as<String>();
    }
  }
  if (code == 401) msg += " (check the API key)";
  if (code == 403) msg += " (API key lacks a required permission)";
  return msg;
}

bool getJson(const AppConfig& cfg, const String& path, const JsonDocument& filter, JsonDocument& doc, String& err) {
  Request req;
  if (!open(req, cfg, path, err)) return false;
  req.http.addHeader("Accept", "application/json");
  int code = req.http.GET();
  if (code != HTTP_CODE_OK) {
    err = describeError(req, code);
    return false;
  }
  DeserializationError de = deserializeJson(doc, req.http.getStream(), DeserializationOption::Filter(filter));
  if (de) {
    err = String("Bad JSON from server: ") + de.c_str();
    return false;
  }
  return true;
}

bool postJson(const AppConfig& cfg, const String& path, const JsonDocument& body, const JsonDocument& filter,
              JsonDocument& doc, String& err) {
  Request req;
  if (!open(req, cfg, path, err)) return false;
  req.http.addHeader("Accept", "application/json");
  req.http.addHeader("Content-Type", "application/json");
  String payload;
  serializeJson(body, payload);
  int code = req.http.POST(payload);
  if (code != HTTP_CODE_OK && code != HTTP_CODE_CREATED) {
    err = describeError(req, code);
    return false;
  }
  DeserializationError de = deserializeJson(doc, req.http.getStream(), DeserializationOption::Filter(filter));
  if (de) {
    err = String("Bad JSON from server: ") + de.c_str();
    return false;
  }
  return true;
}

void copyAsset(JsonObjectConst src, Asset& dst) {
  strlcpy(dst.id, src["id"] | "", sizeof(dst.id));
  const char* when = src["localDateTime"] | "";
  strlcpy(dst.date, when, sizeof(dst.date));
}

void addSourceFilter(const AppConfig& cfg, JsonDocument& body) {
  body["type"] = "IMAGE";
  switch (cfg.source) {
    case Source::Album: body["albumIds"].add(cfg.sourceId); break;
    case Source::Person: body["personIds"].add(cfg.sourceId); break;
    case Source::Tag: body["tagIds"].add(cfg.sourceId); break;
    case Source::Favorites: body["isFavorite"] = true; break;
    case Source::All: break;
  }
}

bool fetchRandom(const AppConfig& cfg, Asset* out, size_t max, size_t& count, String& err) {
  JsonDocument body;
  body["size"] = max;
  addSourceFilter(cfg, body);

  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["localDateTime"] = true;

  JsonDocument doc;
  if (!postJson(cfg, "/search/random", body, filter, doc, err)) return false;
  for (JsonObjectConst a : doc.as<JsonArrayConst>()) {
    if (count >= max) break;
    copyAsset(a, out[count++]);
  }
  return true;
}

bool fetchOrdered(const AppConfig& cfg, int& page, Asset* out, size_t max, size_t& count, String& err) {
  for (int attempt = 0; attempt < 2; attempt++) {
    JsonDocument body;
    body["page"] = page;
    body["size"] = max;
    body["order"] = cfg.order == Order::Oldest ? "asc" : "desc";
    addSourceFilter(cfg, body);

    JsonDocument filter;
    filter["assets"]["items"][0]["id"] = true;
    filter["assets"]["items"][0]["localDateTime"] = true;
    filter["assets"]["nextPage"] = true;

    JsonDocument doc;
    if (!postJson(cfg, "/search/metadata", body, filter, doc, err)) return false;
    for (JsonObjectConst a : doc["assets"]["items"].as<JsonArrayConst>()) {
      if (count >= max) break;
      copyAsset(a, out[count++]);
    }
    // nextPage is a numeric string, or null on the last page.
    const char* next = doc["assets"]["nextPage"] | (const char*)nullptr;
    int nextPage = next ? atoi(next) : 1;
    bool wrapped = page > 1 && count == 0;
    page = nextPage > 0 ? nextPage : 1;
    if (!wrapped) break;  // retry once from page 1 when we ran off the end
  }
  return true;
}

}  // namespace

// On an auth failure, say which key was sent so a stale saved key is obvious.
void noteKey(const AppConfig& cfg, String& err) {
  if (err.indexOf("HTTP 401") < 0) return;
  if (cfg.apiKey.isEmpty()) {
    err += " - no API key is saved";
  } else {
    err += " - the key in use starts with \"" + cfg.apiKey.substring(0, 4) + "\" (" + String(cfg.apiKey.length()) +
           " characters)";
  }
}

void begin() { gNetMutex = xSemaphoreCreateMutex(); }

bool testConnection(const AppConfig& cfg, String& info) {
  NetLock lock;
  if (!lock.ok()) {
    info = "Busy, try again";
    return false;
  }

  JsonDocument filter;
  filter["major"] = true;
  filter["minor"] = true;
  filter["patch"] = true;
  JsonDocument ver;
  if (!getJson(cfg, "/server/version", filter, ver, info)) return false;

  JsonDocument meFilter;
  meFilter["name"] = true;
  meFilter["email"] = true;
  JsonDocument me;
  if (!getJson(cfg, "/users/me", meFilter, me, info)) {
    noteKey(cfg, info);
    return false;
  }

  String who = me["name"] | "";
  if (who.isEmpty()) who = me["email"] | "?";
  info = "Immich " + String(ver["major"] | 0) + "." + String(ver["minor"] | 0) + "." + String(ver["patch"] | 0) +
         " - signed in as " + who;
  return true;
}

bool fetchBatch(const AppConfig& cfg, int& page, Asset* out, size_t max, size_t& count, String& err) {
  count = 0;
  if (cfg.source != Source::All && cfg.source != Source::Favorites && cfg.sourceId.isEmpty()) {
    err = "No album, person or tag selected";
    return false;
  }
  NetLock lock;
  if (!lock.ok()) {
    err = "Network busy";
    return false;
  }
  const bool ok = cfg.order == Order::Random ? fetchRandom(cfg, out, max, count, err)
                                            : fetchOrdered(cfg, page, out, max, count, err);
  if (!ok) noteKey(cfg, err);
  return ok;
}

bool listSources(const AppConfig& cfg, Source type, String& json, String& err) {
  NetLock lock;
  if (!lock.ok()) {
    err = "Busy, try again";
    return false;
  }

  JsonDocument out;
  JsonArray list = out.to<JsonArray>();
  constexpr size_t kMaxEntries = 800;

  if (type == Source::Album) {
    // Owned albums, then albums shared with the user; dedupe by id.
    std::set<String> seen;
    for (const char* path : {"/albums", "/albums?shared=true"}) {
      JsonDocument filter;
      filter[0]["id"] = true;
      filter[0]["albumName"] = true;
      filter[0]["assetCount"] = true;
      JsonDocument doc;
      if (!getJson(cfg, path, filter, doc, err)) return false;
      for (JsonObjectConst a : doc.as<JsonArrayConst>()) {
        String id = a["id"] | "";
        if (id.isEmpty() || !seen.insert(id).second || list.size() >= kMaxEntries) continue;
        JsonObject e = list.add<JsonObject>();
        e["id"] = id;
        e["name"] = String(a["albumName"] | "(untitled)");  // copy: doc dies before serialisation
        e["count"] = a["assetCount"] | 0;
      }
    }
  } else if (type == Source::Person) {
    for (int page = 1; page <= 4 && list.size() < kMaxEntries; page++) {
      JsonDocument filter;
      filter["people"][0]["id"] = true;
      filter["people"][0]["name"] = true;
      filter["hasNextPage"] = true;
      JsonDocument doc;
      if (!getJson(cfg, "/people?withHidden=false&size=500&page=" + String(page), filter, doc, err)) return false;
      for (JsonObjectConst p : doc["people"].as<JsonArrayConst>()) {
        const char* name = p["name"] | "";
        if (!*name || list.size() >= kMaxEntries) continue;  // unnamed faces aren't useful in a picker
        JsonObject e = list.add<JsonObject>();
        e["id"] = String(p["id"] | "");
        e["name"] = String(name);
      }
      if (!(doc["hasNextPage"] | false)) break;
    }
  } else if (type == Source::Tag) {
    JsonDocument filter;
    filter[0]["id"] = true;
    filter[0]["value"] = true;
    JsonDocument doc;
    if (!getJson(cfg, "/tags", filter, doc, err)) return false;
    for (JsonObjectConst t : doc.as<JsonArrayConst>()) {
      if (list.size() >= kMaxEntries) break;
      JsonObject e = list.add<JsonObject>();
      e["id"] = String(t["id"] | "");
      e["name"] = String(t["value"] | "");
    }
  }

  json.clear();
  serializeJson(out, json);
  return true;
}

bool fetchPreview(const AppConfig& cfg, const char* assetId, const ImageSink& sink, String& err) {
  NetLock lock;
  if (!lock.ok()) {
    err = "Network busy";
    return false;
  }
  Request req;
  if (!open(req, cfg, String("/assets/") + assetId + "/thumbnail?size=preview", err)) return false;
  req.http.addHeader("Accept", "image/jpeg");
  const char* headers[] = {"Content-Type"};
  req.http.collectHeaders(headers, 1);

  int code = req.http.GET();
  if (code != HTTP_CODE_OK) {
    err = describeError(req, code);
    return false;
  }
  String type = req.http.header("Content-Type");
  if (!type.startsWith("image/jpeg")) {
    err = "Preview is " + type + "; set Immich > Administration > Settings > Image > Preview format to JPEG";
    return false;
  }
  return sink(*req.http.getStreamPtr(), req.http.getSize(), err);
}

}  // namespace immich
