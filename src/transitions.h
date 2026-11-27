#pragma once

#include <TFT_eSPI.h>

#include <functional>

// Reveals the photo held in fb:: over whatever is on screen. The panel can't
// be read back, so effects that blend with the old image (cross-fades) aren't
// possible; these all uncover or slide in the new one.
namespace fx {

enum class Effect : uint8_t {
  None,  // draw directly while decoding, no buffer
  WipeLeft,
  WipeRight,
  WipeDown,
  WipeUp,
  SlideLeft,
  SlideRight,
  SlideDown,
  SlideUp,
  Blinds,
  Dissolve,
  BoxOut,
  Diagonal,
  Split,      // opens from the middle row outwards
  Curtain,    // closes in from the top and bottom edges
  Scanlines,  // rows in random order
  Count
};

// "random" maps to Effect::Count; resolve() then picks a real one.
Effect fromName(const String& name);
const char* name(Effect e);
Effect resolve(Effect e);

// Runs the effect over about `durationMs`.
void run(TFT_eSPI& tft, Effect e, uint32_t durationMs = 700);

// Row effects need only whole rows of the new image, in any order, so they
// also work from a source that can't be read by columns (a frame on the SD
// card). `push(y, rows)` draws that many full-width rows of the new image.
using RowPush = std::function<bool(int y, int rows)>;
bool isRowEffect(Effect e);
// Like resolve(), but only ever a row effect; None stays None.
Effect resolveRows(Effect e);
bool runRows(Effect e, int height, const RowPush& push, uint32_t durationMs = 700);

}  // namespace fx
