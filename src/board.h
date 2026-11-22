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
// RGB LED, common anode: R=22, G=16, B=17, all active low. Blue is the status LED.
constexpr int kPinLed = 17;
constexpr bool kLedActiveHigh = false;
constexpr int kPinLedOff[] = {22, 16};  // other channels, held off
constexpr int kPinBtn1 = -1;  // no user buttons; touch and BOOT only
constexpr int kPinBtn2 = -1;
constexpr int kPinBoot = 0;
constexpr int kPinSdCs = 5;   // microSD on VSPI; keep it deselected
// TFT_eSPI's ST7796_Rotation.h, without the colour-order bit.
constexpr uint8_t kMadctl[4] = {0x40, 0x20, 0x80, 0x40 | 0x80 | 0x20};
constexpr bool kDefaultBgr = true;
constexpr bool kDefaultInvert = false;

#else  // BOARD_RJ9_ADAPTER

constexpr const char* kName = "ESP32 RJ9 Adapter";
// See ESP32-EasyEDA schematic, sheets "MCU" and "Buttons and LEDs".
constexpr int kPinLed = 2;    // LED2, active high
constexpr bool kLedActiveHigh = true;
constexpr int kPinLedOff[] = {-1};
constexpr int kPinBtn1 = 39;  // BTN1, active low, external 10k pull-up
constexpr int kPinBtn2 = 36;  // BTN2, active low, external 10k pull-up
constexpr int kPinBoot = 0;   // BOOT1, active low
constexpr int kPinSdCs = 33;  // SD card on the display module; keep it off the bus
// TFT_eSPI's ST7789_Rotation.h, without the colour-order bit.
constexpr uint8_t kMadctl[4] = {0x00, 0x40 | 0x20, 0x40 | 0x80, 0x20 | 0x80};
constexpr bool kDefaultBgr = true;  // this board's ST7789 is BGR
constexpr bool kDefaultInvert = false;

#endif

constexpr bool kHasButtons = kPinBtn1 >= 0 && kPinBtn2 >= 0;

}  // namespace board
