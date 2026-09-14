// Output jacks J1–J4 (FirmwareSpec.md §10): per-press mode resolution and
// the four relay state machines with press ownership (§5.2). Pure logic;
// the app's Jacks driver writes the relay pins from the mask.
#pragma once
#include <stdint.h>
#include "config/config.h"

namespace sb {

enum class JackResolved : uint8_t { Follow, Pulse, Off };

// §10: entry.jack, else levels[].jacks == false → Off, else jacks.mode. The
// level flag suppresses every closure on that level (§5.2 step 3), even an
// explicit entry mode. `e` may be null (the level pad).
JackResolved resolveJack(const Config& c, uint8_t levelIdx, const Entry* e);

class JacksModel {
 public:
  static constexpr uint8_t N = 4;
  void configure(uint16_t pulseMs, uint16_t maxFollowMs) { pulseMs_ = pulseMs; maxFollowMs_ = maxFollowMs; }
  void press(uint8_t jack, JackResolved mode, uint16_t pressId, uint32_t now);   // step 3; pulse mode again on a repeat (step 7)
  void close(uint8_t jack, uint16_t ms, uint32_t now);          // bench/portal test: closed for ms, owner 0
  void release(uint8_t jack, uint16_t pressId);                 // PadUp: opens the followed jack of that press only
  void stuck(uint8_t jack);                                     // PadStuck: opens a followed jack
  void levelChanged();                                          // every followed jack opens
  void allOff();                                                // boot, sleep entry, FAULT
  uint8_t tick(uint32_t now);                                   // closed mask (bit j = jack j) after the timeouts
  uint8_t mask() const;
  bool closed(uint8_t j) const { return j < N && j_[j].closed; }
  uint16_t owner(uint8_t j) const { return j < N ? j_[j].owner : 0; }

 private:
  struct J { bool closed = false, follow = false; uint16_t owner = 0; uint32_t until = 0; };
  J j_[N];
  uint16_t pulseMs_ = 500, maxFollowMs_ = 10000;
};

}  // namespace sb
