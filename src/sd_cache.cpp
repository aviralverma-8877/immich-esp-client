#include "sd_cache.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <rom/crc.h>
#include <sd_diskio.h>

#include <algorithm>
#include <vector>

#include "board.h"
#include "ff.h"  // before diskio.h, which uses its types
#include "diskio.h"

namespace cache {
namespace {

constexpr const char* kRoot = "/frame";
constexpr const char* kMarker = "/.immich-frame";
constexpr uint32_t kSpiHz = 20000000;
constexpr size_t kRec = 48;           // 36 id, space, 10 date, newline
constexpr size_t kMaxAssets = 8000;   // ~2.4 GB of frames at 320x480
constexpr uint32_t kHaveMagic = 0x31564148;  // "HAV1"
constexpr uint64_t kSpareBytes = 4ull << 20;

SPIClass gSpi(VSPI);
SemaphoreHandle_t gLock = xSemaphoreCreateRecursiveMutex();

struct Lock {
  Lock() { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(gLock); }
};
bool gSpiReady = false;
bool gMounted = false;
bool gOpen = false;
String gAlbumId, gAlbumDir, gGeomDir;
int gW = 0, gH = 0;
bool gFill = false;
File gIndex;
volatile size_t gCount = 0;
uint32_t gIndexCrc = 0;
std::vector<uint8_t> gHave, gFailed;
volatile uint32_t gCachedN = 0, gFailedN = 0;
volatile uint64_t gFree = 0, gTotal = 0;

// SD.remove()/open() log an error for a missing file; these are routine here.
bool removeIfThere(const String& path) { return SD.exists(path) && SD.remove(path); }

String indexPath() { return gAlbumDir + "/index.dat"; }
String havePath() { return gGeomDir + "/have.bin"; }
String framePath(const char* id) { return gGeomDir + "/" + String(id).substring(0, 2) + "/" + id + ".565"; }
size_t frameBytes() { return (size_t)gW * gH * 2; }

bool getBit(const std::vector<uint8_t>& v, size_t i) { return i / 8 < v.size() && ((v[i / 8] >> (i % 8)) & 1); }
void setBit(std::vector<uint8_t>& v, size_t i, bool on) {
  if (i / 8 >= v.size()) return;
  if (on) v[i / 8] |= 1 << (i % 8);
  else v[i / 8] &= ~(1 << (i % 8));
}
uint32_t countBits(const std::vector<uint8_t>& v) {
  uint32_t n = 0;
  for (uint8_t b : v) n += __builtin_popcount(b);
  return n;
}

void mkdirs(const String& path) {
  for (size_t i = 1; i <= path.length(); i++) {
    if (i < path.length() && path[i] != '/') continue;
    const String part = path.substring(0, i);
    if (!SD.exists(part)) SD.mkdir(part);
  }
}

void updateSpace() {
  gTotal = SD.totalBytes();
  gFree = gTotal - SD.usedBytes();
}

bool mount() {
  if (!gSpiReady) {
    gSpi.begin(board::kPinSdSck, board::kPinSdMiso, board::kPinSdMosi, board::kPinSdCs);
    gSpiReady = true;
  }
  gMounted = SD.begin(board::kPinSdCs, gSpi, kSpiHz, "/sd", 4, false);
  return gMounted;
}

void loadIndex() {
  if (gIndex) gIndex.close();
  gCount = 0;
  gIndexCrc = 0;
  if (!SD.exists(indexPath())) return;
  File f = SD.open(indexPath(), FILE_READ);
  if (!f) return;
  uint8_t buf[512];
  for (int got; (got = f.read(buf, sizeof(buf))) > 0;) gIndexCrc = crc32_le(gIndexCrc, buf, got);
  gCount = std::min(f.size() / kRec, kMaxAssets);
  f.seek(0);
  gIndex = f;
}

void saveHave() {
  File f = SD.open(havePath(), FILE_WRITE);
  if (!f) return;
  const uint32_t hdr[2] = {kHaveMagic, gIndexCrc};
  f.write((const uint8_t*)hdr, sizeof(hdr));
  f.write(gHave.data(), gHave.size());
  f.close();
}

// Works out from the files on the card which photos are stored.
void rebuildHave() {
  gHave.assign((gCount + 7) / 8, 0);
  for (size_t i = 0; i < gCount; i++) {
    Asset a;
    if (asset(i, a) && SD.exists(framePath(a.id))) setBit(gHave, i, true);
  }
  saveHave();
  gCachedN = countBits(gHave);
}

void loadHave() {
  gHave.assign((gCount + 7) / 8, 0);
  gFailed.assign((gCount + 7) / 8, 0);
  gFailedN = 0;
  bool ok = false;
  File f = SD.exists(havePath()) ? SD.open(havePath(), FILE_READ) : File();
  if (f && f.size() == 8 + gHave.size()) {
    uint32_t hdr[2] = {0, 0};
    ok = f.read((uint8_t*)hdr, 8) == 8 && hdr[0] == kHaveMagic && hdr[1] == gIndexCrc &&
         f.read(gHave.data(), gHave.size()) == (int)gHave.size();
  }
  if (f) f.close();
  if (ok) {
    gCachedN = countBits(gHave);
  } else {
    rebuildHave();  // new size or scaling, or the list changed
  }
}

uint32_t idHash(const char* id) { return crc32_le(0, (const uint8_t*)id, 36); }

// Deletes this size's frames of photos that are not in the new list.
void pruneRemoved(const String& newIndex, size_t n) {
  if (gCount == 0) return;
  uint32_t* keep = static_cast<uint32_t*>(malloc(std::max<size_t>(n, 1) * 4));
  if (!keep) {
    log_w("cache: no memory to prune %u photos", (unsigned)n);
    return;
  }
  File f = SD.open(newIndex, FILE_READ);
  size_t k = 0;
  char rec[kRec];
  while (k < n && f && f.read((uint8_t*)rec, kRec) == kRec) keep[k++] = idHash(rec);
  if (f) f.close();
  std::sort(keep, keep + k);
  int removed = 0;
  for (size_t i = 0; i < gCount; i++) {
    Asset a;
    if (!asset(i, a) || std::binary_search(keep, keep + k, idHash(a.id))) continue;
    if (removeIfThere(framePath(a.id))) removed++;
  }
  free(keep);
  if (removed) Serial.printf("[cache] removed %d photos no longer in the album\n", removed);
}

}  // namespace

Card begin() {
  if (!board::kHasSd) return Card::None;
  if (mount()) {
    updateSpace();
    Serial.printf("[cache] SD card %llu MB, %llu MB free\n", gTotal >> 20, gFree >> 20);
    return SD.exists(kMarker) ? Card::Ready : Card::NeedsFormat;
  }
  // Not mountable: tell an empty slot from a card without a FAT filesystem.
  const uint8_t pdrv = sdcard_init(board::kPinSdCs, &gSpi, kSpiHz);
  if (pdrv == 0xFF) return Card::None;
  const bool present = !(disk_initialize(pdrv) & STA_NOINIT);
  sdcard_uninit(pdrv);
  Serial.printf("[cache] %s\n", present ? "SD card has no readable filesystem" : "no SD card");
  return present ? Card::NeedsFormat : Card::None;
}

bool format() {
  if (!board::kHasSd) return false;
  Lock l;
  close();
  if (gMounted) SD.end();
  gMounted = false;
  if (!gSpiReady) {
    gSpi.begin(board::kPinSdSck, board::kPinSdMiso, board::kPinSdMosi, board::kPinSdCs);
    gSpiReady = true;
  }
  const uint8_t pdrv = sdcard_init(board::kPinSdCs, &gSpi, kSpiHz);
  if (pdrv == 0xFF) return false;
  const char drv[3] = {(char)('0' + pdrv), ':', 0};
  // A big work buffer makes this seconds rather than a minute on a 32 GB card.
  size_t len = 32768;
  void* work = nullptr;
  while (len >= 512 && !(work = malloc(len))) len /= 2;
  FRESULT r = FR_NOT_ENOUGH_CORE;
  if (work) {
    const uint32_t t0 = millis();
    // 32 KB clusters suit 300 KB frames; small cards fall back to the default.
    r = f_mkfs(drv, FM_ANY, 32768, work, len);
    if (r != FR_OK) r = f_mkfs(drv, FM_ANY, 0, work, len);
    free(work);
    Serial.printf("[cache] format: %s in %lu ms\n", r == FR_OK ? "done" : "FAILED", millis() - t0);
  }
  sdcard_uninit(pdrv);
  if (r != FR_OK || !mount()) return false;
  File m = SD.open(kMarker, FILE_WRITE);
  if (m) {
    m.print("Immich Frame photo cache. Formatted by the frame; safe to delete.\n");
    m.close();
  }
  updateSpace();
  return true;
}

bool mounted() { return gMounted; }

Info info() {
  Info i;
  i.mounted = gMounted;
  i.open = gOpen;
  i.total = gCount;
  i.cached = gCachedN;
  i.failed = gFailedN;
  i.freeBytes = gFree;
  i.cardBytes = gTotal;
  return i;
}

bool matches(const String& albumId, int w, int h, bool fill) {
  Lock l;
  return gOpen && albumId == gAlbumId && w == gW && h == gH && fill == gFill;
}

bool open(const String& albumId, int w, int h, bool fill) {
  if (!gMounted || albumId.length() != 36) return false;
  Lock l;
  if (matches(albumId, w, h, fill)) return true;
  close();
  gAlbumId = albumId;
  gW = w;
  gH = h;
  gFill = fill;
  gAlbumDir = String(kRoot) + "/" + albumId;
  gGeomDir = gAlbumDir + "/" + String(w) + "x" + String(h) + (fill ? "c" : "f");
  mkdirs(gGeomDir);
  removeIfThere(gGeomDir + "/part.tmp");  // a download cut short by a reset
  loadIndex();
  loadHave();
  gOpen = true;
  Serial.printf("[cache] album %s at %dx%d: %u of %u photos stored\n", albumId.c_str(), w, h, gCachedN,
                (unsigned)gCount);
  return true;
}

void close() {
  Lock l;
  if (gIndex) gIndex.close();
  gOpen = false;
  gCount = 0;
  gCachedN = gFailedN = 0;
  gHave.clear();
  gFailed.clear();
  gAlbumId = "";
}

bool isOpen() { return gOpen; }
size_t count() { return gCount; }
bool has(size_t i) {
  Lock l;
  return getBit(gHave, i);
}

bool failed(size_t i) {
  Lock l;
  return getBit(gFailed, i);
}

void markFailed(size_t i) {
  Lock l;
  setBit(gFailed, i, true);
  gFailedN = countBits(gFailed);
}

void clearFailed() {
  Lock l;
  std::fill(gFailed.begin(), gFailed.end(), 0);
  gFailedN = 0;
}

bool asset(size_t i, Asset& out) {
  Lock l;  // gIndex is one file handle, shared
  if (!gIndex || i >= gCount) return false;
  char rec[kRec];
  if (!gIndex.seek(i * kRec) || gIndex.read((uint8_t*)rec, kRec) != kRec || rec[36] != ' ' || rec[47] != '\n')
    return false;
  memcpy(out.id, rec, 36);
  out.id[36] = 0;
  int n = 0;
  while (n < 10 && rec[37 + n] != ' ') n++;
  memcpy(out.date, rec + 37, n);
  out.date[n] = 0;
  return true;
}

bool refresh(const AppConfig& cfg, bool& changed, String& err) {
  changed = false;
  if (!gOpen) {
    err = "cache not open";
    return false;
  }
  const String tmp = gAlbumDir + "/index.new";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) {
    err = "cannot write to the SD card";
    return false;
  }
  uint32_t crc = 0;
  size_t n = 0;
  bool writeFailed = false, truncated = false;
  bool ok = immich::listAssets(cfg, [&](const Asset& a) {
    if (n >= kMaxAssets) {
      truncated = true;
      return false;
    }
    char rec[kRec];
    memset(rec, ' ', kRec);
    memcpy(rec, a.id, 36);
    memcpy(rec + 37, a.date, std::min<size_t>(strlen(a.date), 10));
    rec[47] = '\n';
    if (f.write((const uint8_t*)rec, kRec) != kRec) {
      writeFailed = true;
      return false;
    }
    crc = crc32_le(crc, (const uint8_t*)rec, kRec);
    n++;
    return true;
  }, err);
  f.close();
  if (writeFailed) {
    err = "writing to the SD card failed";
    ok = false;
  }
  if (!ok) {
    SD.remove(tmp);
    return false;
  }
  if (truncated) log_w("cache: album has more than %u photos; keeping the oldest", (unsigned)kMaxAssets);

