#include "display_ui.h"

#include <qrcode.h>

TFT_eSPI tft;

namespace ui {
namespace {

// esp_qrcode_generate() reports through a plain function pointer, so the
// draw parameters travel in statics.
int gQrTop, gQrMaxPx, gQrDrawn;

void drawQr(esp_qrcode_handle_t qr) {
  const int size = esp_qrcode_get_size(qr);
  const int quiet = 2;  // modules of white border
  const int scale = max(1, gQrMaxPx / (size + quiet * 2));
  const int px = (size + quiet * 2) * scale;
  const int x0 = (tft.width() - px) / 2;
  tft.fillRect(x0, gQrTop, px, px, TFT_WHITE);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qr, x, y))
        tft.fillRect(x0 + (x + quiet) * scale, gQrTop + (y + quiet) * scale, scale, scale, TFT_BLACK);
    }
  }
  gQrDrawn = px;
}

}  // namespace

// ---- splash -----------------------------------------------------------------

namespace {

constexpr uint16_t kSplashBg = 0x0861;  // #0c0e12
constexpr int kLogo = 132;               // logo box, px
int gStatusY;

uint16_t rgb(uint32_t hex) { return tft.color565(hex >> 16, (hex >> 8) & 0xFF, hex & 0xFF); }

// Five petals, each a circle joined to an off-centre base point, so together
// they read as a pinwheel. Colours follow the Immich palette.
void drawPinwheel(int x0, int y0) {
  static const uint32_t kPetals[] = {0xFA2921, 0xED79B5, 0xFFB400, 0x1E83F7, 0x18C249};
  const float cx = x0 + kLogo / 2.0f, cy = y0 + kLogo / 2.0f, d = 30, r = 22, twist = 9;
  for (int i = 0; i < 5; i++) {
    const float a = -HALF_PI + i * TWO_PI / 5;
    const float px = cx + d * cosf(a), py = cy + d * sinf(a);                        // petal centre
    const float bx = cx + twist * cosf(a - HALF_PI), by = cy + twist * sinf(a - HALF_PI);  // base
    const uint16_t col = rgb(kPetals[i]);
    tft.fillTriangle(bx, by, px + r * cosf(a + HALF_PI), py + r * sinf(a + HALF_PI),
                     px + r * cosf(a - HALF_PI), py + r * sinf(a - HALF_PI), col);
    tft.fillCircle(px, py, r, col);
  }
  tft.fillCircle(cx, cy, 7, kSplashBg);  // small hub
}

}  // namespace

void splash(const String& status) {
  tft.fillScreen(kSplashBg);
  const int w = tft.width(), h = tft.height();
  const bool portrait = h > w;
  const int logoY = portrait ? 46 : 14;
  drawPinwheel((w - kLogo) / 2, logoY);
  const int textY = logoY + kLogo + (portrait ? 26 : 10);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, kSplashBg);
  tft.drawString("immich", w / 2, textY, 4);
  tft.setTextColor(rgb(0x8A93A6), kSplashBg);
  tft.drawString("photo frame", w / 2, textY + 28, 2);
  tft.setTextDatum(TL_DATUM);
  gStatusY = textY + (portrait ? 70 : 52);
  splashStatus(status);
}

void splashStatus(const String& status, uint16_t color) {
  const int w = tft.width();
  tft.fillRect(0, gStatusY - 2, w, 20, kSplashBg);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kSplashBg);
  tft.drawString(status, w / 2, gStatusY, 2);
  tft.setTextDatum(TL_DATUM);
}

int qrCode(const String& text, int y, int maxPx) {
  gQrTop = y;
  gQrMaxPx = maxPx;
  gQrDrawn = 0;
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = drawQr;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
  esp_qrcode_generate(&cfg, text.c_str());
  return gQrDrawn;
}

