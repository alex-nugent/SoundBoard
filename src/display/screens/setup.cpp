// The setup card (FirmwareSpec.md §15.2): shown from SETUP entry until the
// first pad or button, and again after 10 s without one.
//   y=2    WI-FI SETUP                    RECOVERY     title, scale 2 (the banner in red when recovery)
//   y=28   SoundBoard-1A2B                             network name, scale 2
//   y=52   password soundboard                         scale 2 (scale 1 when it does not fit)
//   y=80   http://192.168.4.1                          scale 2
//   y=118  1 phone connected                           scale 1, dim
#include <Arduino.h>
#include "display/screens/screens.h"
#include "display/display.h"
#include "display/widgets.h"
#include <stdio.h>
#include <string.h>

void SetupScreen::draw(Display& d, uint8_t) {
  if (!view) return;
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  const SetupView& v = *view;
  g.fillRect(0, 0, SCR_W, SCR_H, COL_BG);
  widgets::text(g, 4, 2, "WI-FI SETUP", 2, t.fg);
  if (v.recovery) widgets::textRight(g, SCR_W - 4, 2, "RECOVERY", 2, COL_ALERT);
  {
    char buf[36]; widgets::clipText(v.ssid, buf, sizeof buf, SCR_W - 8, 2);
    widgets::textCentred(g, 0, SCR_W, 28, buf, 2, t.fg);
  }
  {
    char line[40]; snprintf(line, sizeof line, "password %s", v.password);
    uint8_t sc = widgets::textW(line, 2) <= SCR_W - 8 ? 2 : 1;
    char buf[40]; widgets::clipText(line, buf, sizeof buf, SCR_W - 8, sc);
    widgets::textCentred(g, 0, SCR_W, sc == 2 ? 52 : 56, buf, sc, t.fg);
  }
  {
    uint8_t sc = widgets::textW(v.url, 2) <= SCR_W - 8 ? 2 : 1;
    widgets::textCentred(g, 0, SCR_W, sc == 2 ? 80 : 84, v.url, sc, t.fg);
  }
  char ph[32];
  if (v.clients == 0) snprintf(ph, sizeof ph, "no phone connected yet");
  else snprintf(ph, sizeof ph, "%u phone%s connected", (unsigned)v.clients, v.clients == 1 ? "" : "s");
  widgets::textCentred(g, 0, SCR_W, 120, ph, 1, t.dim);
}