  Lock l;  // swap the list in while nobody reads it
  changed = n != gCount || crc != gIndexCrc;
  if (!changed) {
    SD.remove(tmp);
    return true;
  }
  pruneRemoved(tmp, n);
  gIndex.close();
  removeIfThere(indexPath());
  if (!SD.rename(tmp, indexPath())) {
    err = "cannot update the photo list on the SD card";
    return false;
  }
  loadIndex();
  gHave.assign((gCount + 7) / 8, 0);
  gFailed.assign((gCount + 7) / 8, 0);
  gFailedN = 0;
  rebuildHave();
  updateSpace();
  Serial.printf("[cache] album now %u photos, %u stored\n", (unsigned)gCount, gCachedN);
  return true;
}

bool store(const AppConfig& cfg, size_t i, jpeg::AbortFn abort, String& err, bool& aborted) {
  aborted = false;
  Asset a;
  if (!asset(i, a)) {
    err = "cannot read the photo list on the SD card";
    return false;
  }
  if (gFree < frameBytes() + kSpareBytes) {
    err = "SD card is full";
    return false;
  }
  const String tmp = gGeomDir + "/part.tmp";
  File f;
  {
    Lock l;
    const String dir = gGeomDir + "/" + String(a.id).substring(0, 2);
    if (!SD.exists(dir)) SD.mkdir(dir);
    f = SD.open(tmp, FILE_WRITE);
  }
  if (!f) {
    err = "cannot write to the SD card";
    return false;
  }
  size_t written = 0;
  uint32_t tStart = millis(), tBody = 0, tWrite = 0;
  bool ok = immich::fetchPreview(cfg, a.id, [&](Stream& in, int len, String& e) {
    tBody = millis();
    return jpeg::decode(in, len, gW, gH, gFill, abort, [&](int, int rows, const uint16_t* px) {
      const size_t bytes = (size_t)gW * rows * 2;
      const uint32_t tw = millis();
      Lock l;
      const size_t w = f.write((const uint8_t*)px, bytes);
      tWrite += millis() - tw;
      written += w;
      return w == bytes;
    }, e, aborted);
  }, err);
  const uint32_t tDone = millis();
  Lock l;
  f.close();
  log_i("store: request %lu ms, download+decode %lu ms (SD writes %lu ms)", tBody - tStart, tDone - tBody,
        tWrite);
  if (ok && written != frameBytes()) {
    ok = false;
    err = "incomplete frame";
  }
  if (!gOpen || i >= gCount) ok = false;  // closed or re-listed meanwhile (the caller stops us first)
  const String path = framePath(a.id);
  if (ok) {
    removeIfThere(path);
    ok = SD.rename(tmp, path);
    if (!ok) err = "cannot save the frame on the SD card";
  }
  if (!ok) {
    SD.remove(tmp);
    return false;
  }
  setBit(gHave, i, true);
  setBit(gFailed, i, false);
  saveHave();
  gCachedN = countBits(gHave);
  gFailedN = countBits(gFailed);
  gFree = gFree > written ? gFree - written : 0;
  return true;
}

