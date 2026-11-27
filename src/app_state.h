#pragma once

#include <Arduino.h>

// Shared between the slideshow (loop task) and the web portal task.
namespace state {

enum class Cmd : uint8_t {
  None, Next, Prev, TogglePause, Reload, CalibrateTouch, Reboot, FactoryReset, TestPattern, SetupMode,
  GoOnline,    // BOOT pressed: WiFi and the portal back on
  GoOffline,   // portal: WiFi off now (album must be stored)
  Shutdown,    // BOOT held, or the portal: deep sleep until BOOT is pressed
  FormatSd,    // portal: erase and format the SD card
  ShowBattery, // BOOT double-tapped: battery level on screen
};

struct Status {
  bool apMode = false;
  bool paused = false;
  String ip;
  String assetId;
  String assetDate;
  String lastError;
  uint32_t lastErrorAt = 0;  // millis() when lastError was set
  uint32_t shown = 0;
  uint32_t skipped = 0;
  bool loading = false;    // a preview is downloading/decoding right now
  bool nextReady = false;  // the next photo is decoded and waiting
  bool cacheMode = false;  // the album plays from the SD card
  bool syncing = false;    // photos are still being stored on the card
};

void begin();

// Queue a command for the slideshow. Safe from any task.
void post(Cmd cmd);
// Pop the next pending command, or Cmd::None.
Cmd take();
// Look at the next pending command without removing it.
Cmd peek();

Status get();
void setAp(bool ap, const String& ip);
void setPaused(bool paused);
void setShowing(const char* id, const char* date);
void setError(const String& err);
void countSkipped();
void setLoading(bool loading);
void setNextReady(bool ready);
void setCache(bool cacheMode, bool syncing);

}  // namespace state
