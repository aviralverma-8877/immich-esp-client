#pragma once

#include <TFT_eSPI.h>

// Off-screen copy of one full screen, used to decode the next photo while the
// current one stays visible and then reveal it with a transition.
//
// 240x320 RGB565 is 150 KB, about all the byte-addressable heap a WROOM-32
// has free. So the buffer is split into small bands allocated as 32-bit
// memory, which ESP-IDF serves from the otherwise idle IRAM heap first. IRAM
// only allows aligned 32-bit access, so every read and write here goes
// through whole words. Pixels are stored byte-swapped, ready for pushImage()
// with swapBytes off.
namespace fb {

constexpr int kBandRows = 10;

// Allocates a buffer for a w x h screen, keeping at least `reserve` bytes of
// byte-addressable heap free for networking. Returns false (holding nothing)
// if that isn't possible; callers then draw straight to the panel.
bool begin(int w, int h, size_t reserve = 30 * 1024);
void end();
bool active();

int width();
int height();

// n pixels of row y starting at x0.
void putRow(int y, int x0, const uint16_t* px, int n);
void getRow(int y, int x0, int n, uint16_t* out);

void clear();  // black

// Copies the buffer rectangle (sx, sy, w, h) to the screen at (dx, dy).
void blit(TFT_eSPI& tft, int sx, int sy, int w, int h, int dx, int dy);
inline void blit(TFT_eSPI& tft, int x, int y, int w, int h) { blit(tft, x, y, w, h, x, y); }

}  // namespace fb
