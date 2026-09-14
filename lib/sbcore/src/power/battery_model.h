// The battery model (FirmwareSpec.md §12.1): state of charge from the filtered
// voltage, the median-then-EMA filter, and the monotonic rule while on battery.
// Pure logic; the ADC, the calibration and the RTC copy live in the app.
#pragma once
#include <stdint.h>

namespace sb {

uint8_t socFromMv(uint16_t mv);                 // linear interpolation in the §12.1 table, 0..100

class BatteryFilter {
 public:
  void reset();
  void seed(uint16_t mv, uint8_t pct);          // a sleep-wake restores the filter and the displayed percentage
  uint16_t push(uint16_t avgMv);                // one 32-sample average in: median of the last 3, then EMA (alpha 0.25); returns the filtered mV
  uint8_t percent(bool usb);                    // from the filtered mV; never rises unless USB is present
  uint16_t filtered() const { return (uint16_t)(ema_ + 0.5f); }
  bool primed() const { return primed_; }

 private:
  uint16_t hist_[3] = { 0, 0, 0 };
  uint8_t  n_ = 0;
  float    ema_ = 0;
  bool     primed_ = false, havePct_ = false;
  uint8_t  pct_ = 0;
};

}  // namespace sb
