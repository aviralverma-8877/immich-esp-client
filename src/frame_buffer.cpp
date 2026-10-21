#include "frame_buffer.h"

namespace fb {
namespace {

constexpr int kMaxBands = 32;  // 320 rows / 10
uint32_t* gBands[kMaxBands];
int gBandCount = 0;
int gW = 0, gH = 0;
int gWordsPerRow = 0;  // W is even (240 or 320), so rows are whole words

uint16_t gLine[320] __attribute__((aligned(4)));

// volatile forces real 32-bit loads and stores: GCC otherwise turns the
// masked read-modify-writes below into 16-bit stores (and the clear loop into
// memset), which fault on IRAM.
using Word = volatile uint32_t;

inline Word* rowWords(int y) { return gBands[y / kBandRows] + (y % kBandRows) * gWordsPerRow; }

}  // namespace

bool begin(int w, int h, size_t reserve) {
  end();
  const int bands = (h + kBandRows - 1) / kBandRows;
  if (bands > kMaxBands || (w & 1) || w > 320) return false;
  gWordsPerRow = w / 2;
  for (int i = 0; i < bands; i++) {
    const int rows = min(kBandRows, h - i * kBandRows);
    // MALLOC_CAP_32BIT prefers the IRAM heap, sparing the byte-addressable
    // DRAM that WiFi, HTTP and the portal need.
    gBands[i] = static_cast<uint32_t*>(heap_caps_malloc(rows * gWordsPerRow * 4, MALLOC_CAP_32BIT));
    if (!gBands[i] || heap_caps_get_free_size(MALLOC_CAP_8BIT) < reserve) {
      log_w("frame buffer: stopped at band %d/%d, 8bit free %u", i, bands,
            heap_caps_get_free_size(MALLOC_CAP_8BIT));
      gBandCount = gBands[i] ? i + 1 : i;
      end();
      return false;
    }
  }
  gBandCount = bands;
  gW = w;
  gH = h;
  return true;
}

void end() {
  for (int i = 0; i < gBandCount; i++) heap_caps_free(gBands[i]);
  gBandCount = 0;
  gW = gH = 0;
}

bool active() { return gBandCount > 0; }
int width() { return gW; }
int height() { return gH; }

void putRow(int y, int x0, const uint16_t* px, int n) {
  Word* words = rowWords(y);
  int i = 0, x = x0;
  if ((x & 1) && n > 0) {  // leading odd pixel: high half of its word
    Word& wd = words[x >> 1];
    wd = (wd & 0x0000FFFF) | ((uint32_t)px[0] << 16);
    i++, x++;
  }
  for (; i + 1 < n; i += 2, x += 2) words[x >> 1] = px[i] | ((uint32_t)px[i + 1] << 16);
  if (i < n) {  // trailing even pixel: low half
    Word& wd = words[x >> 1];
    wd = (wd & 0xFFFF0000) | px[i];
  }
}

void getRow(int y, int x0, int n, uint16_t* out) {
  const Word* words = rowWords(y);
  for (int i = 0; i < n; i++) {
    const int x = x0 + i;
    const uint32_t wd = words[x >> 1];
    out[i] = (x & 1) ? (uint16_t)(wd >> 16) : (uint16_t)wd;
  }
}

void clear() {
  for (int i = 0; i < gBandCount; i++) {
    const int rows = min(kBandRows, gH - i * kBandRows);
    Word* p = gBands[i];
    for (int k = rows * gWordsPerRow; k > 0; k--) *p++ = 0;
  }
}

void blit(TFT_eSPI& tft, int sx, int sy, int w, int h, int dx, int dy) {
  // Clip against the buffer and the screen.
  if (sx < 0) { w += sx; dx -= sx; sx = 0; }
  if (sy < 0) { h += sy; dy -= sy; sy = 0; }
  if (dx < 0) { w += dx; sx -= dx; dx = 0; }
  if (dy < 0) { h += dy; sy -= dy; dy = 0; }
  w = min(w, min(gW - sx, (int)tft.width() - dx));
  h = min(h, min(gH - sy, (int)tft.height() - dy));
  if (w <= 0 || h <= 0 || !active()) return;

  int r = 0;
  while (r < h) {
    const int y = sy + r;
    if (sx == 0 && w == gW) {
      // Full-width rows are contiguous and word aligned within a band, and
      // pushImage() reads 32-bit words, so they can go straight out.
      const int rows = min(h - r, kBandRows - y % kBandRows);
      tft.pushImage(dx, dy + r, w, rows, reinterpret_cast<uint16_t*>(const_cast<uint32_t*>(rowWords(y))));
      r += rows;
    } else {
      getRow(y, sx, w, gLine);  // realign through DRAM
      tft.pushImage(dx, dy + r, w, 1, gLine);
      r++;
    }
  }
}

}  // namespace fb
