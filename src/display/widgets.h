// Text and icon helpers over Adafruit GFX (6x8 font at integer scales).
#pragma once
#include <Adafruit_GFX.h>
#include <stdint.h>
#include <stddef.h>
#include "display/theme.h"

namespace widgets {

int16_t textW(const char* s, uint8_t scale);                                         // advance width (6 px per char per scale)
void    clipText(const char* s, char* out, size_t n, int16_t maxW, uint8_t scale);  // truncates with ".." to fit
void    text(Adafruit_GFX& g, int16_t x, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg = COL_BG);
void    textCentred(Adafruit_GFX& g, int16_t x0, int16_t w, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg = COL_BG);
void    textRight(Adafruit_GFX& g, int16_t xRight, int16_t y, const char* s, uint8_t scale, uint16_t fg, uint16_t bg = COL_BG);
void    battery(Adafruit_GFX& g, int16_t x, int16_t y, int pct, bool usb, uint16_t fg, uint16_t dim, uint8_t scale = 1);   // pct < 0 = unknown; icon 20x9 px at scale 1
void    hbar(Adafruit_GFX& g, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t fg, uint16_t bg);

}  // namespace widgets
