#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

namespace jpeg {

// Polled between decoded blocks; return true to stop drawing early.
using AbortFn = bool (*)();

// Decodes a baseline JPEG straight from `in` (no full-image buffer) and draws
// it centred on the screen, scaled to fit (letterbox) or fill (crop).
// `len` is the stream length in bytes, or -1 when unknown.
// Returns false with `err` set on failure; `aborted` is set if AbortFn fired.
bool draw(TFT_eSPI& tft, Stream& in, int len, bool fill, AbortFn abort, String& err, bool& aborted);

}  // namespace jpeg
