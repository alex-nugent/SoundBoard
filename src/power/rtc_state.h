// RtcState (FirmwareSpec.md §12.4): kept through deep sleep and through
// software, panic and watchdog resets; lost on power-on. `kind` is trusted
// only when the wake cause is a deep-sleep wake; the rest whenever magic and
// crc hold.
#pragma once
#include <stdint.h>
#include <esp_system.h>

struct RtcState {
  uint32_t magic;                 // 0x5B4D0002
  uint8_t  kind;                  // 0 none, 1 Sleep, 2 Off
  int64_t  sleptAtUs;             // gettimeofday at sleep entry
  uint32_t baselines[4];          // touch baselines by channel index
  uint8_t  level;                 // current level (Sleep only)
  uint8_t  volumePct;             // master volume (mirrors NVS)
  bool     muted;
  bool     wokeFromOff;
  bool     usbAtSleep;
  uint16_t battMv;  uint8_t battPct;  int64_t fullSinceUs;      // battery filter state (§12.1, §12.2)
  struct { uint16_t wakeHoldMs, chargeCheckMin, ownerLabelMs; bool showChargingWhenOff;
           char ownerLabel[3][21]; uint8_t ownerLines;
           uint8_t theme, dimPct; bool flip; } early;             // settings needed before the card is read (§12.3; theme/flip/dimPct for the charging display)
  uint8_t  crashCount;  int64_t firstCrashUs;                   // panic / watchdog resets in the window (§18)
  uint32_t bootCount;
  bool     consoleSeen;           // a terminal sent a byte while USB stayed plugged: stream the log at once after a reset
  uint32_t crc;                   // over everything above
};

enum RtcKind : uint8_t { RTC_NONE = 0, RTC_SLEEP = 1, RTC_OFF = 2 };

namespace rtc {

RtcState& get();
bool valid();                                   // magic + crc held at boot
bool isCrashReason(esp_reset_reason_t r);       // panic, task WDT, interrupt WDT
// Validates (or resets) the state at boot, counts the boot and any crash.
void boot(esp_reset_reason_t reason, bool deepSleepWake);
void commit();                                  // recompute the crc after any change
void clearCrashes();
bool safeModeDue();                             // §18: three crashes within 2 minutes (call after boot())
void setEarlyDefaults();                        // the §13.3 defaults for the early-boot settings

}  // namespace rtc
