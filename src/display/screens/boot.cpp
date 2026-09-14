#include "display/screens/screens.h"
#include "display/display.h"
#include "display/widgets.h"

void BootScreen::draw(Display& d, uint8_t regions) {
  if (!(regions & R_MAIN)) return;             // the boot screen is one region
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  g.fillRect(0, 0, SCR_W, SCR_H, COL_BG);
  if (recoveryPrompt) {
    widgets::textCentred(g, 0, SCR_W, 40, "release for a normal start", 1, t.fg);
    widgets::textCentred(g, 0, SCR_W, 60, "keep holding for recovery", 1, t.fg);
  } else if (calibrationHint) {
    widgets::textCentred(g, 0, SCR_W, 40, "keep hands off", 2, t.fg);
    widgets::textCentred(g, 0, SCR_W, 64, "the buttons", 2, t.fg);
  } else {
    int16_t total = nLines * 16 + (nLines > 1 ? (nLines - 1) * 6 : 0);
    int16_t y = (SCR_H - 16 - total) / 2;
    if (y < 4) y = 4;
    for (uint8_t i = 0; i < nLines && i < 3; i++) {
      char clipped[24];
      widgets::clipText(lines[i] ? lines[i] : "", clipped, sizeof clipped, SCR_W - 8, 2);
      widgets::textCentred(g, 0, SCR_W, y, clipped, 2, t.fg);
      y += 22;
    }
  }
  widgets::textRight(g, SCR_W - 3, SCR_H - 10, version, 1, t.dim);
}

// ---------------------------------------------------------------------------
// §11.8 / §12.3: big message ("OFF", "BATTERY EMPTY") and the charging card
// ---------------------------------------------------------------------------
void MessageScreen::draw(Display& d, uint8_t) {
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  g.fillRect(0, 0, SCR_W, SCR_H, COL_BG);
  int16_t y = sub ? 34 : (SCR_H - 8 * scale) / 2;
  widgets::textCentred(g, 0, SCR_W, y, text, scale, t.fg);
  if (sub) widgets::textCentred(g, 0, SCR_W, y + 8 * scale + 12, sub, 2, t.dim);
}

void ChargeScreen::draw(Display& d, uint8_t) {
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  g.fillRect(0, 0, SCR_W, SCR_H, COL_BG);
  widgets::battery(g, 32, 36, valid ? (int)pct : -1, true, t.fg, t.dim, 3);
  widgets::textCentred(g, 0, SCR_W, 92, full ? "FULL" : "CHARGING", 3, t.fg);
}
