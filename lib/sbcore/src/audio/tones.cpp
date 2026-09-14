#include "audio/tones.h"
#include <math.h>

namespace sb {



namespace {
typedef ToneGen::Seg Seg;
const Seg MISSING[] = { { 880, 60 }, { 660, 60 } };
const Seg CLICK[]   = { { 1200, 15 } };
const Seg STARTUP[] = { { 523, 150 }, { 659, 150 }, { 784, 150 }, { 1047, 150 } };
const Seg SAVED[]   = { { 660, 80 }, { 880, 80 } };
const Seg FAULT[]   = { { 220, 100 }, { 0, 100 }, { 220, 100 }, { 0, 100 }, { 220, 100 } };
constexpr float TWO_PI_F = 6.28318530718f;
constexpr uint32_t RATE = 44100;
}

void ToneGen::begin(ToneKind kind, float gain) {
  const Seg* s; uint8_t n;
  switch (kind) {
    case ToneKind::MissingSound: s = MISSING; n = 2; break;
    case ToneKind::Click:        s = CLICK;   n = 1; break;
    case ToneKind::Startup:      s = STARTUP; n = 4; break;
    case ToneKind::Saved:        s = SAVED;   n = 2; break;
    default:                     s = FAULT;   n = 5; break;
  }
  segs_ = s; nSeg_ = n; seg_ = 0;
  left_ = (uint32_t)segs_[0].ms * RATE / 1000;
  phase_ = 0;
  amp_ = 9000.0f * gain * 2.0f;                       // as the validations' test tone (§6.6)
  if (amp_ > 32000.0f) amp_ = 32000.0f;
}

uint32_t ToneGen::fill(int16_t* mono, uint32_t frames) {
  uint32_t n = 0;
  while (n < frames && seg_ < nSeg_) {
    if (!left_) { seg_++; if (seg_ >= nSeg_) break; left_ = (uint32_t)segs_[seg_].ms * RATE / 1000; phase_ = 0; continue; }
    uint16_t hz = segs_[seg_].hz;
    if (!hz) { mono[n++] = 0; left_--; continue; }
    // 2 ms fade at each segment edge against clicks.
    uint32_t segLen = (uint32_t)segs_[seg_].ms * RATE / 1000, pos = segLen - left_;
    float env = 1.0f;
    const uint32_t edge = RATE * 2 / 1000;
    if (pos < edge) env = (float)pos / edge;
    else if (left_ <= edge) env = (float)left_ / edge;
    mono[n++] = (int16_t)(amp_ * env * sinf(phase_));
    phase_ += TWO_PI_F * hz / RATE;
    if (phase_ > TWO_PI_F) phase_ -= TWO_PI_F;
    left_--;
  }
  return n;
}

}  // namespace sb
