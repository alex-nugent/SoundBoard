#include "display/display.h"
#include "hal/pins.h"
#include "hal/board.h"
#include "hal/storage.h"
#include "util/timer.h"
#include <Arduino.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include "diag/log.h"

static constexpr uint8_t FADE_STEP = 9;      // 255 / 9 ≈ 28 ticks of 5 ms ≈ 140 ms (§11.3: 150 ms)

Display::Display() : tft_(&SPI, pins::TFT_CS, pins::TFT_DC, pins::TFT_RST) {}   // TFT_RST -1: software reset (Bug 1)

Canvas::Canvas() : GFXcanvas16(SCR_W, SCR_H, false) { buffer = nullptr; buffer_owned = false; }
bool Canvas::allocate() {
  if (buffer) return true;
  size_t bytes = (size_t)SCR_W * SCR_H * 2;                                            // 64.8 KB
  buffer = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  bool psram = buffer != nullptr;
  if (!buffer) buffer = static_cast<uint16_t*>(malloc(bytes));
  if (buffer) memset(buffer, 0, bytes);
  LOG_I("display", "canvas %u KB in %s", (unsigned)(bytes / 1024), !buffer ? "NOTHING (drawing disabled)" : psram ? "PSRAM" : "internal RAM");
  return buffer != nullptr;
}
void Canvas::span(int16_t y0, int16_t y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > SCR_H - 1) y1 = SCR_H - 1;
  if (y1 < y0) return;
  if (y0 < minY_) minY_ = y0;
  if (y1 > maxY_) maxY_ = y1;
}
void Canvas::drawPixel(int16_t x, int16_t y, uint16_t color) { span(y, y); GFXcanvas16::drawPixel(x, y, color); }
void Canvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) { span(y, (int16_t)(y + h - 1)); GFXcanvas16::fillRect(x, y, w, h, color); }
void Canvas::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) { span(y, y); GFXcanvas16::drawFastHLine(x, y, w, color); }
void Canvas::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) { span(y, (int16_t)(y + h - 1)); GFXcanvas16::drawFastVLine(x, y, h, color); }
void Canvas::fillScreen(uint16_t color) { span(0, SCR_H - 1); GFXcanvas16::fillScreen(color); }

void Display::push(int16_t y0, int16_t h) {
  if (!canvas_.ok() || h <= 0) return;
  tft_.drawRGBBitmap(0, y0, canvas_.getBuffer() + (size_t)y0 * SCR_W, SCR_W, h);       // one address window, one stream
}

static uint8_t pctToPwm(uint8_t pct) { return (uint8_t)((uint16_t)255 * (pct > 100 ? 100 : pct) / 100); }

void Display::applyBacklight(uint8_t pwm) { ledcWrite(pins::TFT_BLK, pwm); blCur_ = pwm; }

bool Display::begin(const sb::Config& cfg, bool blockingFade) {
  themeIdx_ = cfg.display.theme;
  flip_ = cfg.display.flip;
  brightnessPct_ = cfg.display.brightnessPct;

  canvas_.allocate();
  Board::gatedRail(true);                                            // §2.3 rule 3: rail first, 10 ms
  ledcAttachChannel(pins::TFT_BLK, pins::BLK_PWM_HZ, pins::PWM_BITS, pins::LEDC_CH_BACKLIGHT);   // channel 2 = timer 1; the motor owns channel 0
  applyBacklight(0);
  {
    Storage::Guard g;
    SPI.begin(pins::SPI_SCK, pins::SPI_MISO, pins::SPI_MOSI);
    tft_.init(135, 240);                                             // sends SWRESET
    tft_.setSPISpeed(pins::TFT_HZ);
    tft_.setRotation(flip_ ? 3 : 1);                                 // landscape 240x135
    tft_.fillScreen(COL_BG);                                         // black before the backlight rises: no white flash (§11.8)
    canvas_.fillScreen(COL_BG);
  }
  ready_ = true;
  blTarget_ = pctToPwm(brightnessPct_);
  while (blockingFade && blCur_ != blTarget_) {                      // cold boot: a blocking 150 ms fade-in; a sleep-wake fades from tick() (§17.3)
    uint8_t next = blCur_ + FADE_STEP > blTarget_ ? blTarget_ : blCur_ + FADE_STEP;
    applyBacklight(next);
    delay(5);
  }
  return true;
}

