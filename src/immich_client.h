#pragma once

#include <Arduino.h>

#include <functional>

#include "app_config.h"

struct Asset {
  char id[37];    // UUID
  char date[11];  // YYYY-MM-DD from localDateTime, may be empty
};

namespace immich {

void begin();

// Checks the server is reachable and the API key is valid.
// On success `info` holds e.g. "Immich 3.2.2 - signed in as Jane".
bool testConnection(const AppConfig& cfg, String& info);

// Fetches the next batch of image assets for the configured source and order.
// `page` is the 1-based cursor for ordered modes; it is advanced (and wrapped)
// in place. Random mode ignores it.
bool fetchBatch(const AppConfig& cfg, int& page, Asset* out, size_t max, size_t& count, String& err);

// Lists albums, people or tags as a JSON array of {id, name, count?}.
bool listSources(const AppConfig& cfg, Source type, String& json, String& err);

// Receives the preview JPEG stream; `len` is the Content-Length or -1.
using ImageSink = std::function<bool(Stream& in, int len, String& err)>;

// Downloads the asset's preview rendition and hands the body to `sink`.
bool fetchPreview(const AppConfig& cfg, const char* assetId, const ImageSink& sink, String& err);

// Receives the small thumbnail rendition (WebP or JPEG, as Immich is set up);
// `type` is its Content-Type.
using ThumbSink = std::function<bool(Stream& in, int len, const String& type, String& err)>;

// Downloads the asset's thumbnail rendition (~250 px) for the web portal.
bool fetchThumbnail(const AppConfig& cfg, const char* assetId, const ThumbSink& sink, String& err);

}  // namespace immich
