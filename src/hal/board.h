// Board / HAL (FirmwareSpec.md §19.4): the early pin states and the rails.
#pragma once
#include <stdint.h>

namespace Board {

// §2.3 rules 1-2: IO39 LOW first, every load off, buttons pulled up, shield input.
// Must be the first call in setup().
void earlyPins();

// §2.3 rule 3: GPIO17 -> 3V3_GATED (screen + SD). On: HIGH then 10 ms.
void gatedRail(bool on);
bool gatedRailOn();

bool usbPresent();          // VBUS_SENSE

// Sleep (§12.3, §12.4): buttons as ext1 any-low wake with RTC pull-ups and the
// RTC peripheral domain kept on; IO39, IO13 and GPIO17 held LOW through the sleep.
void armButtonsWake();
void holdForSleep();
void deepSleep();           // never returns
uint8_t wakeButtons();      // ext1 status after a wake: bit 0 = minus, bit 1 = plus
bool buttonMinusDown();     // U4 (IO6), active low
bool buttonPlusDown();      // U5 (IO8), active low

}  // namespace Board
