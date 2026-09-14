// The Quick Menu screen (FirmwareSpec.md §14.2).
//   y=0    SETTINGS                       3 / 11     title and position, scale 2
//   y=30   On-board speakers                         item label, scale 2
//   y=58   <        OFF        >                     value, scale 3, arrows dim at the ends; an action: "press SAVE"
//   y=118  BACK    DOWN     UP     NEXT              one word above each pad, scale 2 (an action: BACK  SAVE  NEXT); or the both-hold exit bar
#include <Arduino.h>
#include "display/screens/screens.h"
#include "display/display.h"
#include "display/widgets.h"
#include <stdio.h>
#include <string.h>

static void drawTitle(Adafruit_GFX& g, const MenuView& v, const Theme& t) {
  g.fillRect(0, 0, SCR_W, 20, COL_BG);
  widgets::text(g, 4, 2, "SETTINGS", 2, t.fg);
  char pos[12]; snprintf(pos, sizeof pos, "%u / %u", (unsigned)(v.index + 1), (unsigned)v.count);
  widgets::textRight(g, SCR_W - 4, 2, pos, 2, t.dim);
}

// The value line at the largest scale (3, 2, 1) that fits `maxW`.
static void bigLine(Adafruit_GFX& g, int16_t y, const char* s, int16_t maxW, uint16_t col) {
  uint8_t sc = widgets::textW(s, 3) <= maxW ? 3 : widgets::textW(s, 2) <= maxW ? 2 : 1;
  char buf[32]; widgets::clipText(s, buf, sizeof buf, maxW, sc);
  widgets::textCentred(g, 0, SCR_W, (int16_t)(y + (24 - 8 * sc) / 2), buf, sc, col);
}

static void drawMain(Adafruit_GFX& g, const MenuView& v, const Theme& t) {
  g.fillRect(0, 20, SCR_W, 98, COL_BG);
  {
    uint8_t sc = widgets::textW(v.label, 2) <= SCR_W - 8 ? 2 : 1;
    char buf[40]; widgets::clipText(v.label, buf, sizeof buf, SCR_W - 8, sc);
    widgets::textCentred(g, 0, SCR_W, sc == 2 ? 30 : 34, buf, sc, t.fg);
  }
  const int16_t yVal = 58;
  if (v.result[0]) { bigLine(g, yVal, v.result, SCR_W - 8, t.fg); }
  else if (v.action) { bigLine(g, yVal, v.value, SCR_W - 8, t.fg); }
  else {
    widgets::text(g, 6, yVal, "<", 3, v.canDown ? t.fg : COL_GRID);
    widgets::textRight(g, SCR_W - 6, yVal, ">", 3, v.canUp ? t.fg : COL_GRID);
    bigLine(g, yVal, v.value, SCR_W - 60, t.fg);
  }
}

static void drawBottom(Adafruit_GFX& g, const MenuView& v, const Theme& t) {
  g.fillRect(0, 118, SCR_W, 17, COL_BG);
  if (v.holdBar) {                                           // a fresh both-hold: exit (and save) on the release
    widgets::textCentred(g, 0, SCR_W, 119, v.holdOffReached ? "EXIT - release" : "EXIT", 1, t.fg);
    widgets::hbar(g, 0, 129, SCR_W, 6, v.holdPct, t.fg, COL_GRID);
    return;
  }
  const int16_t colW = SCR_W / 4;
  for (uint8_t i = 0; i < 4; i++) {
    if (!v.keys[i][0]) continue;
    uint8_t sc = widgets::textW(v.keys[i], 2) <= colW - 2 ? 2 : 1;
    widgets::textCentred(g, (int16_t)(i * colW), colW, sc == 2 ? 119 : 123, v.keys[i], sc, t.fg);
  }
}

void MenuScreen::draw(Display& d, uint8_t regions) {
  if (!view) return;
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  if (regions & R_TOP)    drawTitle(g, *view, t);
  if (regions & R_MAIN)   drawMain(g, *view, t);
  if (regions & R_BOTTOM) drawBottom(g, *view, t);
}
