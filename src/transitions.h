#pragma once

#include <TFT_eSPI.h>

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
  Count
};

// "random" maps to Effect::Count; resolve() then picks a real one.
Effect fromName(const String& name);
const char* name(Effect e);
Effect resolve(Effect e);

// Runs the effect over about `durationMs`.
void run(TFT_eSPI& tft, Effect e, uint32_t durationMs = 700);

}  // namespace fx
