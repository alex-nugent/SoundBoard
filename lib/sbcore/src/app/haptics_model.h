// Vibration scheduler (FirmwareSpec.md §9): the level cue pattern, the
// confirmation pulse, their priorities and the safety caps of §9.4. Pure
// logic: the app's Haptics driver turns the duty this returns into PWM.
#pragma once
#include <stdint.h>
#include "config/config.h"

namespace sb {

constexpr uint16_t HAPTICS_MIN_GAP_MS      = 50;     // §9.4 between "on" segments
constexpr uint16_t HAPTICS_MAX_ON_MS       = 2000;   // §9.4 single "on" segment
constexpr uint32_t HAPTICS_WINDOW_MS       = 10000;  // §9.4 on-time window ...
constexpr uint32_t HAPTICS_WINDOW_ON_CAP_MS = 5000;  // ... and its cap

// §9.3: the pattern for entering level `levelIdx` (0-based) into out[] as
// alternating on/off ms starting with "on"; returns the segment count (0 =
// nothing: mode none, or vibration disabled).
uint8_t levelPattern(const Config& c, uint8_t levelIdx, uint16_t* out, uint8_t max);

// §9.2: does this entry get the confirmation pulse? (entry.vibrate, else the global switch; never with vibration disabled)
bool resolveVibrate(const Config& c, const Entry& e);

class HapticsModel {
 public:
  void configure(uint8_t strengthPct, uint16_t maxPatternMs) { strength_ = strengthPct; maxPatternMs_ = maxPatternMs; }
  bool pulse(uint16_t ms, uint32_t now);                        // confirmation pulse; false = skipped because a pattern runs (§5.5)
  void pattern(const uint16_t* onOff, uint8_t n, uint32_t now); // level pattern: replaces whatever runs
  void stop();
  uint8_t tick(uint32_t now);                                   // the duty to drive now, 0..255 (call every tick)
  bool active() const { return n_ > 0; }
  bool patternRunning() const { return n_ > 0 && isPattern_; }
  uint8_t duty() const { return duty_; }
  uint32_t remainingMs(uint32_t now) const;                     // until the schedule ends (0 when idle)
  bool clipped() const { return clipped_; }                     // the 10 s cap clipped something since clearClipped()
  void clearClipped() { clipped_ = false; }

 private:
  void load(const uint16_t* onOff, uint8_t n, uint32_t now, bool isPattern);
  void startSegment(uint32_t start);                            // applies the window cap to an "on" segment
  uint32_t onInWindow(uint32_t now) const;
  void noteOn(uint32_t start, uint16_t ms);
  uint16_t seg_[MAX_PATTERN] = { 0 };
  uint8_t  n_ = 0, i_ = 0;
  bool     isPattern_ = false, onPhase_ = false;
  uint32_t segEnd_ = 0;
  uint8_t  strength_ = 100, duty_ = 0;
  uint16_t maxPatternMs_ = 4000;
  bool     clipped_ = false;
  struct OnRec { uint32_t start; uint16_t ms; };
  OnRec    hist_[32] = {};
  uint8_t  histN_ = 0, histHead_ = 0;
};

}  // namespace sb