bool show(TFT_eSPI& tft, size_t i, String& err, fx::Effect effect) {
  Asset a;
  if (!asset(i, a)) {
    err = "cannot read the photo list on the SD card";
    return false;
  }
  if (tft.width() != gW || tft.height() != gH) {
    err = "screen size changed";
    return false;
  }
  Lock l;  // the whole frame, transition included: the storing task waits
  File f = SD.open(framePath(a.id), FILE_READ);
  if (!f || f.size() != frameBytes()) {
    if (f) f.close();
    setBit(gHave, i, false);  // deleted or damaged on the card; fetch it again when online
    saveHave();
    gCachedN = countBits(gHave);
    err = "stored photo missing";
    return false;
  }
  constexpr int kRows = 16;
  uint16_t* buf = static_cast<uint16_t*>(malloc((size_t)gW * kRows * 2));
  if (!buf) {
    f.close();
    err = "no memory";
    return false;
  }
  const bool swap = tft.getSwapBytes();
  tft.setSwapBytes(false);  // frames are stored in panel byte order
  // Rows y.. of the frame, read straight from the file.
  int filePos = 0;  // next row the file is positioned at
  const fx::RowPush push = [&](int y, int n) {
    for (int r = 0; r < n;) {
      const int rows = std::min(kRows, n - r);
      const size_t bytes = (size_t)gW * rows * 2;
      if (filePos != y + r && !f.seek((size_t)(y + r) * gW * 2)) return false;
      if (f.read((uint8_t*)buf, bytes) != bytes) return false;
      tft.pushImage(0, y + r, gW, rows, buf);
      r += rows;
      filePos = y + r;
    }
    return true;
  };
  const uint32_t t0 = millis();
  const bool ok = fx::isRowEffect(effect) ? fx::runRows(effect, gH, push) : push(0, gH);
  log_d("frame %s in %lu ms", fx::name(effect), millis() - t0);
  tft.setSwapBytes(swap);
  free(buf);
  f.close();
  if (!ok) err = "reading the SD card failed";
  return ok;
}

}  // namespace cache
