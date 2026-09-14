#include "input/button_commands.h"

namespace sb {

const char* buttonCmdName(ButtonCmd c) {
  switch (c) {
    case ButtonCmd::VolumeDown: return "VOLUME_DOWN";
    case ButtonCmd::VolumeUp:   return "VOLUME_UP";
    case ButtonCmd::Level1:     return "LEVEL1";
    case ButtonCmd::PrevLevel:  return "PREV_LEVEL";
    case ButtonCmd::NextLevel:  return "NEXT_LEVEL";
    case ButtonCmd::Off:        return "OFF";
    case ButtonCmd::Menu:       return "MENU";
    default:                    return "NONE";
  }
}

static inline bool elapsed(uint32_t now, uint32_t since, uint32_t ms) { return (int32_t)(now - (since + ms)) >= 0; }

ButtonCmd ButtonCommands::feed(uint32_t now, bool minusDown, bool plusDown) {
  minus_ = minusDown; plus_ = plusDown;
  const bool both = minus_ && plus_;
  const bool any  = minus_ || plus_;
  const uint8_t which = minus_ ? 0 : 1;          // meaningful only when exactly one button is down
  ButtonCmd out = ButtonCmd::None;

  if (!enabled_) {
    if ((int32_t)(now - enableAt_) < 0) return ButtonCmd::None;
    enabled_ = true;
    st_ = any ? St::Consumed : St::Idle;         // rule 7: a button held through the boot fires nothing
  }

  switch (st_) {
    case St::Idle:
      if (both)     { st_ = St::Both; tBoth_ = now; stage1_ = false; }
      else if (any) { st_ = St::Single; btn_ = which; tDown_ = now; }
      break;

    case St::Single:
      if (both) { st_ = St::Both; tBoth_ = now; stage1_ = false; break; }      // rule 1: the pending hold is cancelled
      if (!any) { st_ = St::Idle; out = click(btn_); break; }                  // a click resolves on release, at once (Draft 4: no double click)
      if (which != btn_) {                                                      // ours released and the other pressed within one tick
        out = click(btn_);
        btn_ = which; tDown_ = now;
        break;
      }
      if (elapsed(now, tDown_, d_.attendantHoldMs)) { st_ = St::Consumed; out = longHold(btn_); }
      break;

    case St::Both:
      if (both) {
        if (!stage1_ && elapsed(now, tBoth_, d_.offHoldMs)) stage1_ = true;
        if (elapsed(now, tBoth_, d_.menuHoldMs)) { st_ = St::Consumed; out = ButtonCmd::Menu; }
        break;
      }
      st_ = St::Consumed;                                                       // one or both released
      if (stage1_) { if (!suppressOff_) out = ButtonCmd::Off; }                 // stage 1 reached: OFF (rule 3 may suppress it)
      else if (!elapsed(now, tBoth_, d_.bothTapMs)) out = ButtonCmd::Level1;   // a quick tap of both: level 1
      break;

    case St::Consumed:
      break;
  }

  if (st_ == St::Consumed && !any) st_ = St::Idle;                              // rule 8: everything involved released
  if (!any) suppressOff_ = false;                                               // rule 3: "released once"
  return out;
}

HoldProgress ButtonCommands::progress(uint32_t now) const {
  HoldProgress p;
  if (!enabled_) return p;
  auto pctOf = [](uint32_t el, uint32_t full) -> uint8_t { if (!full) return 100; uint32_t v = el * 100 / full; return v > 100 ? 100 : (uint8_t)v; };
  switch (st_) {
    case St::Single: {
      // Shown once a press has outlived a click (150 ms) so clicks do not flicker the bar.
      uint32_t el = now - tDown_;
      if (el < 150) return p;
      p.kind = btn_ == 0 ? HoldKind::PrevLevel : HoldKind::NextLevel;
      p.pct = pctOf(el, d_.attendantHoldMs);
      return p;
    }
    case St::Both: {
      uint32_t el = now - tBoth_;
      if (el < 150) return p;                                   // a both-tap does not flicker the bar either
      p.kind = HoldKind::Both;
      if (el < d_.offHoldMs) { p.pct = pctOf(el, d_.offHoldMs); return p; }
      p.offReached = true;
      uint32_t span = d_.menuHoldMs > d_.offHoldMs ? d_.menuHoldMs - d_.offHoldMs : 1;
      p.pct = pctOf(el - d_.offHoldMs, span);
      uint32_t left = el < d_.menuHoldMs ? d_.menuHoldMs - el : 0;
      p.secondsLeft = (uint8_t)((left + 999) / 1000);
      return p;
    }
    default:
      return p;
  }
}

}  // namespace sb
