#include "audio/volume.h"

namespace sb {

bool Volume::step(int dir) {
  int v = (int)master_ + (dir < 0 ? -(int)step_ : (int)step_);
  uint8_t nv = clamp(v);
  bool changed = nv != master_ || muted_;
  master_ = nv; muted_ = false;
  if (nv != master_ || changed) dirty_ = dirty_ || changed;
  return changed;
}

bool Volume::set(uint8_t pct) {
  uint8_t nv = clamp(pct);
  bool changed = nv != master_ || muted_;
  master_ = nv; muted_ = false;
  if (changed) dirty_ = true;
  return changed;
}

bool Volume::mute(bool on) {
  if (muted_ == on) return false;
  muted_ = on;
  return true;
}

}  // namespace sb