void Display::powerDown() {
  ready_ = false;
  applyBacklight(0);
  ledcDetach(pins::TFT_BLK);
  {
    Storage::Guard g;
    SPI.end();
  }
  for (int8_t p : { pins::SPI_SCK, pins::SPI_MOSI, pins::SD_CS, pins::TFT_CS, pins::TFT_DC, pins::TFT_BLK }) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  pinMode(pins::SPI_MISO, INPUT);
  blCur_ = blTarget_ = 0;
}

void Display::onConfig(const sb::Config& cfg) {
  bool redraw = false;
  if (cfg.display.theme != themeIdx_) { themeIdx_ = cfg.display.theme; redraw = true; }
  if (cfg.display.flip != flip_) {
    flip_ = cfg.display.flip;
    Storage::Guard g;
    tft_.setRotation(flip_ ? 3 : 1);
    redraw = true;
  }
  if (cfg.display.brightnessPct != brightnessPct_) { brightnessPct_ = cfg.display.brightnessPct; setBrightness(brightnessPct_); }
  if (redraw && screen_) { clearPending_ = true; dirty_ = R_ALL; }
}

void Display::setScreen(Screen* s) {
  screen_ = s;
  clearPending_ = true;
  dirty_ = R_ALL;
}

void Display::dirty(uint8_t regions) { dirty_ |= regions; }

void Display::drawSoon() { nextTick_ = millis(); }

void Display::setBrightness(uint8_t pct) { blTarget_ = pctToPwm(pct); }

void Display::drawSlice(uint32_t budgetMs) {
  if (!screen_ || !ready_) return;
  uint32_t t0 = millis();
  if (clearPending_) {
    Storage::Guard g;
    tft_.fillScreen(COL_BG);                                         // ~16 ms at 32 MHz: its own slice
    canvas_.fillScreen(COL_BG);
    clearPending_ = false;
    return;
  }
  while (dirty_ && (millis() - t0) < budgetMs) {
    uint8_t bit = dirty_ & (uint8_t)(-(int8_t)dirty_);               // lowest set bit
    dirty_ &= (uint8_t)~bit;
    Storage::Guard g;
    canvas_.resetSpan();
    uint32_t t1 = micros();
    screen_->draw(*this, bit);                                       // into the canvas
    uint32_t t2 = micros();
    if (canvas_.touched()) push(canvas_.minY(), (int16_t)(canvas_.maxY() - canvas_.minY() + 1));
    LOG_D("display", "region %u: draw %lu us, push %lu us (rows %d..%d), slice at +%lu ms", bit, (unsigned long)(t2 - t1), (unsigned long)(micros() - t2), canvas_.touched() ? canvas_.minY() : -1, canvas_.touched() ? canvas_.maxY() : -1, (unsigned long)(millis() - t0));
  }
}

void Display::drawNow() {
  while (clearPending_ || dirty_) drawSlice(1000);
}

void Display::tick(uint32_t now) {
  if (!ready_) return;
  if (blCur_ != blTarget_) {
    uint8_t next;
    if (blCur_ < blTarget_) next = (uint8_t)(blCur_ + FADE_STEP > blTarget_ ? blTarget_ : blCur_ + FADE_STEP);
    else next = (uint8_t)(blCur_ - FADE_STEP < blTarget_ ? blTarget_ : blCur_ - FADE_STEP);
    applyBacklight(next);
  }
  if (!due(now, nextTick_)) return;
  drawSlice(20);
  nextTick_ = (dirty_ || clearPending_) ? now : now + 50;         // more to draw: the next app tick continues (a level change: 3 regions, ~31 ms, in two slices ~5 ms apart)
}
