#pragma once

#include <TFT_eSPI.h>

extern TFT_eSPI tft;

namespace ui {

void begin(uint8_t rotation, bool bgr, bool invert);

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

// "2026-09-25" -> "25 Sep 2026"; returns "" for malformed input.
String formatDate(const char* isoDate);

}  // namespace ui
