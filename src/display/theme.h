// Screen palette and layout constants (FirmwareSpec.md §11.2, Appendix C).
#pragma once
#include <stdint.h>

struct Theme { uint16_t fg; uint16_t dim; };

// Index = sb::Theme (the display.theme enum order: amber|yellow|white|green|cyan|red).
static constexpr Theme THEMES[6] = {
  { 0xFD20, 0x7A00 },   // amber
  { 0xFFE0, 0x7BE0 },   // yellow
  { 0xFFFF, 0x7BEF },   // white
  { 0x07E0, 0x03E0 },   // green
  { 0x07FF, 0x03EF },   // cyan
  { 0xF800, 0x7800 },   // red
};

constexpr uint16_t COL_BG    = 0x0000;
constexpr uint16_t COL_ALERT = 0xF800;
constexpr uint16_t COL_GRID  = 0x2104;
constexpr int16_t  SCR_W = 240, SCR_H = 135;

// Regions of the normal view (§11.4), used as dirty bits by every screen.
enum : uint8_t { R_TOP = 1, R_MAIN = 2, R_NAME = 4 /* transient message */, R_LABELS = 8, R_BOTTOM = 16, R_BARS = 32 /* §4.5 P mode bars */, R_ALL = 63 };
