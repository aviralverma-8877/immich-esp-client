#pragma once

#include <TFT_eSPI.h>

// TFT_eSPI that can also take over a panel that is already set up.
class Panel : public TFT_eSPI {
 public:
  // After a power-saver deep sleep the panel is still powered, configured
  // and showing the last photo: set up the bus and pins as init() does,
  // without its reset and init commands, so the picture stays.
  void attach();
};

extern Panel tft;

namespace ui {

void begin(uint8_t rotation, bool bgr, bool invert);
// begin() for a panel that kept its picture through deep sleep.
void attach(uint8_t rotation, bool bgr, bool invert);

// Full screen: an almost empty battery with a charging bolt, as phones show
// when they are too flat to start.
void lowBattery(int percent);

// Applies orientation and the panel colour fixes; safe to call at runtime.
void applyPanel(uint8_t rotation, bool bgr, bool invert);

// Labelled colour bars and a grey ramp, drawn both with fillRect and through
// pushImage (the path photos take), for diagnosing colour problems by eye.
void testPattern();

// Boot splash: a pinwheel in Immich colours, the wordmark and a status line
// that splashStatus() updates in place. Stays up until the first photo.
void splash(const String& status);
void splashStatus(const String& status, uint16_t color = 0x9CD3);

// Full-screen message: a title and up to three lines of detail.
void message(const String& title, const String& l1 = "", const String& l2 = "", const String& l3 = "",
             uint16_t titleColor = TFT_WHITE);

// Setup-mode screen: a QR code that joins the access point, plus the same
// details as text for people without a QR scanner.
void setupScreen(const String& apName, const String& apPass, const String& url, const String& note);

// Draws `text` as a QR code centred horizontally with its top edge at `y`,
// using the largest module size that fits `maxPx`. Returns the height drawn.
int qrCode(const String& text, int y, int maxPx);

// Small caption in the bottom-left corner, drawn over the current image.
void caption(const String& text);

// Pause marker in the top-right corner.
void pausedBadge(bool paused);

// Small notice centred at the top, drawn over the current image while the
// next one downloads; the next image repaints it.
void loadingBadge(const String& text);

// Box in the middle of the screen, drawn over the current image: a title in
// the accent colour and up to three lines below it.
void notice(const String& title, const String& l1, const String& l2 = "", const String& l3 = "");

// "2026-09-25" -> "25 Sep 2026"; returns "" for malformed input.
String formatDate(const char* isoDate);

}  // namespace ui
