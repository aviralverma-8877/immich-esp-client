#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include <functional>

namespace jpeg {

// Polled between decoded blocks; return true to stop drawing early.
using AbortFn = bool (*)();

// Decodes a baseline or extended-sequential JPEG straight from `in` (no full-image buffer) and draws
// it centred on the screen, scaled to fit (letterbox) or fill (crop).
// `len` is the stream length in bytes, or -1 when unknown.
// Returns false with `err` set on failure; `aborted` is set if AbortFn fired.
// `touched` reports whether anything was drawn: a JPEG rejected at the header
// leaves the screen as it was.
// With `toBuffer`, pixels go into fb:: (which must be active) and the screen
// is never touched; the caller reveals the result with a transition.
bool draw(TFT_eSPI& tft, Stream& in, int len, bool fill, AbortFn abort, String& err, bool& aborted,
          bool& touched, bool toBuffer = false);

// Receives `rows` full-width rows starting at `y`, top to bottom, every row of
// the screen exactly once (letterbox bars as black). Pixels are RGB565 in
// panel byte order, ready for pushImage() with swapBytes off.
using RowSink = std::function<bool(int y, int rows, const uint16_t* px)>;

// Like draw(), but scales to a scrW x scrH frame handed to `sink` instead of
// the panel. Needs about scrW * 40 bytes of heap for one row of JPEG blocks.
bool decode(Stream& in, int len, int scrW, int scrH, bool fill, AbortFn abort, const RowSink& sink, String& err,
            bool& aborted);

}  // namespace jpeg
