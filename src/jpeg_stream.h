#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

namespace jpeg {

// Polled between decoded blocks; return true to stop drawing early.
using AbortFn = bool (*)();

// Decodes a baseline or extended-sequential JPEG straight from `in` (no full-image buffer) and draws
// it centred on the screen, scaled to fit (letterbox) or fill (crop).
// `len` is the stream length in bytes, or -1 when unknown.
// Returns false with `err` set on failure; `aborted` is set if AbortFn fired.
// `touched` reports whether anything was drawn: a JPEG rejected at the header
// leaves the screen as it was.
bool draw(TFT_eSPI& tft, Stream& in, int len, bool fill, AbortFn abort, String& err, bool& aborted,
          bool& touched);

}  // namespace jpeg
