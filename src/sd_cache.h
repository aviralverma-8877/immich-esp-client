#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "app_config.h"
#include "immich_client.h"
#include "jpeg_stream.h"
#include "transitions.h"

// Offline copy of an album on the microSD card.
//
// Each photo is decoded once, at the screen's size and scaling, and stored as
// a raw RGB565 frame, so showing it later is a plain read and push: no WiFi,
// no JPEG decoding, about 0.2 s awake per photo.
//
//   /.immich-frame                         marks a card this frame has formatted
//   /frame/<album>/index.dat               the album's photos, oldest first, 48-byte records
//   /frame/<album>/<W>x<H><f|c>/have.bin   which of them are stored for this size (f = fit, c = crop)
//   /frame/<album>/<W>x<H><f|c>/<xx>/<id>.565   one frame; <xx> = first two id characters
//
// Safe from several tasks: the slideshow reads frames while the storing task
// adds them. Every card access is serialised by one recursive mutex (reading
// and writing the card at once from two tasks loses writes); store() only
// holds it while writing each block of rows, never while it waits on the
// network.
namespace cache {

enum class Card : uint8_t {
  None,       // no slot on this board, or no card in it
  Ready,      // mounted and ours
  NeedsFormat // a card that is unreadable (no or exFAT filesystem) or not yet formatted by the frame
};

// Mounts the card.
Card begin();
// Formats the card FAT32 and marks it as the frame's. Erases everything on it.
bool format();
bool mounted();

struct Info {
  bool mounted = false;
  bool open = false;
  uint32_t total = 0;   // photos in the album
  uint32_t cached = 0;  // stored for the current screen size
  uint32_t failed = 0;  // could not be stored this session
  uint64_t freeBytes = 0, cardBytes = 0;
};
Info info();  // any task

// Opens (creating as needed) the album's cache for a w x h screen.
bool open(const String& albumId, int w, int h, bool fill);
void close();
bool isOpen();
bool matches(const String& albumId, int w, int h, bool fill);

size_t count();
bool has(size_t i);
bool failed(size_t i);
void markFailed(size_t i);
void clearFailed();
bool asset(size_t i, Asset& out);

// Re-lists the album from Immich; `changed` says whether it differs from the
// stored list. Frames of photos that left the album are deleted.
bool refresh(const AppConfig& cfg, bool& changed, String& err);

// Downloads photo i's preview and stores it as a frame.
bool store(const AppConfig& cfg, size_t i, jpeg::AbortFn abort, String& err, bool& aborted);

// Draws photo i from the card with a row effect (None: top to bottom at once).
bool show(TFT_eSPI& tft, size_t i, String& err, fx::Effect effect = fx::Effect::None);

}  // namespace cache
