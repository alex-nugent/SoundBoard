// The update progress screen (FirmwareSpec.md §16): UPDATING, a bar, one line of state.
#include <Arduino.h>
#include "display/screens/screens.h"
#include "display/display.h"
#include "display/widgets.h"

void UpdateScreen::draw(Display& d, uint8_t) {
  if (!view) return;
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  g.fillRect(0, 0, SCR_W, SCR_H, COL_BG);
  widgets::textCentred(g, 0, SCR_W, 18, view->failed ? "UPDATE FAILED" : "UPDATING", 3, view->failed ? COL_ALERT : t.fg);
  widgets::hbar(g, 20, 62, SCR_W - 40, 12, view->pct, t.fg, COL_GRID);
  uint8_t sc = widgets::textW(view->line, 2) <= SCR_W - 8 ? 2 : 1;
  char buf[48]; widgets::clipText(view->line, buf, sizeof buf, SCR_W - 8, sc);
  widgets::textCentred(g, 0, SCR_W, sc == 2 ? 92 : 96, buf, sc, t.fg);
  widgets::textCentred(g, 0, SCR_W, 122, "keep the USB lead in", 1, t.dim);
}
