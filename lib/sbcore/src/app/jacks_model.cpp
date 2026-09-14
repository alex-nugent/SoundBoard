#include "app/jacks_model.h"

namespace sb {

static inline bool dueAt(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

JackResolved resolveJack(const Config& c, uint8_t levelIdx, const Entry* e) {
  if (levelIdx < c.levels.count && !c.levels.levels[levelIdx].jacks) return JackResolved::Off;
  if (e) {
    if (e->jack == JackMode::Follow) return JackResolved::Follow;
    if (e->jack == JackMode::Pulse)  return JackResolved::Pulse;
    if (e->jack == JackMode::Off)    return JackResolved::Off;
  }
  switch (c.jacks.mode) {
    case JACKS_FOLLOW: return JackResolved::Follow;
    case JACKS_PULSE:  return JackResolved::Pulse;
    default:           return JackResolved::Off;
  }
}

void JacksModel::press(uint8_t jack, JackResolved mode, uint16_t pressId, uint32_t now) {
  if (jack >= N || mode == JackResolved::Off) return;
  J& j = j_[jack];
  j.closed = true; j.owner = pressId;
  if (mode == JackResolved::Follow) {
    j.follow = true;
    j.until = maxFollowMs_ ? now + maxFollowMs_ : 0;
    if (maxFollowMs_ && !j.until) j.until = 1;
  } else {
    j.follow = false;
    j.until = now + pulseMs_; if (!j.until) j.until = 1;
  }
}

void JacksModel::close(uint8_t jack, uint16_t ms, uint32_t now) {
  if (jack >= N) return;
  J& j = j_[jack];
  j.closed = true; j.follow = false; j.owner = 0;
  j.until = now + ms; if (!j.until) j.until = 1;
}

void JacksModel::release(uint8_t jack, uint16_t pressId) {
  if (jack >= N) return;
  J& j = j_[jack];
  if (j.closed && j.follow && j.owner == pressId) j = J();
}

void JacksModel::stuck(uint8_t jack) {
  if (jack >= N) return;
  if (j_[jack].closed && j_[jack].follow) j_[jack] = J();
}

void JacksModel::levelChanged() { for (uint8_t k = 0; k < N; k++) if (j_[k].closed && j_[k].follow) j_[k] = J(); }

void JacksModel::allOff() { for (uint8_t k = 0; k < N; k++) j_[k] = J(); }

uint8_t JacksModel::tick(uint32_t now) {
  for (uint8_t k = 0; k < N; k++) if (j_[k].closed && j_[k].until && dueAt(now, j_[k].until)) j_[k] = J();
  return mask();
}

uint8_t JacksModel::mask() const {
  uint8_t m = 0;
  for (uint8_t k = 0; k < N; k++) if (j_[k].closed) m |= (uint8_t)(1u << k);
  return m;
}

}  // namespace sb
