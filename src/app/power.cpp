// SLEEP and OFF (FirmwareSpec.md §12.3, §12.4) and the boot-path helpers of
// §17.1 steps 5-6.
#include "app/state.h"
#include "app/power.h"
#include "app/faults.h"
#include "hal/board.h"
#include "hal/pins.h"
#include "hal/storage.h"
#include "power/rtc_state.h"
#include "power/battery.h"
#include "config/loader.h"
#include "diag/log.h"
#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

static const char* TAG = "power";

static int64_t nowUs() { struct timeval tv; gettimeofday(&tv, nullptr); return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec; }
static void waitMs(uint32_t ms) { uint32_t t0 = millis(); while (millis() - t0 < ms) { esp_task_wdt_reset(); delay(5); } }

// ---------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------
bool AppState::sleepAllowed() const {                          // §3.2
  if (mode_ != AppMode::Active && mode_ != AppMode::Dimmed) return false;
  if (audio_.playing()) return false;
  if (haptics_.active()) return false;                         // §9.4: SLEEP entry waits for the pattern to end
  if (buttons_.minusDown() || buttons_.plusDown()) return false;
  if (hintShown_ || touch_.calibrating()) return false;
  if (recoveryRequested_) return false;
  return true;
}

void AppState::fillRtcForSleep(uint8_t kind) {                 // §12.3 SLEEP step 2 / OFF step 2
  RtcState& r = rtc::get();
  r.kind = kind;
  r.sleptAtUs = nowUs();
  for (uint8_t k = 0; k < 4; k++) r.baselines[k] = touch_.baseline(k);
  r.level = level_;
  r.volumePct = volume_.master();
  r.muted = volume_.muted();
  r.wokeFromOff = kind == RTC_OFF ? false : wokeFromOff_;
  r.usbAtSleep = Board::usbPresent();
  battery_.save(r);
  refreshRtcEarly();                                           // commits
}

void AppState::quiesce() {                                     // §12.3 "Quiesce": <= 300 ms
  haptics_.stop(); jacks_.allOff();                            // §9.4 / §10: OFF stops the motor at once, every relay opens
  kbd_.shutdown(millis());                                     // §8.3: keys up, host dropped, advertising stopped
  if (audio_.playing()) {
    audio_.stop();
    uint32_t t0 = millis();
    while (audio_.playing() && millis() - t0 < 300) { esp_task_wdt_reset(); delay(5); }
  }
  cache_.joinLoader();                                         // loader cancelled, files closed
  if (volume_.dirty()) volumeWriteThunk(this);                 // audio is silent now: the pending volume write
  touch_.storeBaselinesIfMoved();                              // the baseline copy (§4.1)
  uint32_t t0 = millis();
  while (Storage::pending() && millis() - t0 < 400) { esp_task_wdt_reset(); Storage::tick(millis()); delay(5); }
}

void AppState::powerDownForSleep() {                           // §12.4 power-down order
  cache_.joinLoader();
  store_.unmountCard();
  display_.powerDown();                                        // backlight 0, ledcDetach(7), SPI.end(), gated pins LOW, MISO input
  Board::gatedRail(false);                                     // GPIO17 LOW
  audio_.railDown();                                           // audio task: I2S stop, pins LOW, IO42 LOW, Serial1.end + TX LOW, IO13 LOW
  uint32_t t0 = millis();
  while (audio_.rail() != AudioEngine::Rail::Down && millis() - t0 < 1000) { esp_task_wdt_reset(); delay(5); }
  if (audio_.rail() != AudioEngine::Rail::Down) LOG_W(TAG, "audio rail did not report down within 1 s");
  ampOn_ = false; digitalWrite(pins::AMP_EN, LOW);
  for (int8_t p : pins::RELAY) digitalWrite(p, LOW);
  digitalWrite(pins::MOTOR, LOW);
}

