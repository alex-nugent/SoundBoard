// The level controller (FirmwareSpec.md §5.3): the current level index, the
// wrap rules shared by the level pad, the attendant buttons and the actions,
// which changes carry cues, and the return-to-level-1 timer. Pure logic (no
// Arduino headers); the app applies each LevelChange to the screen, the click,
// the motor pattern (Phase 6) and the RTC copy.
#pragma once
#include <stdint.h>

namespace sb {

enum class LevelSource : uint8_t { Pad, Attendant, Action, Return, Portal };

struct LevelPolicy {                  // levelChange.* (§13.3)
  bool     click = true;              // cues on a change by the pad, an action or the buttons
  bool     attendantCues = true;      // ... buttons only when this is also true
  bool     returnCue = true;          // the return to level 1 plays the level-1 cues
  uint16_t returnToFirstAfterS = 20;  // 0 = never
};

struct LevelChange {
  bool        changed = false;        // the index moved
  bool        cues = false;           // play the click (and the pattern from Phase 6)
  uint8_t     level = 0;              // the index now current, 0-based
  uint8_t     from = 0;
  LevelSource source = LevelSource::Pad;
};

class LevelController {
 public:
  // count >= 1. A shorter list clamps the current index (§5.3 "clamped to the new count").
  void configure(const LevelPolicy& p, uint8_t count);
  void restore(uint8_t idx);                              // a sleep-wake's RTC copy: clamped, no cues
  uint8_t current() const { return current_; }
  uint8_t count() const { return count_; }

  LevelChange set(uint8_t idx, LevelSource s);            // out of range -> level 1; the same level -> flash only
  LevelChange next(LevelSource s);                        // wraps [V3]
  LevelChange prev(LevelSource s);                        // wraps ([OPEN 18], drafted)

  // The return timer runs from the last input of any kind; a level change
  // from any source restarts it too, so a portal change never returns at once.
  void onInput(uint32_t now) { lastInput_ = now; }
  bool returnDue(uint32_t now) const;
  LevelChange tickReturnTimer(uint32_t now);              // set(0, Return) once due; otherwise changed == false
  uint32_t returnInMs(uint32_t now) const;                // 0 when the timer is not running

 private:
  bool cuesFor(LevelSource s, bool changed) const;
  LevelPolicy p_;
  uint8_t  count_ = 1, current_ = 0;
  uint32_t lastInput_ = 0;
};

}  // namespace sb
