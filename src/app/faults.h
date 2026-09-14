// Fault table (FirmwareSpec.md §18): a bit per condition, its status-line
// token and message. Adding a fault is one row.
#pragma once
#include <stdint.h>

enum FaultBit : uint16_t {
  F_CARD   = 1 << 0,   // SD does not mount
  F_CONFIG = 1 << 1,   // config.json unusable (unparsable or newer than the firmware)
  F_TOUCH  = 1 << 2,
  F_BT     = 1 << 3,
  F_AUDIO  = 1 << 4,
  F_KBD    = 1 << 5,
  F_WIFI   = 1 << 6,
  F_BATT   = 1 << 7,
  F_SAFE   = 1 << 8,
};

struct FaultDesc { uint16_t bit; const char* token; const char* message; };

static const FaultDesc FAULTS[] = {
  { F_CARD,   "!CARD",   "card not mounted: configuration from flash, sounds unavailable" },
  { F_CONFIG, "!CONFIG", "config.json unusable: backup, flash copy or defaults in use" },
  { F_TOUCH,  "!TOUCH",  "touch driver failed: pads dead" },
  { F_BT,     "!BT",     "Bluetooth speaker module not answering" },
  { F_AUDIO,  "!AUDIO",  "I2S failed: no audio" },
  { F_KBD,    "!KBD",    "BLE init failed: keyboard off" },
  { F_WIFI,   "!WIFI",   "Wi-Fi AP failed to start" },
  { F_BATT,   "!BATT",   "battery reading implausible" },
  { F_SAFE,   "!SAFE",   "safe mode after repeated crashes" },
};
static constexpr uint8_t N_FAULTS = sizeof(FAULTS) / sizeof(FAULTS[0]);
