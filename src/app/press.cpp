#include "app/press.h"
#include "diag/log.h"
#include "util/timer.h"
#include "util/strutil.h"

static const char* TAG = "press";

void PressPipeline::armRepeat(uint32_t now) {
  p_.repeatAt = now + h_->pressConfig().press.repeatDelayMs;
  if (!p_.repeatAt) p_.repeatAt = 1;
}

void PressPipeline::onPadDown(uint8_t pos, uint16_t pressId, uint32_t now) {
  const sb::Config& c = h_->pressConfig();
  // A new press replaces the record: exclusivity (§4.1) means the previous pad
  // is up, so its repeat is over; its sound plays on or is interrupted below.
  p_ = Press();
  p_.id = pressId; p_.pos = pos; p_.held = true;
  const sb::Role role = pos < sb::N_PADS ? c.pads.roles[pos] : sb::Role::None;
  if (role == sb::Role::Level) {                                                  // §5.3
    p_.levelPad = true;
    h_->levelPad(pressId, now, false);
    if (c.press.levelRepeatWhileHeld) armRepeat(now);                             // step 7
    return;
  }
  if (role != sb::Role::Sound) return;
  int idx = sb::soundIndex(c, pos);
  uint8_t lvl = h_->pressLevel();
  if (idx < 0 || lvl >= c.levels.count) return;
  const sb::Entry& e = c.levels.levels[lvl].buttons[idx];
  if (!c.press.interrupt && h_->audioPlaying()) {                                 // §5.2 [V3]: ignored apart from step 0
    LOG_D(TAG, "P%u ignored: a sound is playing and press.interrupt is off", pos + 1);
    return;
  }
  bool blank = !e.sound[0] && !e.type[0] && !e.key[0] && e.action == sb::Action::None;   // §5.2: a blank entry runs steps 0 and 5 only
  if (!blank) {                                                                   // steps 2, 3 and 4 (mute silences audio only)
    h_->keyPress(e, pressId, now);
    h_->jackPress(pos, e, pressId, now, false);
    h_->confirmPulse(e, now);
  }
  h_->pressLabel(e, now);                                                         // step 5
  if (e.action != sb::Action::None) { h_->runAction(e, pressId, now); return; }   // §5.4; action entries never repeat
  if (!e.sound[0]) return;                                                        // blank entry: steps 0 and 5 only
  p_.hasSound = true;
  sb::copyStr(p_.sound, sizeof p_.sound, e.sound);
  p_.volumePct = e.volumePct;
  if (h_->muted()) { LOG_D(TAG, "P%u: muted", pos + 1); return; }                 // §5.4: every output silent
  h_->playSound(p_.sound, p_.volumePct, pressId, now, false);                     // steps 1 and 6 (the engine fades the old sound)
}

void PressPipeline::onPadUp(uint8_t pos, uint16_t pressId, uint32_t now) {
  h_->keyRelease(pressId, now);                                                   // §5.2: that press's held key up
  h_->jackRelease(pos, pressId);                                                  // §5.2: opens the followed jack of that press only
  if (pressId != p_.id) return;
  p_.held = false;
  p_.repeatAt = 0;                                                                // the sound is not cut on release [V3]
}

void PressPipeline::onPadStuck(uint8_t pos, uint32_t now) {
  h_->keyStuck(now);                                                              // §8.3: PadStuck releases a held key
  h_->jackStuck(pos);                                                             // §10: PadStuck opens a followed jack
  if (!p_.id || pos != p_.pos) return;
  p_.held = false;
  p_.repeatAt = 0;
}

void PressPipeline::onAudioDone(uint16_t requestId, bool interrupted, uint32_t now) {
  if (requestId != p_.id || interrupted || !p_.held || p_.levelPad || !p_.hasSound) return;
  if (!h_->pressConfig().press.repeatWhileHeld) return;
  armRepeat(now);                                                                 // step 7: repeatDelayMs after the sound ends
}

void PressPipeline::onLevelChanged() { if (!p_.levelPad) p_.repeatAt = 0; }

void PressPipeline::cancel() { p_.repeatAt = 0; p_.held = false; }

void PressPipeline::tick(uint32_t now) {
  if (!p_.repeatAt || !due(now, p_.repeatAt)) return;
  p_.repeatAt = 0;
  if (!p_.held || !h_->padHeld(p_.pos)) return;                                   // released meanwhile
  const sb::Config& c = h_->pressConfig();
  if (p_.levelPad) {
    if (!c.press.levelRepeatWhileHeld) return;
    p_.repeats++;
    h_->levelPad(p_.id, now, true);                                               // §5.3 step 7: step 1 again
    armRepeat(now);
    return;
  }
  if (!c.press.repeatWhileHeld) return;
  p_.repeats++;
  int idx = sb::soundIndex(c, p_.pos);
  uint8_t lvl = h_->pressLevel();
  if (idx >= 0 && lvl < c.levels.count) {                                         // steps 3 (pulse mode only) and 4 again
    const sb::Entry& e = c.levels.levels[lvl].buttons[idx];
    h_->jackPress(p_.pos, e, p_.id, now, true);
    h_->confirmPulse(e, now);
  }
  if (h_->muted()) return;
  h_->playSound(p_.sound, p_.volumePct, p_.id, now, true);                        // step 6 again; the next AudioDone re-arms
}
