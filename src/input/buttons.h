// The two attendant buttons (FirmwareSpec.md §4.2): − on IO6, + on IO8, active
// low, polled every 5 ms from the app task with a 30 ms debounce. Edges are
// posted as BtnEdge events; the debounced states feed the command engine.
#pragma once
#include <stdint.h>

class Buttons {
 public:
  void begin();
  bool tick(uint32_t now);                 // returns true when a debounced edge happened this tick
  bool minusDown() const { return stable_[0]; }
  bool plusDown() const { return stable_[1]; }

 private:
  bool     raw_[2] = { false, false };
  bool     stable_[2] = { false, false };
  uint32_t tChange_[2] = { 0, 0 };
};
