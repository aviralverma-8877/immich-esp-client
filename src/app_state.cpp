#include "app_state.h"

namespace state {
namespace {

QueueHandle_t gQueue;
SemaphoreHandle_t gMutex;
Status gStatus;

struct Guard {
  Guard() { xSemaphoreTake(gMutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(gMutex); }
};

}  // namespace

void begin() {
  gQueue = xQueueCreate(8, sizeof(Cmd));
  gMutex = xSemaphoreCreateMutex();
}

void post(Cmd cmd) { xQueueSend(gQueue, &cmd, 0); }

Cmd take() {
  Cmd cmd = Cmd::None;
  xQueueReceive(gQueue, &cmd, 0);
  return cmd;
}

Cmd peek() {
  Cmd cmd = Cmd::None;
  xQueuePeek(gQueue, &cmd, 0);
  return cmd;
}

Status get() {
  Guard g;
  return gStatus;
}

void setAp(bool ap, const String& ip) {
  Guard g;
  gStatus.apMode = ap;
  gStatus.ip = ip;
}

void setPaused(bool paused) {
  Guard g;
  gStatus.paused = paused;
}

void setShowing(const char* id, const char* date) {
  Guard g;
  gStatus.assetId = id;
  gStatus.assetDate = date;
  gStatus.shown++;
}

void setError(const String& err) {
  Guard g;
  gStatus.lastError = err;
  gStatus.lastErrorAt = millis();
}

void setLoading(bool loading) {
  Guard g;
  gStatus.loading = loading;
}

void setNextReady(bool ready) {
  Guard g;
  gStatus.nextReady = ready;
}

void countSkipped() {
  Guard g;
  gStatus.skipped++;
}

}  // namespace state
