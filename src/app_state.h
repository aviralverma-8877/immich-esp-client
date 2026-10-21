#pragma once

#include <Arduino.h>

// Shared between the slideshow (loop task) and the web portal task.
namespace state {

enum class Cmd : uint8_t { None, Next, Prev, TogglePause, Reload, CalibrateTouch, Reboot, FactoryReset, TestPattern };

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

}  // namespace state
