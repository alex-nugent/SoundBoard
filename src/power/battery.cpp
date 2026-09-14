#include "power/battery.h"
#include "power/rtc_state.h"
#include "hal/pins.h"
#include "hal/board.h"
#include "diag/log.h"
#include "util/timer.h"
#include <Arduino.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <sys/time.h>

static const char* TAG = "batt";
static constexpr float K_DESIGN = 3.7625f;      // 442 k / 160 k divider (rev B, healthy rev A)
static constexpr float TCOEF    = 0.0035f;      // pull-down leg: +0.35 %/degC of die temperature (validation 03)

static int64_t nowUs() { struct timeval tv; gettimeofday(&tv, nullptr); return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec; }

bool Battery::store() {
  Preferences p;
  if (!p.begin("sb-cal", false)) return false;
  p.putString("method", method_ == Pulldown ? "pulldown" : "divider");
  p.putFloat("k", k_); p.putFloat("tcal", tcal_);
  p.end();
  return true;
}

void Battery::begin() {
  Preferences p;
  bool have = false;
  if (p.begin("sb-cal", true)) {
    if (p.isKey("method")) {
      String m = p.getString("method", "divider");
      method_ = m == "pulldown" ? Pulldown : Divider;
      k_ = p.getFloat("k", k_); tcal_ = p.getFloat("tcal", tcal_);
      have = true;
    }
    p.end();
  }
  if (!have) {                                                 // §12.1: import validation 03's calibration once
    Preferences v;
    if (v.begin("battcal", true)) {
      if (v.isKey("k")) {
        float k = v.getFloat("k", 0);
        if (k >= 3.0f && k <= 9.0f) { method_ = Pulldown; k_ = k; tcal_ = 33.0f; store(); LOG_I(TAG, "imported validation 03's K %.3f into sb-cal (pull-down leg, tcal 33 C)", (double)k); }
      }
      v.end();
    }
  }
  kBad_ = method_ == Pulldown && (k_ < 3.0f || k_ > 9.0f);
  if (kBad_) LOG_W(TAG, "K %.3f is outside 3.0-9.0: readings invalid until `batt <volts>`", (double)k_);
  analogSetAttenuation(ADC_2_5db);
  analogReadMilliVolts(pins::VBAT);                            // attaches the channel and the calibration handle
  filter_.reset();
  sampleNow();
  nextSample_ = millis() + 10000; forceAt_ = millis() + 60000;
  LOG_I(TAG, "method %s%s: %u mV, %u %%, USB %s", methodName(), method_ == Pulldown ? String(String(" K ") + String(k_, 3) + " @ " + String(tcal_, 1) + " C").c_str() : "",
        (unsigned)mv_, (unsigned)pct_, usb_ ? "present" : "absent");
}

void Battery::restore(const RtcState& r) {
  if (r.battMv >= 2500 && r.battMv <= 4500) {
    filter_.seed(r.battMv, r.battPct);
    fullSinceUs_ = r.fullSinceUs;
    mv_ = r.battMv; pct_ = r.battPct; valid_ = true;
    LOG_I(TAG, "filter restored from the sleep: %u mV, %u %%", (unsigned)mv_, (unsigned)pct_);
    sampleNow();
  }
}

void Battery::save(RtcState& r) const { r.battMv = mv_; r.battPct = pct_; r.fullSinceUs = fullSinceUs_; }

uint16_t Battery::readAdc(bool pulldown) {
  analogSetAttenuation(ADC_2_5db);
  analogReadMilliVolts(pins::VBAT);
  if (pulldown) { gpio_pulldown_en((gpio_num_t)pins::VBAT); delay(5); }     // validation 03's order: attach, then the leg, then 5 ms
  uint32_t acc = 0;
  for (int i = 0; i < 32; i++) acc += analogReadMilliVolts(pins::VBAT);
  if (pulldown) gpio_pulldown_dis((gpio_num_t)pins::VBAT);
  return (uint16_t)(acc / 32);
}

uint16_t Battery::readRawMv(uint16_t& adcMv) {
  bool pd = method_ == Pulldown;
  adcMv = readAdc(pd);
  float k = pd ? k_ * (1.0f - TCOEF * (temperatureRead() - tcal_)) : K_DESIGN;
  float mv = adcMv * k;
  if (mv > 65535.0f) mv = 65535.0f;
  return (uint16_t)(mv + 0.5f);
}

