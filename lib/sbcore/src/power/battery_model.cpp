#include "power/battery_model.h"

namespace sb {

static const uint16_t TABLE_MV[]  = { 4200, 4150, 4110, 4080, 4020, 3980, 3950, 3910, 3870, 3850, 3840, 3820, 3800, 3790, 3770, 3750, 3730, 3710, 3690, 3610, 3500 };
static const uint8_t  TABLE_PCT[] = {  100,   95,   90,   85,   80,   75,   70,   65,   60,   55,   50,   45,   40,   35,   30,   25,   20,   15,   10,    5,    0 };
static constexpr int  N = sizeof TABLE_MV / sizeof TABLE_MV[0];

uint8_t socFromMv(uint16_t mv) {
  if (mv >= TABLE_MV[0]) return 100;
  if (mv <= TABLE_MV[N - 1]) return 0;
  for (int i = 1; i < N; i++) {
    if (mv >= TABLE_MV[i]) {                       // between row i (lower voltage) and row i-1
      uint16_t hi = TABLE_MV[i - 1], lo = TABLE_MV[i];
      float f = (float)(mv - lo) / (float)(hi - lo);
      return (uint8_t)(TABLE_PCT[i] + f * (TABLE_PCT[i - 1] - TABLE_PCT[i]) + 0.5f);
    }
  }
  return 0;
}

void BatteryFilter::reset() { hist_[0] = hist_[1] = hist_[2] = 0; n_ = 0; ema_ = 0; primed_ = false; havePct_ = false; pct_ = 0; }

void BatteryFilter::seed(uint16_t mv, uint8_t pct) {
  hist_[0] = hist_[1] = hist_[2] = mv; n_ = 3;
  ema_ = mv; primed_ = true;
  pct_ = pct > 100 ? 100 : pct; havePct_ = true;
}

uint16_t BatteryFilter::push(uint16_t avgMv) {
  hist_[2] = hist_[1]; hist_[1] = hist_[0]; hist_[0] = avgMv;
  if (n_ < 3) n_++;
  uint16_t med;
  if (n_ < 3) med = avgMv;                                     // not enough history: the sample itself
  else {
    uint16_t a = hist_[0], b = hist_[1], c = hist_[2];
    med = (a > b) ? ((b > c) ? b : (a > c ? c : a)) : ((a > c) ? a : (b > c ? c : b));
  }
  if (!primed_) { ema_ = med; primed_ = true; }
  else ema_ += 0.25f * ((float)med - ema_);
  return filtered();
}

uint8_t BatteryFilter::percent(bool usb) {
  uint8_t p = socFromMv(filtered());
  if (!usb && havePct_ && p > pct_) p = pct_;                  // §12.1: never climbs because a load was removed
  pct_ = p; havePct_ = true;
  return p;
}

}  // namespace sb
