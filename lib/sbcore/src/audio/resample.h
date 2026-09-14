// Linear resampling to 44 100 Hz (FirmwareSpec.md §6.3). Pure. The block
// form is used by the loader (whole file in memory); the stateful form by the
// streamer, which converts as the data arrives.
#pragma once
#include <stdint.h>

namespace sb {

constexpr uint32_t CANON_RATE = 44100;

// out must hold outFrames samples; returns the number written.
inline uint32_t resampleMono(const int16_t* in, uint32_t inFrames, uint32_t inRate, int16_t* out, uint32_t outFrames) {
  if (!inFrames || !inRate) return 0;
  if (inRate == CANON_RATE) { uint32_t n = inFrames < outFrames ? inFrames : outFrames; for (uint32_t i = 0; i < n; i++) out[i] = in[i]; return n; }
  uint64_t step = ((uint64_t)inRate << 16) / CANON_RATE;      // input frames per output frame, 16.16
  uint64_t pos = 0;
  uint32_t n = 0;
  for (; n < outFrames; n++) {
    uint32_t i = (uint32_t)(pos >> 16);
    if (i + 1 >= inFrames) { if (i >= inFrames) break; out[n] = in[i]; pos += step; continue; }
    uint32_t frac = (uint32_t)(pos & 0xFFFF);
    int32_t a = in[i], b = in[i + 1];
    out[n] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
    pos += step;
  }
  return n;
}

// Stateful: feed mono input samples, pull canonical output samples. The
// state is the last input sample consumed (`prev_`) and the 16.16 fractional
// position between it and the next input sample.
class LinearResampler {
 public:
  void begin(uint32_t inRate) { step_ = ((uint64_t)inRate << 16) / CANON_RATE; frac_ = 0; prev_ = 0; havePrev_ = false; passthrough_ = (inRate == CANON_RATE); }
  bool passthrough() const { return passthrough_; }
  // Produces up to outFrames samples from `in` (inFrames available); returns
  // frames written and sets `consumed` to the input frames used up.
  uint32_t run(const int16_t* in, uint32_t inFrames, uint32_t& consumed, int16_t* out, uint32_t outFrames) {
    consumed = 0;
    if (passthrough_) { uint32_t n = inFrames < outFrames ? inFrames : outFrames; for (uint32_t i = 0; i < n; i++) out[i] = in[i]; consumed = n; return n; }
    uint32_t idx = 0, n = 0;
    if (!havePrev_) { if (!inFrames) return 0; prev_ = in[0]; idx = 1; havePrev_ = true; frac_ = 0; }
    while (n < outFrames) {
      while (frac_ >= 65536) { if (idx >= inFrames) goto done; prev_ = in[idx++]; frac_ -= 65536; }
      if (idx >= inFrames) break;
      int32_t a = prev_, b = in[idx];
      out[n++] = (int16_t)(a + (((b - a) * (int32_t)frac_) >> 16));
      frac_ += step_;
    }
  done:
    consumed = idx;
    return n;
  }
 private:
  uint64_t step_ = 1 << 16, frac_ = 0;
  int16_t  prev_ = 0;
  bool     havePrev_ = false, passthrough_ = true;
};

}  // namespace sb
