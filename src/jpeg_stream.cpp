#include "jpeg_stream.h"

#include <tjpgd.h>

#include "frame_buffer.h"

namespace jpeg {
namespace {

constexpr size_t kBufPixels = 1024;

struct Ctx {
  Stream* in;
  int remaining;  // bytes left in the body, -1 if unknown
  bool ioError;

  TFT_eSPI* tft;
  bool toBuffer;
  AbortFn abort;
  bool aborted;

  // Decoded (post-tjpgd-scale) image size, and where it lands on screen.
  int srcW, srcH;
  int dstX, dstY, dstW, dstH;
  int scrW, scrH;

  uint16_t buf[kBufPixels];
};

// tjpgd's stock workspace assumes two Huffman tables per class; Immich
// previews often carry three (one per component), so leave room for them.
uint8_t gWork[TJPGD_WORKSPACE_SIZE + 2048] __attribute__((aligned(4)));
Ctx gCtx;  // ~2 KB; kept off the task stack. Only the slideshow task draws.

size_t input(JDEC* jd, uint8_t* dst, size_t n) {
  Ctx* c = static_cast<Ctx*>(jd->device);
  uint8_t skip[64];
  size_t done = 0;
  while (done < n && c->remaining != 0) {
    size_t want = n - done;
    if (c->remaining > 0 && want > (size_t)c->remaining) want = c->remaining;
    size_t got = dst ? c->in->readBytes(dst + done, want) : c->in->readBytes(skip, min(want, sizeof(skip)));
    if (got == 0) {
      c->ioError = true;  // stream timed out
      break;
    }
    done += got;
    if (c->remaining > 0) c->remaining -= got;
  }
  return done;
}

inline int ceilDiv(int a, int b) { return (a + b - 1) / b; }

// Maps one decoded block onto the screen with nearest-neighbour resampling.
int output(JDEC* jd, void* bitmap, JRECT* r) {
  Ctx* c = static_cast<Ctx*>(jd->device);
  if (c->abort && c->abort()) {
    c->aborted = true;
    return 0;
  }

  const uint16_t* src = static_cast<const uint16_t*>(bitmap);
  const int bw = r->right - r->left + 1;
  const int bh = r->bottom - r->top + 1;

  // Screen pixels whose source sample falls inside this block.
  int xs = c->dstX + ceilDiv(r->left * c->dstW, c->srcW);
  int xe = c->dstX + ceilDiv((r->right + 1) * c->dstW, c->srcW) - 1;
  int ys = c->dstY + ceilDiv(r->top * c->dstH, c->srcH);
  int ye = c->dstY + ceilDiv((r->bottom + 1) * c->dstH, c->srcH) - 1;
  xs = max(xs, 0);
  ys = max(ys, 0);
  xe = min(xe, c->scrW - 1);
  ye = min(ye, c->scrH - 1);
  if (xs > xe || ys > ye) return 1;

  if (c->toBuffer) {
    for (int yy = ys; yy <= ye; yy++) {
      int sy = (yy - c->dstY) * c->srcH / c->dstH - r->top;
      const uint16_t* row = src + constrain(sy, 0, bh - 1) * bw;
      uint16_t* p = c->buf;
      for (int xx = xs; xx <= xe; xx++) {
        int sx = (xx - c->dstX) * c->srcW / c->dstW - r->left;
        *p++ = row[constrain(sx, 0, bw - 1)];
      }
      fb::putRow(yy, xs, c->buf, xe - xs + 1);
    }
    return 1;
  }

  const int w = xe - xs + 1;
  const int rowsPerPush = max(1, (int)(kBufPixels / w));
  for (int y = ys; y <= ye; y += rowsPerPush) {
    const int rows = min(rowsPerPush, ye - y + 1);
    uint16_t* p = c->buf;
    for (int yy = y; yy < y + rows; yy++) {
      int sy = (yy - c->dstY) * c->srcH / c->dstH - r->top;
      sy = constrain(sy, 0, bh - 1);
      const uint16_t* row = src + sy * bw;
      for (int xx = xs; xx <= xe; xx++) {
        int sx = (xx - c->dstX) * c->srcW / c->dstW - r->left;
        *p++ = row[constrain(sx, 0, bw - 1)];
      }
    }
    c->tft->pushImage(xs, y, w, rows, c->buf);
  }
  return 1;
}

// Clears only the letterbox bars so the previous image is painted over
// rather than flashing to black.
void clearBars(TFT_eSPI& tft, const Ctx& c) {
  if (c.dstX > 0) {
    tft.fillRect(0, 0, c.dstX, c.scrH, TFT_BLACK);
    tft.fillRect(c.dstX + c.dstW, 0, c.scrW - c.dstX - c.dstW, c.scrH, TFT_BLACK);
  }
  if (c.dstY > 0) {
    tft.fillRect(0, 0, c.scrW, c.dstY, TFT_BLACK);
    tft.fillRect(0, c.dstY + c.dstH, c.scrW, c.scrH - c.dstY - c.dstH, TFT_BLACK);
  }
}

const char* describe(JRESULT r) {
  switch (r) {
    case JDR_INP: return "stream ended early";
    case JDR_MEM1: return "not enough decoder memory";
    case JDR_MEM2: return "not enough decoder buffer";
    case JDR_PAR: return "bad parameter";
    case JDR_FMT1: return "corrupt JPEG data";
    case JDR_FMT2: return "unsupported JPEG format";
    case JDR_FMT3: return "unsupported JPEG type (progressive, 12-bit or CMYK)";
    default: return "decode error";
  }
}

}  // namespace

bool draw(TFT_eSPI& tft, Stream& in, int len, bool fill, AbortFn abort, String& err, bool& aborted,
          bool& touched, bool toBuffer) {
  Ctx& c = gCtx;
  c.in = &in;
  c.remaining = len;
  c.ioError = false;
  c.tft = &tft;
  c.toBuffer = toBuffer && fb::active();
  c.abort = abort;
  c.aborted = false;
  aborted = false;
  touched = false;

  // Bodmer's tjpgd keeps jd.swap across jd_prepare(), so it must be set
  // first. Swapping in the decoder lets pushImage() send bytes untouched.
  JDEC jd = {};
  jd.swap = 1;
  JRESULT res = jd_prepare(&jd, input, gWork, sizeof(gWork), &c);
  if (res != JDR_OK) {
    if (c.ioError) {
      err = "JPEG header: download timed out";
    } else if (res == JDR_INP || res == JDR_FMT1) {
      // Seen in the wild: Immich serving a corrupt preview file as image/jpeg.
      err = "not a valid JPEG - regenerate this asset's thumbnails in Immich";
    } else {
      err = String("JPEG header: ") + describe(res);
    }
    return false;
  }
  touched = !c.toBuffer;  // bars and blocks are drawn from here on

  c.scrW = tft.width();
  c.scrH = tft.height();
  const int w = jd.width, h = jd.height;
  const float fx = (float)c.scrW / w, fy = (float)c.scrH / h;
  const float f = fill ? max(fx, fy) : min(fx, fy);
  c.dstW = max(1L, lroundf(w * f));
  c.dstH = max(1L, lroundf(h * f));
  c.dstX = (c.scrW - c.dstW) / 2;
  c.dstY = (c.scrH - c.dstH) / 2;

  // Coarsest tjpgd scale (1/1 .. 1/8) that still has at least the target
  // resolution, so we always downsample and decode as little as possible.
  uint8_t scale = 0;
  while (scale < 3 && (w >> (scale + 1)) >= c.dstW && (h >> (scale + 1)) >= c.dstH) scale++;
  c.srcW = max(1, w >> scale);
  c.srcH = max(1, h >> scale);

  if (c.toBuffer) {
    fb::clear();  // letterbox bars
  } else if (c.dstX > 0 || c.dstY > 0) {
    clearBars(tft, c);
  }

  const bool swap = tft.getSwapBytes();
  tft.setSwapBytes(false);
  uint32_t t0 = millis();
  res = jd_decomp(&jd, output, scale);
  tft.setSwapBytes(swap);

  if (c.aborted) {
    aborted = true;
    err = "aborted";
    return false;
  }
  if (res != JDR_OK) {
    err = String("JPEG decode: ") + (c.ioError ? "download timed out" : describe(res));
    return false;
  }
  log_i("Decoded %dx%d at 1/%d -> %dx%d in %lu ms", w, h, 1 << scale, c.dstW, c.dstH, millis() - t0);
  return true;
}

}  // namespace jpeg
