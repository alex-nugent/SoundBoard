// P mode (FirmwareSpec.md §4.5): four running exponential moving averages, one
// per pad position, each fed one bit per sample from the hardware random
// number generator. A channel whose average strays more than S sigma from
// 0.5 fires that pad; every channel then restarts at 0.5. Pure logic: the
// task that draws the bits and the app that acts on a trigger are in
// src/app/pmode.*.
#pragma once
#include <stdint.h>
#include "config/config.h"

namespace sb {

class PModeModel {
 public:
  static constexpr uint8_t CHANNELS = N_PADS;

  // k: the EMA constant (x += k * (bit - x)); sigmaS: the threshold in sigma; twoSided: either direction counts.
  void configure(float k, float sigmaS, bool twoSided);
  void reset();                                            // every average back to 0.5

  // One 32-bit random word: bit c feeds channel c. Returns the channel that crossed its threshold, or -1.
  // A trigger resets all four channels (§4.5) before returning.
  int  feed(uint32_t word);

  float avg(uint8_t ch) const { return x_[ch < CHANNELS ? ch : 0]; }
  float k() const { return k_; }
  float sigma() const { return sigma_; }                   // the standing deviation of a fair channel: 0.5 * sqrt(k / (2 - k))
  float threshold() const { return th_; }                  // the distance from 0.5 that fires
  bool  twoSided() const { return two_; }
  uint32_t samples() const { return samples_; }            // words fed since the last reset()/configure()
  uint32_t triggers(uint8_t ch) const { return trig_[ch < CHANNELS ? ch : 0]; }

 private:
  float    x_[CHANNELS] = { 0.5f, 0.5f, 0.5f, 0.5f };
  float    k_ = 0.01f, sigma_ = 0.0f, th_ = 1.0f;
  bool     two_ = true;
  uint32_t samples_ = 0;
  uint32_t trig_[CHANNELS] = { 0, 0, 0, 0 };
};

}  // namespace sb
