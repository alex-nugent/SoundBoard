// The USB CDC console (FirmwareSpec.md §4.4, §19.8): every command is a line
// ended by Enter; a one-character line is a short command; a line beginning
// `AT+` goes to the KCX (Phase 8). Commands of later phases answer with the
// phase that brings them.
#pragma once
#include <stdint.h>

class AppState;

namespace Console {
void begin(AppState* app);
void tick(uint32_t now);
}  // namespace Console
