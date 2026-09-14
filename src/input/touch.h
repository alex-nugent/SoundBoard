// Touch pads (FirmwareSpec.md §4.1): the IDF touch driver on GPIO2-5 with the
// shield on T14, the percentage detector of validations 01-12, stored
// baselines with the boot check, fresh calibration, drift, automatic
// re-baseline, per-pad thresholds and the stuck rule. Events go on the bus;
// positions (P1-P4), not channels, are what the rest of the firmware sees.
#pragma once
#include <stdint.h>
#include <Print.h>
#include "config/config.h"

class TouchInput {
 public:
  struct Pad {                    // by position P1..P4
    int8_t   ch = -1;             // channel index 0..3 (GPIO 2..5), -1 = no valid channel
    sb::Role role = sb::Role::None;
    uint32_t raw = 0;
    float    delta = 0;           // % of baseline
    bool     pressed = false;     // a reported press
    bool     held = false;        // pressed at boot (or when calibrated): silent until it releases (§4.1)
    bool     stuck = false;
    uint16_t pressId = 0;
    uint32_t tDown = 0;
    float    peak = 0;
  };

  // Driver up, shield mode, baselines restored (RTC copy when given and
  // complete, else NVS) and the boot check started; with no usable copy a
  // fresh calibration starts. False = driver failed (!TOUCH).
  bool begin(const sb::Config& cfg, const uint32_t* rtcBaselines);
  void onConfig(const sb::Config& cfg);        // thresholds, roles, stuck time, pad order; shield changes need a reboot
  void tick(uint32_t now);                     // every app tick; polls every 15 ms
  void calibrate();                            // fresh calibration: settle, average, save
  bool calibrating() const { return phase_ == Phase::Settling || phase_ == Phase::Averaging; }
  bool ready() const { return phase_ == Phase::Ready; }
  bool failed() const { return phase_ == Phase::Down; }
  bool anyPressed() const;
  const Pad& pad(uint8_t pos) const { return pad_[pos < 4 ? pos : 0]; }
  uint32_t baseline(uint8_t ch) const { return base_[ch < 4 ? ch : 0]; }
  uint16_t nextPressId() { return ++pressId_; }

  // Sleep and wake (§4.1, §12.3).
  void latchWake(uint8_t gpio);                // boot: the channel that woke the chip is a pending press (before begin())
  void latchWakeUnknown() { wakeUnknown_ = true; }   // a touch woke the chip but the mask was empty: the boot check picks a pad still pressed
  bool wakePending() const { return wakeGpio_ != 0; }
  bool wakeReadable() const { return phase_ == Phase::Ready || (phase_ == Phase::Checking && wakeReady_); }   // the latched pad has a usable reading
  void consumeWake(uint32_t now);              // once the level is restored, the configuration loaded and the rail ready
  bool storeBaselinesIfMoved();                // sleep/off entry: NVS refresh when a baseline moved > 1 % from the stored copy
  bool armSleepWake();                         // hardware thresholds from the baselines, any-pad deep-sleep wake; false = driver down
  void disableWake();                          // OFF: no touch wake
  void printStatus(Print& out) const;
  void printLive(Print& out) const;

 private:
  enum class Phase : uint8_t { Down, Checking, Settling, Averaging, Ready };
  struct NvsBlob { uint32_t magic; uint8_t chans[4]; uint8_t shield, drive; uint32_t baselines[4]; };

  void  applyConfig(const sb::Config& cfg);
  bool  driverUp();
  bool  loadNvs(uint32_t out[4]) const;
  void  storeBaselines();                      // NVS (deferred write) + RtcState
  static void storeThunk(void* arg);
  bool  readAll(uint32_t v[4]) const;          // false until every channel has scanned
  void  evaluateCheck(uint32_t now);
  void  finishCalibration(uint32_t now);
  void  detect(uint32_t now);
  float pressThr(uint8_t pos) const { return padPress_[pos] > 0 ? padPress_[pos] : press_; }
  float releaseThr(uint8_t pos) const { float h = 0.5f * pressThr(pos); return release_ < h ? release_ : h; }
  bool  eligible(uint8_t pos) const { return pad_[pos].ch >= 0 && pad_[pos].role != sb::Role::None && !pad_[pos].stuck; }
  void  postDown(uint8_t pos, uint32_t now);
  void  postUp(uint8_t pos, uint32_t now);
  void  clearPresses(uint32_t now, bool report);

  Phase    phase_ = Phase::Down;
  Pad      pad_[4];
  uint32_t base_[4] = { 0, 0, 0, 0 };          // by channel index
  uint8_t  confirm_[4] = { 0, 0, 0, 0 };       // by position
  uint32_t offSince_[4] = { 0, 0, 0, 0 };      // by position: parked off the baseline since (0 = not parked)
  uint64_t sum_[4] = { 0, 0, 0, 0 };
  uint8_t  samples_ = 0, skipped_ = 0;
  uint32_t nextPoll_ = 0, settleUntil_ = 0;
  uint16_t pressId_ = 0;
  // configuration
  uint8_t  chans_[4] = { 2, 3, 4, 5 };         // GPIO per position
  float    press_ = 3.0f, release_ = 1.5f, sep_ = 1.0f;
  float    padPress_[4] = { 0, 0, 0, 0 };
  uint8_t  shield_ = 0, drive_ = 3;
  uint32_t stuckMs_ = 30000;
  bool     driverUp_ = false;
  NvsBlob  pending_ = {};                     // the last stored copy (or the restored one)
  uint8_t  wakeGpio_ = 0;                     // latched wake press, 0 = none
  bool     wakeUnknown_ = false, wakeReady_ = false;
};
