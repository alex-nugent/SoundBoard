#include "app/pmode_model.h"
#include <math.h>

namespace sb {

void PModeModel::configure(float k, float sigmaS, bool twoSided) {
  if (k < 1e-6f) k = 1e-6f;
  if (k > 1.0f) k = 1.0f;
  k_ = k; two_ = twoSided;
  // A fair bit has variance 1/4; the EMA of an i.i.d. stream has variance k / (2 - k) times that.
  sigma_ = 0.5f * sqrtf(k / (2.0f - k));
  th_ = sigmaS * sigma_;
  samples_ = 0;
  for (uint8_t c = 0; c < CHANNELS; c++) trig_[c] = 0;
  reset();
}

void PModeModel::reset() {
  for (uint8_t c = 0; c < CHANNELS; c++) x_[c] = 0.5f;
}

int PModeModel::feed(uint32_t word) {
  samples_++;
  int hit = -1;
  for (uint8_t c = 0; c < CHANNELS; c++) {
    float bit = (word >> c) & 1u ? 1.0f : 0.0f;
    x_[c] += k_ * (bit - x_[c]);
    float d = x_[c] - 0.5f;
    if (hit < 0 && (d >= th_ || (two_ && -d >= th_))) hit = c;
  }
  if (hit >= 0) { trig_[hit]++; reset(); }
  return hit;
}

}  // namespace sb
