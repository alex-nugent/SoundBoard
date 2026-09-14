// Built-in tones (FirmwareSpec.md §6.6): synthesised by the audio task from a
// table of segments, no card involved. Pure so it compiles natively.
#pragma once
#include <stdint.h>

namespace sb {

enum class ToneKind : uint8_t { MissingSound, Click, Startup, Saved, Fault };

class ToneGen {
 public:
  struct Seg { uint16_t hz; uint16_t ms; };     // hz 0 = silence
  void begin(ToneKind kind, float gain);
  // Fills `frames` mono samples; returns the number written (less than
  // `frames` only at the end; 0 when finished).
  uint32_t fill(int16_t* mono, uint32_t frames);
  bool done() const { return seg_ >= nSeg_; }

 private:
  const Seg* segs_ = nullptr;
  uint8_t  nSeg_ = 0, seg_ = 0;
  uint32_t left_ = 0;         // frames left in the current segment
  float    phase_ = 0, amp_ = 0;
};

}  // namespace sb
