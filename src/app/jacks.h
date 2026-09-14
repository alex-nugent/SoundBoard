// Output jack driver (FirmwareSpec.md §10): the sbcore state machine on the
// four TLP241A relays, one per touch channel (pins::RELAY).
#pragma once
#include <stdint.h>
#include <Print.h>
#include "app/jacks_model.h"      // sbcore state machine
#include "config/config.h"

class Jacks {
 public:
  void begin();
  void configure(const sb::Config& c);
  int  jackOf(uint8_t pos) const { return pos < sb::N_PADS ? jackOf_[pos] : -1; }
  void press(uint8_t pos, sb::JackResolved mode, uint16_t pressId, uint32_t now);
  void release(uint8_t pos, uint16_t pressId);
  void stuck(uint8_t pos);
  void levelChanged();
  void allOff();                                                       // boot, sleep entry, FAULT: pins LOW at once
  void closeTest(uint8_t jack, uint16_t ms, uint32_t now);             // console `j1`..`j4` / portal test
  void tick(uint32_t now);
  uint8_t mask() const { return applied_; }
  void printStatus(Print& out) const;

 private:
  void apply(uint8_t mask);
  sb::JacksModel m_;
  uint8_t applied_ = 0;
  int8_t  jackOf_[sb::N_PADS] = { -1, -1, -1, -1 };
};
