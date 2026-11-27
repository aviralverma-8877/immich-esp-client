#pragma once

#include <Arduino.h>

// Backlight dimming, battery monitoring and the two deep sleeps:
//
//  - power saver, between photos while the frame is offline. The panel keeps
//    its picture (its chip-select is held high so nothing reaches it) and the
//    ULP co-processor drives the backlight PWM, so the photo stays on screen
//    at the chosen brightness while the chip sleeps. A timer (next photo),
//    BOOT or a touch wakes it; the wake-up attaches to the panel without
//    resetting it.
//  - shutdown: panel and backlight off, until BOOT is pressed, which then
//    starts the frame from scratch.
//
// A small task on core 0 samples the battery while the chip is awake.
namespace power {

enum class BatteryState : uint8_t { None, Unknown, Discharging, Charging, Full };

struct Battery {
  BatteryState state = BatteryState::None;
  float volts = 0;  // smoothed, at the cell
  int percent = -1; // -1 when there is no battery
};

enum class Boot : uint8_t {
  Cold,          // power on, reset or upload
  FromShutdown,  // BOOT pressed after a shutdown
  SaverTimer,    // power saver: time for the next photo
  SaverButton,   // power saver: BOOT pressed
  SaverTouch,    // power saver: screen touched
};

// First thing in setup(): works out why the chip started and undoes the pin
// holds the last sleep left behind.
void releaseSleepHolds();
Boot bootReason();
inline bool saverWake(Boot b) { return b == Boot::SaverTimer || b == Boot::SaverButton || b == Boot::SaverTouch; }
bool wokeFromShutdown();

// Call after the TFT is initialised (TFT_eSPI drives the backlight pin on
// init). After a power-saver wake the ULP keeps the backlight until
// takeBacklight().
void begin(uint8_t backlightPct);
void takeBacklight();  // from the ULP back to the normal PWM

void setBacklight(uint8_t pct);  // 5..100
void setBusy(bool busy);         // the status LED, on boards with a single LED

Battery battery();
// battery() with the level read fresh: the averaged one goes stale while
// the chip spends its time asleep.
Battery measureBattery();
const char* stateName(BatteryState s);

// Power saver: deep sleep for `ms` (0 = until BOOT or a touch) with the
// photo left on screen. WiFi must be off.
[[noreturn]] void saverSleep(uint32_t ms, bool touchWakes);

// Backlight off, unused pins held, deep sleep until the BOOT button is
// pressed. Switch the panel off and wait for BOOT to be released first.
[[noreturn]] void shutdown();

}  // namespace power