bool Battery::sampleNow() {
  uint16_t adc;
  uint16_t mv = readRawMv(adc);
  if (fakeMv_) mv = fakeMv_;                                   // bench: `batt fake <V>`
  bool usbWas = usb_;
  usb_ = fakeNoUsb_ ? false : Board::usbPresent();
  lastSample_ = millis();
  bool wasValid = valid_;
  if (kBad_ || mv < 2500 || mv > 4500) {
    if (wasValid) LOG_W(TAG, "implausible reading %u mV (ADC %u mV): percentage shown as --", (unsigned)mv, (unsigned)adc);
    valid_ = false;
  } else {
    uint16_t f = filter_.push(mv);
    pct_ = filter_.percent(usb_);
    mv_ = f;
    valid_ = true;
    if (usb_ && f >= 4180) {                                   // §12.2: full after 5 min at >= 4.18 V on USB while awake
      if (!fullSinceUs_) fullSinceUs_ = nowUs();
      full_ = (nowUs() - fullSinceUs_) >= 5LL * 60LL * 1000000LL;
    } else { fullSinceUs_ = 0; full_ = false; }
  }
  if (usb_ != usbWas && wasValid)                              // a bench aid: the raw sample before the change is the last one on the other supply
    LOG_I(TAG, "USB %s: this sample %u mV raw, previous %u mV raw, filtered %u mV, %u %%", usb_ ? "present" : "absent", (unsigned)mv, (unsigned)prevRaw_, (unsigned)mv_, (unsigned)pct_);
  prevRaw_ = mv;
  uint8_t sig = (uint8_t)((valid_ ? 1 : 0) | (usb_ ? 2 : 0) | (full_ ? 4 : 0));
  if (sig != lastSig_ || (valid_ && wasValid && pct_ != filter_.percent(usb_))) changed_ = true;
  static uint8_t lastPct = 255;
  if (valid_ && pct_ != lastPct) { changed_ = true; lastPct = pct_; }
  lastSig_ = sig;
  return valid_;
}

void Battery::tick(uint32_t now, bool quiet) {
  if (!due(now, nextSample_)) return;
  if (!quiet && !due(now, forceAt_)) return;                   // wait for a quiet window, at most 60 s
  sampleNow();
  nextSample_ = now + 10000;
  forceAt_ = now + 60000;
}

bool Battery::changed() { bool c = changed_; changed_ = false; return c; }

bool Battery::calibrate(float meterVolts, char* msg, size_t n) {
  if (meterVolts < 3.0f || meterVolts > 4.4f) { snprintf(msg, n, "meter reading %.3f V is not a LiPo voltage (3.0-4.4)", (double)meterVolts); return false; }
  uint16_t adc = readAdc(true);
  if (adc < 100) { snprintf(msg, n, "ADC reads %u mV with the pull-down leg: no signal", (unsigned)adc); return false; }
  float k = meterVolts * 1000.0f / (float)adc;
  if (k < 3.0f || k > 9.0f) { snprintf(msg, n, "K = %.3f is outside 3.0-9.0 (ADC %u mV): not stored", (double)k, (unsigned)adc); return false; }
  method_ = Pulldown; k_ = k; tcal_ = temperatureRead(); kBad_ = false;
  bool ok = store();
  filter_.reset();
  sampleNow();
  snprintf(msg, n, "K = %.3f at %.1f C (ADC %u mV), method pulldown%s; now %u mV, %u %%", (double)k_, (double)tcal_, (unsigned)adc, ok ? ", stored in sb-cal" : " (NVS write FAILED)", (unsigned)mv_, (unsigned)pct_);
  return ok;
}

bool Battery::clearCalibration() {
  method_ = Divider; kBad_ = false;
  bool ok = store();
  filter_.reset();
  sampleNow();
  return ok;
}

void Battery::printStatus(Print& out) const {
  if (valid_) out.printf("battery %.2f V %u%%", mv_ / 1000.0, (unsigned)pct_);
  else out.print("battery -- (reading implausible)");
  out.printf(" | USB %s%s | method %s", usb_ ? "present" : "absent", usb_ ? (full_ ? " (full)" : " (charging)") : "", methodName());
  if (method_ == Pulldown) out.printf(" K %.3f @ %.1f C (die now %.1f C)", (double)k_, (double)tcal_, (double)temperatureRead());
  out.printf(" | sampled %lu s ago\n", (unsigned long)((millis() - lastSample_) / 1000));
}

void Battery::setFake(uint16_t mv, bool noUsb) {
  fakeMv_ = mv; fakeNoUsb_ = mv && noUsb;
  filter_.reset();                                             // re-seeded by the first sample, so the fake shows at once
  fullSinceUs_ = 0;
  sampleNow();
  if (mv) LOG_W(TAG, "battery FAKE %u mV -> %u %%%s (bench only)", (unsigned)mv, (unsigned)pct_, fakeNoUsb_ ? ", USB faked absent" : "");
  else LOG_I(TAG, "battery fake off: real readings again");
}
