#include "power/rtc_state.h"
#include <Arduino.h>
#include <esp_rom_crc.h>
#include <sys/time.h>
#include <string.h>

// RTC_NOINIT_ATTR: the bootloader reloads .rtc.data from flash on every reset
// that is not a deep-sleep wake, which would wipe the crash and boot counters;
// .rtc_noinit is never reloaded, so magic + crc decide whether it is usable.
static RTC_NOINIT_ATTR RtcState g_rtc;
static bool s_valid = false;

static constexpr uint32_t MAGIC = 0x5B4D0002;

static uint32_t computeCrc() {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&g_rtc), offsetof(RtcState, crc));
}

static int64_t nowUs() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

namespace rtc {

RtcState& get() { return g_rtc; }
bool valid() { return s_valid; }

bool isCrashReason(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT;
}

void setEarlyDefaults() {
  g_rtc.early.wakeHoldMs = 400;
  g_rtc.early.chargeCheckMin = 10;
  g_rtc.early.ownerLabelMs = 2000;
  g_rtc.early.showChargingWhenOff = true;
  memset(g_rtc.early.ownerLabel, 0, sizeof g_rtc.early.ownerLabel);
  strlcpy(g_rtc.early.ownerLabel[0], "If found please call", 21);
  strlcpy(g_rtc.early.ownerLabel[1], "(phone number)", 21);
  g_rtc.early.ownerLines = 2;
  g_rtc.early.theme = 0; g_rtc.early.flip = false; g_rtc.early.dimPct = 10;
}

void boot(esp_reset_reason_t reason, bool deepSleepWake) {
  s_valid = (g_rtc.magic == MAGIC) && (computeCrc() == g_rtc.crc);
  if (!s_valid) {
    memset(&g_rtc, 0, sizeof g_rtc);
    g_rtc.magic = MAGIC;
    setEarlyDefaults();
  }
  if (!deepSleepWake) g_rtc.kind = RTC_NONE;          // `kind` is trusted only on a deep-sleep wake
  g_rtc.bootCount++;
  if (isCrashReason(reason)) {
    if (g_rtc.crashCount == 0) g_rtc.firstCrashUs = nowUs();
    if (g_rtc.crashCount < 255) g_rtc.crashCount++;
  }
  commit();
}

void commit() { g_rtc.crc = computeCrc(); }

void clearCrashes() { g_rtc.crashCount = 0; g_rtc.firstCrashUs = 0; commit(); }

bool safeModeDue() {                            // the system clock runs on through panic and watchdog resets
  if (g_rtc.crashCount < 3) return false;
  int64_t span = nowUs() - g_rtc.firstCrashUs;
  return span >= 0 && span < 120LL * 1000000LL;
}

}  // namespace rtc
