#include "display/screens/screens.h"
#include "display/display.h"
#include "display/widgets.h"

void FaultScreen::draw(Display& d, uint8_t regions) {
  if (!(regions & R_MAIN)) return;
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  g.fillRect(0, 0, SCR_W, SCR_H, COL_BG);
  widgets::textCentred(g, 0, SCR_W, 24, "FAULT", 3, COL_ALERT);
  char line[40];
  widgets::clipText(reason, line, sizeof line, SCR_W - 8, 1);
  widgets::textCentred(g, 0, SCR_W, 68, line, 1, t.fg);
  widgets::textCentred(g, 0, SCR_W, 110, "press RESET", 1, t.dim);
}
