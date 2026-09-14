// Vibration motor driver (FirmwareSpec.md §9): the sbcore scheduler on
// LEDC channel 0 / IO39. The pin is LOW whenever the module is idle.
#pragma once
#include <stdint.h>
#include <Print.h>
#include "app/haptics_model.h"    // sbcore scheduler
#include "config/config.h"

class Haptics {
 public:
  void begin();
  void configure(const sb::Config& c);
  void levelCue(const sb::Config& c, uint8_t levelIdx, uint32_t now);   // §9.3, replaces a running pattern
  void singlePulse(uint32_t now);                                       // §5.3 return-to-level-1 cue
  bool confirmPulse(uint32_t now);                                      // §9.2; false = skipped (pattern running) or disabled
  void buzzTest(const sb::Config& c, uint8_t levelIdx, uint32_t now);   // console `v` / portal Buzz
  void stop();                                                          // OFF, shutdown, UPDATING: pin LOW at once
  void tick(uint32_t now);
  bool active() const { return m_.active(); }
  bool patternRunning() const { return m_.patternRunning(); }
  uint32_t remainingMs(uint32_t now) const { return m_.remainingMs(now); }
  void printStatus(Print& out, uint32_t now) const;

 private:
  void apply(uint8_t duty);
  sb::HapticsModel m_;
  bool     enabled_ = true;
  uint8_t  strength_ = 100, lastDuty_ = 0;
  uint16_t confirmMs_ = 200, pulseMs_ = 150;
  bool     clipLogged_ = false;
};