// ---------------------------------------------------------------------------
// SLEEP (§12.3)
// ---------------------------------------------------------------------------
void AppState::enterSleep(const char* why) {
  LOG_I(TAG, "SLEEP: %s (level %u, volume %u%%%s, battery %u%%)", why, (unsigned)(level_ + 1), (unsigned)volume_.master(), volume_.muted() ? " muted" : "", (unsigned)battery_.percent());
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  quiesce();                                                   // step 1 (nothing plays: the checks of §3.2 passed)
  fillRtcForSleep(RTC_SLEEP);                                  // step 2
  Log::tick(); Serial.flush(); delay(20);
  display_.setBrightness(0);                                   // §11.8: fade to 0 over ~150 ms
  waitMs(160); display_.tick(millis());
  powerDownForSleep();                                         // step 3
  bool touchOk = touch_.armSleepWake();                        // step 4: hardware thresholds, any-pad wake
  Board::armButtonsWake();
  if (!touchOk) LOG_W(TAG, "touch wake not armed: buttons only");
  Board::holdForSleep();
  Board::deepSleep();                                          // step 5
}

// ---------------------------------------------------------------------------
// OFF (§12.3)
// ---------------------------------------------------------------------------
void AppState::enterOff(const char* why) {
  LOG_I(TAG, "OFF: %s", why);
  uint32_t t0 = millis();
  msgScreen_.text = "OFF"; msgScreen_.sub = nullptr; msgScreen_.scale = 5;
  if (display_.ready()) { display_.setScreen(&msgScreen_); display_.drawNow(); }   // step 1: "OFF" while the quiesce runs
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  quiesce();
  fillRtcForSleep(RTC_OFF);                                    // step 2: wokeFromOff cleared, usbAtSleep
  while (millis() - t0 < 500) { esp_task_wdt_reset(); delay(5); }   // §11.8: readable for 500 ms
  Log::tick(); Serial.flush(); delay(20);
  display_.setBrightness(0); waitMs(160); display_.tick(millis());
  powerDownForSleep();                                         // step 3
  touch_.disableWake();                                        // pads dead in OFF
  Board::armButtonsWake();
  const RtcState& r = rtc::get();
  if (r.early.showChargingWhenOff)
    esp_sleep_enable_timer_wakeup((uint64_t)(r.usbAtSleep ? r.early.chargeCheckMin : 60) * 60ULL * 1000000ULL);
  Board::holdForSleep();
  Board::deepSleep();                                          // step 4
}

// ---------------------------------------------------------------------------
// Boot path (§17.1 steps 5-6), before the app: no rail, no screen unless charging
// ---------------------------------------------------------------------------
namespace Power {

bool confirmWakePress(uint16_t holdMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < holdMs) {
    if (!Board::buttonMinusDown() && !Board::buttonPlusDown()) return false;
    delay(5);
  }
  return true;
}

void rearmOff(bool usb) {
  const RtcState& r = rtc::get();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  Board::armButtonsWake();
  if (r.early.showChargingWhenOff)
    esp_sleep_enable_timer_wakeup((uint64_t)(usb ? r.early.chargeCheckMin : 60) * 60ULL * 1000000ULL);
  Board::holdForSleep();
  Board::deepSleep();
}

void chargeCheck(Display& d) {
  RtcState& r = rtc::get();
  bool usb = Board::usbPresent();
  if (usb && r.early.showChargingWhenOff) {
    Battery b;
    b.begin();
    bool full = false;
    int64_t t = nowUs();
    if (b.valid() && b.millivolts() >= 4180) {                 // §12.2: full at two consecutive charge-check wakes
      if (r.fullSinceUs && (t - r.fullSinceUs) >= (int64_t)r.early.chargeCheckMin * 60LL * 1000000LL - 30000000LL) full = true;
      if (!r.fullSinceUs) r.fullSinceUs = t;
    } else r.fullSinceUs = 0;
    r.battMv = b.millivolts(); r.battPct = b.percent();
    sb::Config cfg;
    sb::config::defaults(cfg);
    cfg.display.theme = r.early.theme; cfg.display.flip = r.early.flip; cfg.display.brightnessPct = r.early.dimPct;
    if (d.begin(cfg)) {
      ChargeScreen cs; cs.valid = b.valid(); cs.pct = b.percent(); cs.full = full;
      d.setScreen(&cs); d.drawNow();
      delay(5000);
      d.setBrightness(0); { uint32_t t0 = millis(); while (millis() - t0 < 160) { d.tick(millis()); delay(5); } }
      d.powerDown();
    }
    Board::gatedRail(false);
  } else r.fullSinceUs = usb ? r.fullSinceUs : 0;
  r.usbAtSleep = usb;
  rtc::commit();
  rearmOff(usb);
}

}  // namespace Power
