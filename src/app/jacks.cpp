#include "app/jacks.h"
#include "hal/pins.h"
#include "diag/log.h"
#include <Arduino.h>

static const char* TAG = "jacks";

void Jacks::begin() { m_.allOff(); apply(0); }

void Jacks::configure(const sb::Config& c) {
  m_.configure(c.jacks.pulseMs, c.jacks.maxFollowMs);
  for (uint8_t pos = 0; pos < sb::N_PADS; pos++) {
    uint8_t gpio = c.hardware.padChannels[pos];
    jackOf_[pos] = (gpio >= 2 && gpio <= 5) ? (int8_t)(gpio - 2) : -1;   // J1..J4 sit on touch channels 2..5 (§2.1)
  }
}

void Jacks::apply(uint8_t mask) {
  for (uint8_t k = 0; k < sb::JacksModel::N; k++) {
    bool want = (mask >> k) & 1, have = (applied_ >> k) & 1;
    if (want == have) continue;
    digitalWrite(pins::RELAY[k], want ? HIGH : LOW);
    LOG_D(TAG, "J%u %s", (unsigned)(k + 1), want ? "closed" : "open");
  }
  applied_ = mask;
}

void Jacks::press(uint8_t pos, sb::JackResolved mode, uint16_t pressId, uint32_t now) {
  int j = jackOf(pos);
  if (j < 0 || mode == sb::JackResolved::Off) return;
  m_.press((uint8_t)j, mode, pressId, now);
  LOG_I(TAG, "J%u %s [press %u]", (unsigned)(j + 1), mode == sb::JackResolved::Follow ? "closed while held" : "pulse", (unsigned)pressId);
  apply(m_.mask());
}

void Jacks::release(uint8_t pos, uint16_t pressId) { int j = jackOf(pos); if (j >= 0) { m_.release((uint8_t)j, pressId); apply(m_.mask()); } }
void Jacks::stuck(uint8_t pos)                    { int j = jackOf(pos); if (j >= 0) { m_.stuck((uint8_t)j); apply(m_.mask()); } }
void Jacks::levelChanged()                        { m_.levelChanged(); apply(m_.mask()); }
void Jacks::allOff()                              { m_.allOff(); apply(0); }

void Jacks::closeTest(uint8_t jack, uint16_t ms, uint32_t now) {
  if (jack >= sb::JacksModel::N) return;
  m_.close(jack, ms, now);
  LOG_I(TAG, "J%u closed for %u ms (test)", (unsigned)(jack + 1), (unsigned)ms);
  apply(m_.mask());
}

void Jacks::tick(uint32_t now) { apply(m_.tick(now)); }

void Jacks::printStatus(Print& out) const {
  out.printf("jacks: closed [%s%s%s%s] | P1..P4 -> J%d J%d J%d J%d\n",
             (applied_ & 1) ? "J1 " : "", (applied_ & 2) ? "J2 " : "", (applied_ & 4) ? "J3 " : "", (applied_ & 8) ? "J4" : "",
             jackOf_[0] + 1, jackOf_[1] + 1, jackOf_[2] + 1, jackOf_[3] + 1);
}
