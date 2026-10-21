#include "transitions.h"

#include "frame_buffer.h"

namespace fx {
namespace {

const char* const kNames[] = {"none",       "wipe-left",  "wipe-right", "wipe-down", "wipe-up",
                              "slide-left", "slide-right", "slide-down", "slide-up", "blinds",
                              "dissolve",   "box-out",    "diagonal"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == (size_t)Effect::Count, "names out of sync");

// Spreads `steps` frames evenly over the duration.
struct Pacer {
  uint32_t start, duration;
  int steps;
  void wait(int i) const {
    const uint32_t due = start + duration * (i + 1) / steps;
    while ((int32_t)(millis() - due) < 0) delay(1);
  }
};

float easeOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

void wipe(TFT_eSPI& tft, Effect e, const Pacer& p) {
  const int W = fb::width(), H = fb::height();
  const bool horizontal = e == Effect::WipeLeft || e == Effect::WipeRight;
  const int span = horizontal ? W : H;
  int prev = 0;
  for (int i = 0; i < p.steps; i++) {
    const int cur = span * (i + 1) / p.steps;
    const int a = prev, n = cur - prev;
    switch (e) {
      case Effect::WipeRight: fb::blit(tft, a, 0, n, H); break;          // reveals left to right
      case Effect::WipeLeft: fb::blit(tft, W - cur, 0, n, H); break;     // right to left
      case Effect::WipeDown: fb::blit(tft, 0, a, W, n); break;           // top to bottom
      default: fb::blit(tft, 0, H - cur, W, n); break;                   // bottom to top
    }
    prev = cur;
    p.wait(i);
  }
}

// The new photo slides in over the old one from one edge.
void slide(TFT_eSPI& tft, Effect e, const Pacer& p) {
  const int W = fb::width(), H = fb::height();
  for (int i = 0; i < p.steps; i++) {
    const float t = easeOut((float)(i + 1) / p.steps);
    const int ox = lroundf(W * t), oy = lroundf(H * t);
    switch (e) {
      case Effect::SlideLeft: fb::blit(tft, 0, 0, ox, H, W - ox, 0); break;   // enters from the right
      case Effect::SlideRight: fb::blit(tft, W - ox, 0, ox, H, 0, 0); break;  // from the left
      case Effect::SlideDown: fb::blit(tft, 0, H - oy, W, oy, 0, 0); break;   // from the top
      default: fb::blit(tft, 0, 0, W, oy, 0, H - oy); break;                  // from the bottom
    }
    p.wait(i);
  }
}

void blinds(TFT_eSPI& tft, const Pacer& p) {
  const int W = fb::width(), H = fb::height();
  const int slat = p.steps;
  for (int i = 0; i < slat; i++) {
    for (int y = i; y < H; y += slat) fb::blit(tft, 0, y, W, 1);
    p.wait(i);
  }
}

void dissolve(TFT_eSPI& tft, const Pacer& p) {
  constexpr int kBlock = 16;
  const int W = fb::width(), H = fb::height();
  const int cols = (W + kBlock - 1) / kBlock, rows = (H + kBlock - 1) / kBlock;
  const int n = cols * rows;
  uint16_t order[(320 / kBlock) * (320 / kBlock)];
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = n - 1; i > 0; i--) std::swap(order[i], order[esp_random() % (i + 1)]);

  int done = 0;
  for (int s = 0; s < p.steps; s++) {
    const int until = n * (s + 1) / p.steps;
    for (; done < until; done++) {
      const int b = order[done];
      fb::blit(tft, (b % cols) * kBlock, (b / cols) * kBlock, kBlock, kBlock);
    }
    p.wait(s);
  }
}

// A rectangle grows from the centre; only the newly uncovered ring is drawn.
void boxOut(TFT_eSPI& tft, const Pacer& p) {
  const int W = fb::width(), H = fb::height();
  const int cx = W / 2, cy = H / 2;
  int l = cx, r = cx, t = cy, b = cy;  // previous rect [l, r) x [t, b)
  for (int i = 0; i < p.steps; i++) {
    const float k = easeOut((float)(i + 1) / p.steps);
    const int hw = lroundf(cx * k), hh = lroundf(cy * k);
    const int nl = cx - hw, nr = (i == p.steps - 1) ? W : cx + hw;
    const int nt = cy - hh, nb = (i == p.steps - 1) ? H : cy + hh;
    fb::blit(tft, nl, nt, nr - nl, t - nt);  // top band
    fb::blit(tft, nl, b, nr - nl, nb - b);   // bottom band
    fb::blit(tft, nl, t, l - nl, b - t);     // left strip
    fb::blit(tft, r, t, nr - r, b - t);      // right strip
    l = nl, r = nr, t = nt, b = nb;
    p.wait(i);
  }
}

// A diagonal edge sweeps from the top-left corner to the bottom-right.
void diagonal(TFT_eSPI& tft, const Pacer& p) {
  const int W = fb::width(), H = fb::height();
  const int span = W + W;  // threshold runs over x + y*W/H in [0, 2W)
  int prev = 0;
  for (int i = 0; i < p.steps; i++) {
    const int cur = span * (i + 1) / p.steps;
    for (int y = 0; y < H; y++) {
      const int shift = y * W / H;
      const int xs = max(0, prev - shift), xe = min(W, cur - shift);
      if (xe > xs) fb::blit(tft, xs, y, xe - xs, 1);
    }
    prev = cur;
    p.wait(i);
  }
}

}  // namespace

Effect fromName(const String& name) {
  for (int i = 0; i < (int)Effect::Count; i++)
    if (name == kNames[i]) return (Effect)i;
  return Effect::Count;  // "random" and anything unknown
}

const char* name(Effect e) { return e < Effect::Count ? kNames[(int)e] : "random"; }

Effect resolve(Effect e) {
  if (e != Effect::Count) return e;
  return (Effect)(1 + esp_random() % ((int)Effect::Count - 1));
}

void run(TFT_eSPI& tft, Effect e, uint32_t durationMs) {
  if (!fb::active()) return;
  const bool swap = tft.getSwapBytes();
  tft.setSwapBytes(false);  // the buffer is already in panel byte order

  int steps = 24;
  if (e >= Effect::SlideLeft && e <= Effect::SlideUp) steps = 14;  // each frame redraws more
  if (e == Effect::Blinds) steps = 16;
  Pacer p{millis(), durationMs, steps};

  switch (e) {
    case Effect::WipeLeft:
    case Effect::WipeRight:
    case Effect::WipeDown:
    case Effect::WipeUp: wipe(tft, e, p); break;
    case Effect::SlideLeft:
    case Effect::SlideRight:
    case Effect::SlideDown:
    case Effect::SlideUp: slide(tft, e, p); break;
    case Effect::Blinds: blinds(tft, p); break;
    case Effect::Dissolve: dissolve(tft, p); break;
    case Effect::BoxOut: boxOut(tft, p); break;
    case Effect::Diagonal: diagonal(tft, p); break;
    default: fb::blit(tft, 0, 0, fb::width(), fb::height()); break;
  }
  tft.setSwapBytes(swap);
}

}  // namespace fx
