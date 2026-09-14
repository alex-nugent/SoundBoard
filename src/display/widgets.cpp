#include "display/widgets.h"
#include <string.h>

namespace widgets {

int16_t textW(const char* s, uint8_t scale) { return (int16_t)(strlen(s) * 6 * scale); }

void clipText(const char* s, char* out, size_t n, int16_t maxW, uint8_t scale) {
  size_t maxChars = (size_t)(maxW / (6 * scale));
  size_t len = strlen(s);
  if (len <= maxChars) { strlcpy(out, s, n); return; }
  if (maxChars <= 2) { strlcpy(out, s, n < maxChars + 1 ? n : maxChars + 1); return; }
  size_t keep = maxChars - 2;
  if (keep >= n - 3) keep = n - 3;
  memcpy(out, s, keep); out[keep] = '.'; out[keep + 1] = '.'; out[keep + 2] = 0;
}

void text(Adafruit_GFX& g, int16_t x, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg) {
  g.setTextWrap(false);
  g.setTextSize(scale);
  g.setTextColor(fg, bg);
  g.setCursor(x, y);
  g.print(s);
}

void textCentred(Adafruit_GFX& g, int16_t x0, int16_t w, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg) {
  int16_t tw = textW(s, scale) - scale;      // the last column of a glyph is blank
  int16_t x = x0 + (w - tw) / 2;
  if (x < x0) x = x0;
  text(g, x, y, s, scale, fg, bg);
}

void textRight(Adafruit_GFX& g, int16_t xRight, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg) {
  int16_t x = xRight - (textW(s, scale) - scale);
  if (x < 0) x = 0;
  text(g, x, y, s, scale, fg, bg);
}

// 3-segment icon 20x9 with a nub, pct text after it, a bolt when on USB.
void battery(Adafruit_GFX& g, int16_t x, int16_t y, int pct, bool usb, uint16_t fg, uint16_t dim, uint8_t scale) {
  int16_t s = scale < 1 ? 1 : scale;
  g.drawRect(x, y, 18 * s, 9 * s, fg);
  g.fillRect(x + 18 * s, y + 2 * s, 2 * s, 5 * s, fg);
  int segs = pct < 0 ? 0 : (pct > 66 ? 3 : pct > 33 ? 2 : pct > 5 ? 1 : 0);
  for (int i = 0; i < 3; i++) g.fillRect(x + (2 + i * 5) * s, y + 2 * s, 4 * s, 5 * s, i < segs ? fg : COL_BG);
  char t[8];
  if (pct < 0) strlcpy(t, "--%", sizeof t); else snprintf(t, sizeof t, "%d%%", pct);
  int16_t tx = x + 24 * s;
  text(g, tx, y + 1 * s, t, (uint8_t)s, fg);
  if (usb) {                                  // a bolt after the percentage
    int16_t bx = tx + textW(t, (uint8_t)s) + 2 * s;
    for (int16_t k = 0; k < s; k++) {         // thicken the strokes with the scale
      g.drawLine(bx + 3 * s + k, y, bx + k, y + 5 * s, fg);
      g.drawLine(bx + k, y + 5 * s, bx + 4 * s + k, y + 4 * s, fg);
      g.drawLine(bx + 4 * s + k, y + 4 * s, bx + 1 * s + k, y + 9 * s, fg);
    }
  } else {
    (void)dim;
  }
}

void hbar(Adafruit_GFX& g, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t fg, uint16_t bg) {
  if (pct > 100) pct = 100;
  int16_t fw = (int16_t)((int32_t)w * pct / 100);
  g.fillRect(x, y, fw, h, fg);
  g.fillRect(x + fw, y, w - fw, h, bg);
}

}  // namespace widgets
