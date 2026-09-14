// The button-command engine (FirmwareSpec.md §4.2): debounced button states
// in, commands out. Pure logic (no Arduino headers) so the native tests can
// drive it tick by tick. One instance, fed every app tick with the two
// debounced button states; it returns at most one command per call and
// reports the hold progress for the countdown bar (§11.5).
#pragma once
#include <stdint.h>

namespace sb {

enum class ButtonCmd : uint8_t { None, VolumeDown, VolumeUp, Level1, PrevLevel, NextLevel, Off, Menu };
enum class HoldKind  : uint8_t { None, PrevLevel, NextLevel, Both };

const char* buttonCmdName(ButtonCmd c);

struct ButtonDurations {
  uint16_t attendantHoldMs = 1000;   // levelChange.attendantHoldMs
  uint16_t bothTapMs       = 400;    // levelChange.bothTapMs: both pressed and released within this = level 1
  uint16_t offHoldMs       = 1000;   // power.offHoldMs
  uint16_t menuHoldMs      = 3000;   // menu.holdMs
};

// What the countdown bar shows. For a single-button hold the bar fills toward
// attendantHoldMs. For a both-hold it fills toward offHoldMs; once that is
// reached `offReached` is set and the bar fills again toward menuHoldMs with
// `secondsLeft` counting down.
struct HoldProgress {
  HoldKind kind = HoldKind::None;
  uint8_t  pct = 0;
  bool     offReached = false;
  uint8_t  secondsLeft = 0;
  bool operator==(const HoldProgress& o) const { return kind == o.kind && pct == o.pct && offReached == o.offReached && secondsLeft == o.secondsLeft; }
  bool operator!=(const HoldProgress& o) const { return !(*this == o); }
};

class ButtonCommands {
 public:
  void setDurations(const ButtonDurations& d) { d_ = d; }
  // §4.2 rule 7: nothing fires before `t`; a button already down at `t` must be
  // released before anything counts.
  void enableAt(uint32_t t) { enableAt_ = t; enabled_ = false; }
  // §4.2 rule 3: after an OFF wake the both-hold cannot produce Off until both
  // buttons have been released once; Menu stays reachable.
  void suppressOffUntilRelease() { suppressOff_ = true; }
  // §4.2 rule 5: in MENU the both-hold exits the menu; clicks resolve on release everywhere (Draft 4).
  void setMenuMode(bool on) { menuMode_ = on; }

  // Feed the debounced states every tick. Returns the command that resolved on
  // this tick, or None.
  ButtonCmd feed(uint32_t now, bool minusDown, bool plusDown);
  HoldProgress progress(uint32_t now) const;
  bool anyDown() const { return minus_ || plus_; }
  bool enabled() const { return enabled_; }

 private:
  enum class St : uint8_t { Idle, Single, Both, Consumed };
  static ButtonCmd click(uint8_t btn) { return btn == 0 ? ButtonCmd::VolumeDown : ButtonCmd::VolumeUp; }
  static ButtonCmd longHold(uint8_t btn) { return btn == 0 ? ButtonCmd::PrevLevel : ButtonCmd::NextLevel; }

  ButtonDurations d_;
  St       st_ = St::Idle;
  uint8_t  btn_ = 0;                 // the button of a Single (0 = minus, 1 = plus)
  bool     minus_ = false, plus_ = false;
  uint32_t tDown_ = 0, tBoth_ = 0;
  bool     stage1_ = false;          // both-hold past offHoldMs
  bool     enabled_ = true;
  uint32_t enableAt_ = 0;
  bool     suppressOff_ = false;
  bool     menuMode_ = false;
};

}  // namespace sb
