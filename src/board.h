#pragma once

#include <stdint.h>

// Board-specific pins and panel details. Pick a board with a build flag
// (see platformio.ini); display/touch SPI pins live in the TFT_eSPI flags there.
//
//   BOARD_RJ9_ADAPTER  "ESP32 RJ9 Adapter" (ESP32-EasyEDA/), ST7789 240x320
//   BOARD_E32R35T      Hosyond / LCDWiki 3.5" ESP32-32E, ST7796U 320x480,
//                      XPT2046 resistive touch on the panel's SPI bus

#if !defined(BOARD_RJ9_ADAPTER) && !defined(BOARD_E32R35T)
#define BOARD_RJ9_ADAPTER 1
#endif

namespace board {

#if defined(BOARD_E32R35T)

constexpr const char* kName = "Hosyond 3.5\" ESP32 (E32R35T)";
constexpr int kPinLed = -1;  // no single status LED
constexpr bool kLedActiveHigh = false;
// RGB LED, common anode: unused, so its pins are driven high (off) and held
// that way through sleep.
constexpr int kPinLedOff[] = {22, 16, 17};
constexpr int kPinBtn1 = -1;  // no user buttons; touch and BOOT only
constexpr int kPinBtn2 = -1;
constexpr int kPinBoot = 0;
constexpr int kPinTouchIrq = 36;  // XPT2046 PENIRQ, low while touched; wakes the chip from light sleep
// microSD on VSPI, separate from the panel's HSPI bus.
constexpr int kPinSdCs = 5, kPinSdSck = 18, kPinSdMiso = 19, kPinSdMosi = 23;
constexpr bool kHasSd = true;
// TFT_eSPI's ST7796_Rotation.h, without the colour-order bit.
constexpr uint8_t kMadctl[4] = {0x40, 0x20, 0x80, 0x40 | 0x80 | 0x20};
constexpr bool kDefaultBgr = true;
constexpr bool kDefaultInvert = false;
constexpr int kPinBacklight = 27;  // TFT_BL, PWM dimmable
// Battery sense on ADC1 through a 1:2 divider; the on-board charger has no status pin.
constexpr int kPinBattery = 34;
constexpr float kBatteryDivider = 2.0f;

#else  // BOARD_RJ9_ADAPTER

constexpr const char* kName = "ESP32 RJ9 Adapter";
// See ESP32-EasyEDA schematic, sheets "MCU" and "Buttons and LEDs".
constexpr int kPinLed = 2;    // LED2, active high
constexpr bool kLedActiveHigh = true;
constexpr int kPinLedOff[] = {-1};
constexpr int kPinBtn1 = 39;  // BTN1, active low, external 10k pull-up
constexpr int kPinBtn2 = 36;  // BTN2, active low, external 10k pull-up
constexpr int kPinBoot = 0;   // BOOT1, active low
constexpr int kPinTouchIrq = -1;
// SD card on the display module shares the panel's SPI bus; kept deselected.
constexpr int kPinSdCs = 33, kPinSdSck = -1, kPinSdMiso = -1, kPinSdMosi = -1;
constexpr bool kHasSd = false;
// TFT_eSPI's ST7789_Rotation.h, without the colour-order bit.
constexpr uint8_t kMadctl[4] = {0x00, 0x40 | 0x20, 0x40 | 0x80, 0x20 | 0x80};
constexpr bool kDefaultBgr = true;  // this board's ST7789 is BGR
constexpr bool kDefaultInvert = false;
constexpr int kPinBacklight = -1;  // backlight wired always on
constexpr int kPinBattery = -1;    // no battery
constexpr float kBatteryDivider = 1.0f;

#endif

constexpr bool kHasButtons = kPinBtn1 >= 0 && kPinBtn2 >= 0;
constexpr bool kHasBacklight = kPinBacklight >= 0;
constexpr bool kHasBattery = kPinBattery >= 0;

}  // namespace board
