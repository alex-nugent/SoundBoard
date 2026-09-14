// SoundBoard V4 firmware entry (FirmwareSpec.md §17.1, §19.1).
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "hal/board.h"
#include "hal/pins.h"
#include "power/rtc_state.h"
#include "diag/log.h"
#include "app/state.h"
#include "app/power.h"
#include <hal/touch_sensor_ll.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// §19.1: the Arduino loop task gets 16 KB (the core creates it with 8 KB).
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// §2.6 / §16: initArduino() would mark a pending OTA image valid before
// setup(); returning true leaves it pending so the app marks it itself once
// its health check passes (rollback otherwise).
extern "C" bool verifyRollbackLater() { return true; }

static AppState app;
static BootInfo bootInfo;

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on / EN pin";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brown-out";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
  }
}

static const char* wakeName(esp_sleep_wakeup_cause_t w) {
  switch (w) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
    case ESP_SLEEP_WAKEUP_EXT1:      return "button (ext1)";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "touch pad";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    default:                         return "other";
  }
}

void setup() {
  // Steps 1-2 (§2.3 rules 1-2): IO39 LOW is the first statement; every load off.
  Board::earlyPins();
  // The touch sensor's live active mask, read at once: on an any-pad wake it is the only record of which
  // pad woke the chip (the sleep-pad register only holds a single configured channel), and it clears
  // when the finger lifts.
  { uint32_t m = 0; touch_ll_read_trigger_status_mask(&m); bootInfo.touchActiveMask = m; }
  vTaskPrioritySet(NULL, 2);                       // §19.1: app task priority 2
  bootInfo.appStartMs = millis();

  // Step 3: reset reason, wake cause, RtcState.
  bootInfo.reset = esp_reset_reason();
  bootInfo.wake  = esp_sleep_get_wakeup_cause();
  bool deepWake  = bootInfo.reset == ESP_RST_DEEPSLEEP && bootInfo.wake != ESP_SLEEP_WAKEUP_UNDEFINED;
  rtc::boot(bootInfo.reset, deepWake);
  bootInfo.rtcValid = rtc::valid();

  // Step 4: recovery check (never on a deep-sleep wake).
  bootInfo.bothButtonsAtReset = !deepWake && Board::buttonMinusDown() && Board::buttonPlusDown();

  // Steps 5-6 (§12.3): Off-wake confirmation and the charge-check timer, before any rail or screen.
  if (deepWake && rtc::get().kind == RTC_OFF) {
    if (bootInfo.wake == ESP_SLEEP_WAKEUP_EXT1) {
      bootInfo.wakeButtons = Board::wakeButtons();
      if (!Power::confirmWakePress(rtc::get().early.wakeHoldMs)) Power::rearmOff(Board::usbPresent());   // released early: OFF again, screen never lit
    } else if (bootInfo.wake == ESP_SLEEP_WAKEUP_TIMER) {
      Power::chargeCheck(app.display());                                                                    // never returns
    }
  }
  // Step 7: boot kind; a touch wake latches its channel (§4.1).
  bootInfo.kind = !deepWake ? BootKind::Cold : (rtc::get().kind == RTC_OFF ? BootKind::OffWake : BootKind::SleepWake);
  if (deepWake && bootInfo.wake == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    for (uint8_t ch = 2; ch <= 5 && !bootInfo.wakeTouchGpio; ch++) if (bootInfo.touchActiveMask & (1u << ch)) bootInfo.wakeTouchGpio = ch;   // S3: touch channel n == GPIO n
  } else if (deepWake && bootInfo.wake == ESP_SLEEP_WAKEUP_EXT1) bootInfo.wakeButtons = Board::wakeButtons();

  // Step 8: console; wait for the host only when USB is present. The USB
  // receive queue defaults to 256 bytes, which drops most of a pasted
  // `merge <json>` line before the 5 ms loop can drain it: size it for one
  // console line.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  uint32_t t0 = millis();
  if (Board::usbPresent()) { while (!Serial && millis() - t0 < 500) delay(5); }
  bootInfo.serialWaitMs = millis() - t0;
  Log::begin();
  LOG_I("boot", "=== SoundBoard V4 firmware %s (built %s %s) ===", FW_VERSION, __DATE__, __TIME__);
  LOG_I("boot", "kind %s | reset %s | wake %s | boot #%lu | crashes %u%s | rtc %s",
        bootInfo.kind == BootKind::Cold ? "cold" : bootInfo.kind == BootKind::SleepWake ? "sleep-wake" : "off-wake",
        resetName(bootInfo.reset), wakeName(bootInfo.wake), (unsigned long)rtc::get().bootCount,
        (unsigned)rtc::get().crashCount, rtc::isCrashReason(bootInfo.reset) ? " (this boot follows a crash)" : "",
        bootInfo.rtcValid ? "kept" : "reset");
  LOG_I("boot", "app start +%lu ms | serial wait %lu ms (USB %s) | recovery prompt %s",
        (unsigned long)bootInfo.appStartMs, (unsigned long)bootInfo.serialWaitMs, Board::usbPresent() ? "present" : "absent",
        bootInfo.bothButtonsAtReset ? "shown" : "no");
  if (deepWake) LOG_I("boot", "wake detail: touch active mask 0x%04lx -> GPIO %u%s | buttons %s%s", (unsigned long)bootInfo.touchActiveMask, bootInfo.wakeTouchGpio,
                      (bootInfo.wake == ESP_SLEEP_WAKEUP_TOUCHPAD && !bootInfo.wakeTouchGpio) ? " (pad not identified: released before the app started)" : "",
                      (bootInfo.wakeButtons & 1) ? "-" : "", (bootInfo.wakeButtons & 2) ? "+" : "");

  // Step 9: Serial1 and IO13, the audio rail, on their way while the screen and card come up (§6.7).
  app.audioEarlyStart();

  app.begin(bootInfo);

  // §19.1: the app task subscribes to the 5 s task watchdog and feeds it every iteration.
  esp_err_t w = esp_task_wdt_add(NULL);
  if (w != ESP_OK) LOG_W("boot", "task WDT subscribe: %d", (int)w);
  LOG_I("boot", "setup done at +%lu ms", (unsigned long)millis());
}

void loop() {
  uint32_t t0 = millis();
  esp_task_wdt_reset();
  app.tick();
  uint32_t spent = millis() - t0;
  if (spent < 5) delay(5 - spent);                 // the 5 ms app tick (§19.1)
}
