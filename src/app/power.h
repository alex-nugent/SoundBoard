// Boot-path power helpers (FirmwareSpec.md §17.1 steps 5-6, §12.3): run
// before the app starts, before any rail or screen. The sleep and off entries
// themselves are AppState methods (app/power.cpp).
#pragma once
#include <stdint.h>

class Display;

namespace Power {
bool confirmWakePress(uint16_t holdMs);       // §12.3 Off-wake by a button: still down after holdMs?
void rearmOff(bool usb);                      // back to OFF: buttons, charge-check timer, holds, deep sleep (never returns)
void chargeCheck(Display& d);                 // §12.3 Off-wake by timer: charging display when USB is present, then OFF (never returns)
}
