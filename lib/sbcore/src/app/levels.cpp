#include "app/levels.h"

namespace sb {

void LevelController::configure(const LevelPolicy& p, uint8_t count) {
  p_ = p;
  count_ = count ? count : 1;
  if (current_ >= count_) current_ = (uint8_t)(count_ - 1);
}

void LevelController::restore(uint8_t idx) { current_ = idx < count_ ? idx : 0; }

bool LevelController::cuesFor(LevelSource s, bool changed) const {
  switch (s) {
    case LevelSource::Pad:       return p_.click;                                  // §5.3 step 3: every level-pad press, one level or twenty
    case LevelSource::Attendant: return p_.click && p_.attendantCues && changed;   // §4.2: at level 1 already, only the numeral flashes
    case LevelSource::Action:    return p_.click && changed;                       // §5.4: as the level pad
    case LevelSource::Return:    return p_.returnCue;                              // §5.3: the level-1 cues
    case LevelSource::Portal:    return false;                                     // screen only
  }
  return false;
}

LevelChange LevelController::set(uint8_t idx, LevelSource s) {
  LevelChange c;
  c.from = current_;
  c.source = s;
  if (idx >= count_) idx = 0;
  c.changed = idx != current_;
  current_ = idx;
  c.level = current_;
  c.cues = cuesFor(s, c.changed);
  return c;
}

LevelChange LevelController::next(LevelSource s) { return set((uint8_t)((current_ + 1) % count_), s); }
LevelChange LevelController::prev(LevelSource s) { return set((uint8_t)((current_ + count_ - 1) % count_), s); }

bool LevelController::returnDue(uint32_t now) const {
  if (current_ == 0 || p_.returnToFirstAfterS == 0) return false;
  return (int32_t)(now - (lastInput_ + (uint32_t)p_.returnToFirstAfterS * 1000UL)) >= 0;
}

uint32_t LevelController::returnInMs(uint32_t now) const {
  if (current_ == 0 || p_.returnToFirstAfterS == 0) return 0;
  uint32_t at = lastInput_ + (uint32_t)p_.returnToFirstAfterS * 1000UL;
  return (int32_t)(now - at) >= 0 ? 0 : at - now;
}

LevelChange LevelController::tickReturnTimer(uint32_t now) {
  if (!returnDue(now)) { LevelChange none; none.level = current_; none.from = current_; none.source = LevelSource::Return; return none; }
  LevelChange c = set(0, LevelSource::Return);
  lastInput_ = now;
  return c;
}

}  // namespace sb
