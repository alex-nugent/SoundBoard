#include "input/buttons.h"
#include "hal/board.h"
#include "util/event.h"
#include "util/timer.h"

static constexpr uint32_t DEBOUNCE_MS = 30;

void Buttons::begin() {
  stable_[0] = raw_[0] = Board::buttonMinusDown();
  stable_[1] = raw_[1] = Board::buttonPlusDown();
}

bool Buttons::tick(uint32_t now) {
  bool edge = false;
  bool in[2] = { Board::buttonMinusDown(), Board::buttonPlusDown() };
  for (uint8_t i = 0; i < 2; i++) {
    if (in[i] != raw_[i]) { raw_[i] = in[i]; tChange_[i] = now; }
    if (raw_[i] != stable_[i] && due(now, tChange_[i] + DEBOUNCE_MS)) {
      stable_[i] = raw_[i];
      edge = true;
      Event e = { Ev::BtnEdge, now, {} };
      e.u32 = (uint32_t)i | ((uint32_t)(stable_[i] ? 1 : 0) << 8);
      EventBus::post(e);
    }
  }
  return edge;
}
