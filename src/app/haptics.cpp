#include "app/haptics.h"
#include "hal/pins.h"
#include "diag/log.h"
#include <Arduino.h>
#include "driver/gpio.h"

static const char* TAG = "motor";

void Haptics::begin() { apply(0); }

void Haptics::configure(const sb::Config& c) {
  enabled_ = c.vibration.enabled; strength_ = c.vibration.strengthPct;
  confirmMs_ = c.vibration.confirmPulseMs; pulseMs_ = c.levelChange.vibration.pulseMs;
  m_.configure(strength_, c.vibration.maxPatternMs);
  if (!enabled_) stop();
}

void Haptics::apply(uint8_t duty) {
  if (duty == lastDuty_) return;
  lastDuty_ = duty;
  ledcWrite(pins::MOTOR, duty);
}

void Haptics::levelCue(const sb::Config& c, uint8_t levelIdx, uint32_t now) {
  uint16_t p[sb::MAX_PATTERN];
  uint8_t n = sb::levelPattern(c, levelIdx, p, sb::MAX_PATTERN);
  if (!n) return;
  m_.pattern(p, n, now);
  LOG_I(TAG, "level %u pattern: %u segment(s), %lu ms", (unsigned)(levelIdx + 1), (unsigned)n, (unsigned long)m_.remainingMs(now));
  tick(now);
}

void Haptics::singlePulse(uint32_t now) {
  if (!enabled_) return;
  uint16_t p[1] = { pulseMs_ };
  m_.pattern(p, 1, now);
  tick(now);
}

bool Haptics::confirmPulse(uint32_t now) {
  if (!enabled_) return false;
  bool ok = m_.pulse(confirmMs_, now);
  if (ok) tick(now); else LOG_D(TAG, "confirmation pulse skipped: a pattern runs");
  return ok;
}

void Haptics::playPattern(const uint16_t* onOff, uint8_t n, uint32_t now) {
  if (!n) return;
  m_.pattern(onOff, n, now);
  LOG_I(TAG, "buzz: a %u-segment pattern from the page at %u %%, %lu ms", (unsigned)n, (unsigned)strength_, (unsigned long)m_.remainingMs(now));
  tick(now);
}

void Haptics::buzzTest(const sb::Config& c, uint8_t levelIdx, uint32_t now) {
  uint16_t p[sb::MAX_PATTERN];
  uint8_t n = sb::levelPattern(c, levelIdx, p, sb::MAX_PATTERN);
  if (!n) { LOG_W(TAG, "buzz: nothing to play (vibration %s, mode none or an empty pattern)", enabled_ ? "enabled" : "disabled"); return; }
  m_.pattern(p, n, now);
  LOG_I(TAG, "buzz: level %u pattern at %u %%, %lu ms", (unsigned)(levelIdx + 1), (unsigned)strength_, (unsigned long)m_.remainingMs(now));
  tick(now);
}

void Haptics::stop() { m_.stop(); apply(0); }

void Haptics::tick(uint32_t now) {
  apply(m_.tick(now));
  if (m_.clipped() && !clipLogged_) { clipLogged_ = true; LOG_W(TAG, "on-time capped at 5 s in any 10 s (§9.4): a pattern was clipped"); }
  if (!m_.clipped()) clipLogged_ = false;
}

void Haptics::printStatus(Print& out, uint32_t now) const {
  out.printf("motor: %s, strength %u%%, %s, %lu ms left, duty %u (LEDC reads %lu, pad %d)\n", enabled_ ? "enabled" : "disabled", (unsigned)strength_,
             m_.patternRunning() ? "pattern running" : (m_.active() ? "pulse running" : "idle"), (unsigned long)m_.remainingMs(now), (unsigned)lastDuty_,
             (unsigned long)ledcRead(pins::MOTOR), (int)gpio_get_level((gpio_num_t)pins::MOTOR));
}