void setupScreen(const String& apName, const String& apPass, const String& url, const String& note) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  const int cx = tft.width() / 2;
  const bool portrait = tft.height() > tft.width();
  int y = 8;

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Frame setup", cx, y, 4);
  y += 32;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Scan to join the frame's WiFi", cx, y, 2);
  y += 20;

  // Standard WiFi QR payload; special characters in the fields are escaped.
  auto esc = [](String s) {
    String out;
    for (char c : s) {
      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
      out += c;
    }
    return out;
  };
  const int qrMax = portrait ? 150 : 110;
  y += qrCode("WIFI:T:WPA;S:" + esc(apName) + ";P:" + esc(apPass) + ";;", y, qrMax) + 8;

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("or join " + apName, cx, y, 2);
  y += 18;
  tft.drawString("password: " + apPass, cx, y, 2);
  y += 24;
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("The setup page opens itself,", cx, y, 2);
  y += 18;
  tft.drawString("or browse to " + url, cx, y, 2);
  y += 22;
  if (note.length()) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(note, cx, y, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void begin(uint8_t rotation, bool bgr, bool invert) {
  tft.init();
  applyPanel(rotation, bgr, invert);
  tft.fillScreen(TFT_BLACK);
}

void applyPanel(uint8_t rotation, bool bgr, bool invert) {
  tft.setRotation(rotation);
  // setRotation() writes MADCTL with the compile-time RGB order; rewrite it
  // with the chosen order. Values mirror TFT_eSPI's ST7789_Rotation.h.
  static const uint8_t kMadctl[4] = {0x00, 0x40 | 0x20, 0x40 | 0x80, 0x20 | 0x80};
  tft.writecommand(0x36);
  tft.writedata(kMadctl[rotation & 3] | (bgr ? 0x08 : 0x00));
  tft.invertDisplay(invert);
}

void testPattern() {
  struct Bar {
    uint16_t color;
    const char* name;
  };
  static const Bar kBars[] = {{TFT_RED, "RED"},     {TFT_GREEN, "GREEN"},  {TFT_BLUE, "BLUE"},
                              {TFT_YELLOW, "YELLOW"}, {TFT_CYAN, "CYAN"}, {TFT_MAGENTA, "MAGENTA"},
                              {TFT_WHITE, "WHITE"}};
  const int n = sizeof(kBars) / sizeof(kBars[0]);
  const int w = tft.width(), h = tft.height();
  const int barH = (h - 40) / n;
  const int half = w / 2;

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  uint16_t line[320];
  for (int i = 0; i < n; i++) {
    const int y = i * barH;
    // Left half: fillRect (how text and UI are drawn).
    tft.fillRect(0, y, half, barH, kBars[i].color);
    // Right half: pushImage with pre-swapped pixels, exactly as photos are drawn.
    const uint16_t c = kBars[i].color;
    for (int x = 0; x < w - half; x++) line[x] = (c << 8) | (c >> 8);
    for (int yy = y; yy < y + barH; yy++) tft.pushImage(half, yy, w - half, 1, line);
    const bool dark = kBars[i].color == TFT_BLUE || kBars[i].color == TFT_RED;
    tft.setTextColor(dark ? TFT_WHITE : TFT_BLACK);
    tft.drawString(kBars[i].name, 6, y + barH / 2, 2);
  }
  // Grey ramp: should run smoothly from black (left) to white (right).
  const int y = n * barH + 4;
  for (int x = 0; x < w; x++) {
    const uint8_t v = x * 255 / (w - 1);
    tft.drawFastVLine(x, y, 16, tft.color565(v, v, v));
  }
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("left: UI  right: photo path", w / 2, y + 18, 1);
  tft.setTextDatum(TL_DATUM);
}

void message(const String& title, const String& l1, const String& l2, const String& l3, uint16_t titleColor) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  const int cx = tft.width() / 2;
  int y = tft.height() / 2 - 40;

  tft.setTextColor(titleColor, TFT_BLACK);
  tft.drawString(title, cx, y, 4);
  y += 40;

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  for (const String* line : {&l1, &l2, &l3}) {
    if (line->length()) tft.drawString(*line, cx, y, 2);
    y += 22;
  }
  tft.setTextDatum(TL_DATUM);
}

void caption(const String& text) {
  if (text.isEmpty()) return;
  tft.setTextDatum(BL_DATUM);
  tft.setTextPadding(0);
  const int pad = 4;
  const int w = tft.textWidth(text, 2) + pad * 2;
  const int h = tft.fontHeight(2) + pad;
  tft.fillRect(0, tft.height() - h, w, h, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(text, pad, tft.height() - 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void pausedBadge(bool paused) {
  const int x = tft.width() - 26, y = 6;
  if (!paused) return;  // the next image repaints the corner
  tft.fillRoundRect(x, y, 20, 20, 4, TFT_BLACK);
  tft.fillRect(x + 5, y + 4, 4, 12, TFT_WHITE);
  tft.fillRect(x + 11, y + 4, 4, 12, TFT_WHITE);
}

String formatDate(const char* iso) {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y, m, d;
  if (!iso || sscanf(iso, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12) return "";
  return String(d) + " " + kMonths[m - 1] + " " + String(y);
}

}  // namespace ui
