// Display module (FirmwareSpec.md §11): ST7789 240x135 landscape, theme,
// brightness fade, dirty-region redraw on a 50 ms tick, one slice <= 20 ms.
// Only the app task draws; the SPI bus is taken through the storage mutex.
#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "display/theme.h"
#include "display/screens/screens.h"
#include "config/config.h"

// Screens draw into a full-screen 16-bit canvas in PSRAM; the canvas records
// the rows a draw touched and the display pushes just those rows to the panel
// as one block (§11.10: a 240 x 66 push is ~8 ms, where the same text drawn
// glyph pixel by glyph pixel over SPI took ~40 ms and flickered).
class Canvas : public GFXcanvas16 {
 public:
  Canvas();
  bool allocate();                           // in Display::begin(): the PSRAM heap is not registered when static constructors run
  bool ok() const { return buffer != nullptr; }
  void resetSpan() { minY_ = 0x7FFF; maxY_ = -1; }
  bool touched() const { return maxY_ >= minY_; }
  int16_t minY() const { return minY_; }
  int16_t maxY() const { return maxY_; }
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void fillScreen(uint16_t color) override;
 private:
  void span(int16_t y0, int16_t y1);
  int16_t minY_ = 0x7FFF, maxY_ = -1;
};

class Display {
 public:
  Display();
  bool begin(const sb::Config& cfg, bool blockingFade = true);   // gated rail on, SPI, init, black, backlight fade in (blocking ~150 ms; a sleep-wake lets the tick fade)
  void powerDown();                          // backlight off, SPI released, every gated-device GPIO LOW (§12.4 order); rail left to the caller
  void onConfig(const sb::Config& cfg);      // theme, flip, brightness; redraws when they change
  void setScreen(Screen* s);                 // clears the panel and redraws everything over the next ticks
  void dirty(uint8_t regions);
  void drawNow();                            // draws everything pending at once (boot only)
  void tick(uint32_t now);                   // call every 5 ms: backlight fade; a redraw slice every 50 ms
  void setBrightness(uint8_t pct);           // fades over ~150 ms
  uint8_t brightness() const { return brightnessPct_; }

  Adafruit_GFX& gfx() { return canvas_; }   // screens draw here; drawSlice pushes the touched rows
  void push(int16_t y0, int16_t h);          // canvas rows -> panel, one block
  const Theme& theme() const { return THEMES[themeIdx_ < 6 ? themeIdx_ : 0]; }
  bool ready() const { return ready_; }
  bool pending() const { return dirty_ != 0 || clearPending_; }   // regions still to draw
  void drawSoon();                           // the next tick draws a slice at once (a level change, §5.3 step 2)

 private:
  void drawSlice(uint32_t budgetMs);
  void applyBacklight(uint8_t pwm);

  Adafruit_ST7789 tft_;
  Canvas   canvas_;
  Screen*  screen_ = nullptr;
  uint8_t  dirty_ = 0;
  bool     clearPending_ = false;
  bool     ready_ = false;
  uint32_t nextTick_ = 0;
  uint8_t  themeIdx_ = 0;
  bool     flip_ = false;
  uint8_t  brightnessPct_ = 80;
  uint8_t  blCur_ = 0, blTarget_ = 0;
};
