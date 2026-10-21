#pragma once

#include <Arduino.h>

// What the slideshow pulls images from.
enum class Source : uint8_t { All, Album, Person, Tag, Favorites };
// Order in which assets are shown.
enum class Order : uint8_t { Random, Newest, Oldest };

struct AppConfig {
  // Network
  String wifiSsid;
  String wifiPass;
  String hostname = "immich-frame";

  // Immich
  String serverUrl;  // e.g. http://192.168.1.134:2283 (no trailing /api)
  String apiKey;
  bool insecureTls = true;  // skip certificate validation for https servers

  // Slideshow
  Source source = Source::All;
  String sourceId;    // album / person / tag UUID
  String sourceName;  // human readable, for the status page and screen
  Order order = Order::Random;
  uint16_t intervalSec = 30;
  bool fill = false;  // false = fit (letterbox), true = fill (crop)
  bool showDate = false;
  uint8_t rotation = 0;  // TFT_eSPI rotation; 0/2 portrait, 1/3 landscape

  // Panel colour fixes: identical-looking ST7789 modules differ in these.
  bool colorBgr = true;      // panel expects blue-green-red (true for this board's ST7789)
  bool colorInvert = false;  // panel needs display inversion

  // Portal
  String adminPass;  // optional HTTP basic auth password (user "admin")

  // Touch calibration from TFT_eSPI::calibrateTouch()
  bool touchCalValid = false;
  uint16_t touchCal[5] = {0, 0, 0, 0, 0};
};

namespace config {

void begin();

// Thread-safe snapshot of the current config.
AppConfig get();

// Replace and persist the config. Bumps version() so the slideshow can react.
void set(const AppConfig& cfg);

// Incremented on every set(); compare against a cached value to detect changes.
uint32_t version();

// Erase everything stored in NVS (factory reset).
void erase();

const char* sourceToStr(Source s);
Source sourceFromStr(const String& s);
const char* orderToStr(Order o);
Order orderFromStr(const String& s);

// Strip trailing slashes and a trailing "/api" from a user-entered URL.
String normalizeUrl(String url);

}  // namespace config
