// The normal view (FirmwareSpec.md §11.4, status row at scale 2 since CP-2:
// scale 1 was unreadable on the panel).
//   y=0    status row, scale 2: battery + %, VOL nn / MUTE
//   y=20   level line "3 VOLUME" (number and name together) at the largest
//          scale that fits the width, centred in the space down to the labels
//   y=86   labels row, one column per sound pad, scale 2 if every label fits
//   y=118  bottom line (§11.4): hold bar, a red message or fault, a link change in plain words, or a dim state word
// (§11.4 as amended at CP-2: the name is what she looks for, so it shares the
// big line with the number instead of sitting under it at scale 1.)
#include <Arduino.h>
#include "display/screens/screens.h"
#include "display/display.h"
#include "display/widgets.h"
#include "app/faults.h"
#include <stdio.h>
#include <string.h>

static constexpr int16_t TOP_H = 20;

static void drawTop(Adafruit_GFX& g, const NormalView& v, const Theme& t) {
  g.fillRect(0, 0, SCR_W, TOP_H, COL_BG);
  uint16_t battCol = v.lowBattery ? COL_ALERT : t.fg;
  // No "LEVEL" word at scale 2: it collided with the battery text and the
  // numeral below already says what it is (§11.4 change for Draft 4).
  if (v.lowBattery && v.battValid) {
    char s[24]; snprintf(s, sizeof s, "BATT LOW %u%%", v.battPct);
    widgets::text(g, 4, 2, s, 2, COL_ALERT);
  } else {
    widgets::battery(g, 4, 1, v.battValid ? (int)v.battPct : -1, v.usb, battCol, t.dim, 2);
  }
  if (v.muted) widgets::textRight(g, SCR_W - 4, 2, " MUTE ", 2, COL_BG, t.fg);
  else { char s[12]; snprintf(s, sizeof s, "VOL %u", v.volumePct); widgets::textRight(g, SCR_W - 4, 2, s, 2, t.fg); }
}

static void drawMain(Adafruit_GFX& g, const NormalView& v, const Theme& t) {
  int16_t h = (v.showLabels ? 86 : 118) - TOP_H;
  g.fillRect(0, TOP_H, SCR_W, h, COL_BG);
  if (v.overlay[0]) {                                        // §11.5: volume popup / press label over the numeral
    int16_t y = TOP_H + (h - 24) / 2 - (v.overlayBar ? 6 : 0);
    widgets::textCentred(g, 0, SCR_W, y, v.overlay, 3, t.fg);
    if (v.overlayBar) widgets::hbar(g, 40, y + 30, SCR_W - 80, 6, v.overlayPct, t.fg, COL_GRID);
    return;
  }
  char line[24];
  if (v.name[0]) snprintf(line, sizeof line, "%u %s", (unsigned)(v.level + 1), v.name);
  else snprintf(line, sizeof line, "%u", (unsigned)(v.level + 1));
  size_t len = strlen(line);
  uint8_t scale = 2;
  for (uint8_t s = 7; s >= 2; s--) {                        // largest scale that fits the width and the area
    int16_t w = (int16_t)((len * 6 - 1) * s);
    if (w <= SCR_W - 4 && 8 * s <= h - 4) { scale = s; break; }
  }
  int16_t y = TOP_H + (h - 8 * scale) / 2;
  if (v.flashNumeral) {                                      // inverted for 150 ms on a level change, or while the level pad is held
    g.fillRect(0, TOP_H, SCR_W, h, t.fg);
    widgets::textCentred(g, 0, SCR_W, y, line, scale, COL_BG, t.fg);
  } else widgets::textCentred(g, 0, SCR_W, y, line, scale, t.fg);
}

static void drawLabels(Adafruit_GFX& g, const NormalView& v, const Theme& t) {
  if (!v.showLabels) return;
  g.fillRect(0, 86, SCR_W, 32, COL_BG);
  uint8_t n = v.nSoundPads ? v.nSoundPads : 1;
  int16_t colW = SCR_W / n;
  uint8_t scale = 2;
  for (uint8_t i = 0; i < n; i++) if (widgets::textW(v.labels[i], 2) > colW - 4) { scale = 1; break; }
  for (uint8_t i = 0; i < n; i++) {
    int16_t x0 = i * colW;
    bool hi = (v.pressedPad == (int8_t)i);
    uint16_t fg = hi ? COL_BG : t.fg, bg = hi ? t.fg : COL_BG;
    if (hi) g.fillRect(x0, 86, colW, 32, t.fg);
    char s[25];
    widgets::clipText(v.labels[i], s, sizeof s, colW - 4, scale);
    int16_t y = scale == 2 ? 94 : 98;
    widgets::textCentred(g, x0, colW, y, s, scale, fg, bg);
  }
}

static void drawBottom(Adafruit_GFX& g, const NormalView& v, const Theme& t) {
  g.fillRect(0, 118, SCR_W, 17, COL_BG);
  if (v.holdKind) {                                          // §11.5 hold countdown bar: caption + 6 px bar along the bottom
    char cap[32];
    if (v.holdKind == 1) snprintf(cap, sizeof cap, "PREV LEVEL");
    else if (v.holdKind == 2) snprintf(cap, sizeof cap, "NEXT LEVEL");
    else if (!v.holdOffReached) snprintf(cap, sizeof cap, "OFF");
    else if (v.holdSeconds) snprintf(cap, sizeof cap, "OFF - release   MENU in %u", (unsigned)v.holdSeconds);
    else snprintf(cap, sizeof cap, "OFF - release");            // menu.enabled false: no second stage
    widgets::textCentred(g, 0, SCR_W, 119, cap, 1, t.fg);
    widgets::hbar(g, 0, 129, SCR_W, 6, v.holdPct, t.fg, COL_GRID);
    return;
  }
  // §11.4: plain words, scale 2 when the text fits the width, else scale 1, clipped.
  auto line = [&](const char* s, uint16_t col) {
    uint8_t sc = widgets::textW(s, 2) <= SCR_W - 4 ? 2 : 1;
    char buf[40]; widgets::clipText(s, buf, sizeof buf, SCR_W - 4, sc);
    widgets::textCentred(g, 0, SCR_W, sc == 2 ? 119 : 123, buf, sc, col);
  };
  if (v.message[0]) { line(v.message, COL_ALERT); return; }  // priority 2: load errors, stuck pad, in red
  char f[40] = "";                                           // priority 2: faults, in red
  for (uint8_t i = 0; i < N_FAULTS; i++) if (v.faults & FAULTS[i].bit) {
    if (f[0]) strlcat(f, " ", sizeof f);
    strlcat(f, FAULTS[i].token, sizeof f);
  }
  if (f[0]) { line(f, COL_ALERT); return; }
  if (v.link[0]) { line(v.link, t.fg); return; }             // priority 3: a link change
  if (v.stateWord[0]) line(v.stateWord, t.dim);              // priority 4: one dim word
}

void NormalScreen::draw(Display& d, uint8_t regions) {
  if (!view) return;
  Adafruit_GFX& g = d.gfx();
  const Theme& t = d.theme();
  if (regions & R_TOP)    drawTop(g, *view, t);
  if (regions & R_MAIN)   drawMain(g, *view, t);
  if (regions & R_LABELS) drawLabels(g, *view, t);
  if (regions & (R_BOTTOM | R_NAME)) drawBottom(g, *view, t);   // R_NAME: a message changed
}
