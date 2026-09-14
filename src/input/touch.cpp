#include "input/touch.h"
#include "input/touch_tuning.h"
#include "hal/pins.h"
#include "hal/storage.h"
#include "power/rtc_state.h"
#include "util/event.h"
#include "util/timer.h"
#include "diag/log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <driver/touch_sens.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <string.h>

using namespace touch_tuning;
static const char* TAG = "touch";

// Driver handles (one instance per firmware; the class keeps the logic).
static touch_sensor_handle_t  s_sens = nullptr;
static touch_channel_handle_t s_chan[4] = { nullptr, nullptr, nullptr, nullptr };
static touch_channel_handle_t s_shield = nullptr;
static touch_sensor_sample_config_t s_sample[1];

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
void TouchInput::applyConfig(const sb::Config& cfg) {
  press_ = cfg.touch.pressPct; release_ = cfg.touch.releasePct; sep_ = cfg.touch.separationPct;
  for (uint8_t i = 0; i < 4; i++) padPress_[i] = cfg.touch.hasPadPressPct ? cfg.touch.padPressPct[i] : 0;
  stuckMs_ = cfg.touch.stuckAfterMs;
  for (uint8_t pos = 0; pos < 4; pos++) {
    uint8_t gpio = cfg.hardware.padChannels[pos];
    chans_[pos] = gpio;
    pad_[pos].ch = (gpio >= 2 && gpio <= 5) ? (int8_t)(gpio - 2) : -1;
    pad_[pos].role = cfg.pads.roles[pos];
    if (pad_[pos].ch < 0) LOG_W(TAG, "hardware.padChannels[%u] = %u is not a touch channel: P%u dead", pos, gpio, pos + 1);
  }
}

void TouchInput::onConfig(const sb::Config& cfg) {
  uint8_t oldCh[4]; for (uint8_t i = 0; i < 4; i++) oldCh[i] = chans_[i];
  applyConfig(cfg);
  if (cfg.touch.shield != shield_ || cfg.touch.shieldDrive != drive_)
    LOG_I(TAG, "shield settings changed: applied at the next boot");
  if (memcmp(oldCh, chans_, 4) != 0) LOG_I(TAG, "pad order changed: P1-P4 = GPIO %u %u %u %u (baselines are per channel, kept)", chans_[0], chans_[1], chans_[2], chans_[3]);
}

// ---------------------------------------------------------------------------
// Driver (validation 12 recipe)
// ---------------------------------------------------------------------------
bool TouchInput::driverUp() {
  s_sample[0] = (touch_sensor_sample_config_t)TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2);
  touch_sensor_config_t sensCfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, s_sample);
  esp_err_t e = touch_sensor_new_controller(&sensCfg, &s_sens);
  if (e != ESP_OK) { LOG_E(TAG, "new_controller: %d", (int)e); return false; }
  touch_sensor_filter_config_t filterCfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
  if ((e = touch_sensor_config_filter(s_sens, &filterCfg)) != ESP_OK) { LOG_E(TAG, "config_filter: %d", (int)e); return false; }
  touch_channel_config_t chanCfg = TOUCH_CHANNEL_DEFAULT_CONFIG();
  for (uint8_t k = 0; k < 4; k++) {
    if ((e = touch_sensor_new_channel(s_sens, pins::TOUCH_CH[k], &chanCfg, &s_chan[k])) != ESP_OK) { LOG_E(TAG, "new_channel %u: %d", pins::TOUCH_CH[k], (int)e); return false; }
  }
  if (shield_ == sb::SHIELD_DRIVEN) {
    touch_channel_config_t c = TOUCH_CHANNEL_DEFAULT_CONFIG();
    if ((e = touch_sensor_new_channel(s_sens, TOUCH_SHIELD_CHAN_ID, &c, &s_shield)) != ESP_OK) { LOG_E(TAG, "shield channel: %d", (int)e); return false; }
    touch_waterproof_config_t wp = {};
    wp.guard_chan = NULL; wp.shield_chan = s_shield; wp.shield_drv = drive_; wp.flags.immersion_proof = 0;
    if ((e = touch_sensor_config_waterproof(s_sens, &wp)) != ESP_OK) { LOG_E(TAG, "config_waterproof: %d", (int)e); return false; }
  } else {
    rtc_gpio_deinit((gpio_num_t)pins::SHIELD);
    pinMode(pins::SHIELD, INPUT);                                   // floating (§4.1); never grounded
  }
  if ((e = touch_sensor_enable(s_sens)) != ESP_OK) { LOG_E(TAG, "enable: %d", (int)e); return false; }
  if ((e = touch_sensor_start_continuous_scanning(s_sens)) != ESP_OK) { LOG_E(TAG, "start scanning: %d", (int)e); return false; }
  return true;
}

// The first readings after the driver starts are garbage (a saturated
// 0x3FFFFF or values tens of times the baseline, see tests/02): a sample is
// usable only when every channel is non-zero and, when a baseline exists,
// within half of it. Callers skip unusable samples.
bool TouchInput::readAll(uint32_t v[4]) const {
  bool ok = true;
  for (uint8_t k = 0; k < 4; k++) {
    uint32_t d[1] = { 0 };
    touch_channel_read_data(s_chan[k], TOUCH_CHAN_DATA_TYPE_SMOOTH, d);
    v[k] = d[0];
    if (!d[0] || d[0] >= 0x3FFFFF) ok = false;
    else if (base_[k] && (d[0] > base_[k] + base_[k] / 2 || d[0] < base_[k] / 2)) ok = false;
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Stored baselines: NVS sb-cal/touch (by channel, tagged with the pad order and
// shield settings they were taken under) and the RtcState copy.
// ---------------------------------------------------------------------------
bool TouchInput::loadNvs(uint32_t out[4]) const {
  Preferences p;
  if (!p.begin("sb-cal", true)) return false;
  NvsBlob b = {};
  size_t n = p.isKey("touch") ? p.getBytes("touch", &b, sizeof b) : 0;
  p.end();
  if (n != sizeof b || b.magic != NVS_MAGIC) return false;
  if (memcmp(b.chans, chans_, 4) != 0 || b.shield != shield_ || b.drive != drive_) { LOG_I(TAG, "stored baselines were taken under other pad/shield settings: ignored"); return false; }
  for (uint8_t k = 0; k < 4; k++) { if (!b.baselines[k]) return false; out[k] = b.baselines[k]; }
  return true;
}

void TouchInput::storeThunk(void* arg) {
  TouchInput* self = static_cast<TouchInput*>(arg);
  Preferences p;
  if (p.begin("sb-cal", false)) { p.putBytes("touch", &self->pending_, sizeof self->pending_); p.end(); }
}

void TouchInput::storeBaselines() {
  pending_.magic = NVS_MAGIC;
  memcpy(pending_.chans, chans_, 4);
  pending_.shield = shield_; pending_.drive = drive_;
  for (uint8_t k = 0; k < 4; k++) pending_.baselines[k] = base_[k];
  Storage::deferredWrite(&TouchInput::storeThunk, this);           // §2.3 rule 17: never during audio
  RtcState& r = rtc::get();
  for (uint8_t k = 0; k < 4; k++) r.baselines[k] = base_[k];
  rtc::commit();
}

// ---------------------------------------------------------------------------
// Begin, calibration and the boot check
// ---------------------------------------------------------------------------
bool TouchInput::begin(const sb::Config& cfg, const uint32_t* rtcBaselines) {
  shield_ = cfg.touch.shield; drive_ = cfg.touch.shieldDrive;
  applyConfig(cfg);
  if (!driverUp_) driverUp_ = driverUp();
  if (!driverUp_) { phase_ = Phase::Down; return false; }
  uint32_t restored[4] = { 0, 0, 0, 0 };
  bool have = false;
  const char* from = "";
  if (rtcBaselines && rtcBaselines[0] && rtcBaselines[1] && rtcBaselines[2] && rtcBaselines[3]) {
    memcpy(restored, rtcBaselines, sizeof restored); have = true; from = "RTC";
  } else if (loadNvs(restored)) { have = true; from = "NVS"; }
  uint32_t now = millis();
  nextPoll_ = now;
  if (have) {
    memcpy(base_, restored, sizeof base_);
    pending_.magic = NVS_MAGIC; memcpy(pending_.chans, chans_, 4); pending_.shield = shield_; pending_.drive = drive_;
    memcpy(pending_.baselines, restored, sizeof restored);
    memset(sum_, 0, sizeof sum_); samples_ = 0; skipped_ = 0;
    phase_ = Phase::Checking;
    LOG_I(TAG, "driver up, shield %s%s; baselines from %s: %lu %lu %lu %lu (checking)", shield_ == sb::SHIELD_DRIVEN ? "driven " : "floating",
          shield_ == sb::SHIELD_DRIVEN ? String(drive_).c_str() : "", from,
          (unsigned long)base_[0], (unsigned long)base_[1], (unsigned long)base_[2], (unsigned long)base_[3]);
  } else {
    LOG_I(TAG, "driver up, shield %s; no stored baselines: fresh calibration", shield_ == sb::SHIELD_DRIVEN ? "driven" : "floating");
    calibrate();
  }
  return true;
}

void TouchInput::calibrate() {
  if (!driverUp_) return;
  uint32_t now = millis();
  clearPresses(now, true);
  memset(sum_, 0, sizeof sum_); samples_ = 0;
  settleUntil_ = now + SETTLE_MS;
  phase_ = Phase::Settling;
  LOG_I(TAG, "calibrating: hands off for %u ms", (unsigned)SETTLE_MS);
}

void TouchInput::clearPresses(uint32_t now, bool report) {
  for (uint8_t pos = 0; pos < 4; pos++) {
    if (pad_[pos].pressed && !pad_[pos].held && report) postUp(pos, now);
    pad_[pos].pressed = pad_[pos].held = pad_[pos].stuck = false;
    confirm_[pos] = 0; offSince_[pos] = 0;
  }
}

void TouchInput::finishCalibration(uint32_t now) {
  for (uint8_t k = 0; k < 4; k++) base_[k] = samples_ ? (uint32_t)(sum_[k] / samples_) : 0;
  clearPresses(now, false);
  phase_ = Phase::Ready;
  storeBaselines();
  LOG_I(TAG, "calibrated: baselines %lu %lu %lu %lu (ch2-5), saved", (unsigned long)base_[0], (unsigned long)base_[1], (unsigned long)base_[2], (unsigned long)base_[3]);
}

// §4.1 "Baseline check" on the first scans after a restore.
void TouchInput::evaluateCheck(uint32_t now) {
  float d[4]; bool use[4]; uint8_t n = 0;
  for (uint8_t pos = 0; pos < 4; pos++) {
    int8_t k = pad_[pos].ch;
    // The latched wake pad is left out (§4.1), and so is a wake press already consumed and still held: it was
    // reported as a normal press and releases normally (CP-5: the check used to re-mark it "held from boot",
    // which swallowed its PadUp).
    use[pos] = k >= 0 && base_[k] > 0 && pad_[pos].role != sb::Role::None && !(wakeGpio_ && chans_[pos] == wakeGpio_) && !pad_[pos].pressed;
    d[pos] = use[pos] ? 100.0f * ((float)(sum_[k] / samples_) - (float)base_[k]) / (float)base_[k] : 0;
    if (use[pos]) n++;
  }
  if (wakeUnknown_) {                                          // §4.1: a touch wake whose pad the hardware no longer reports
    wakeUnknown_ = false;
    int best = -1;
    for (uint8_t pos = 0; pos < 4; pos++) if (use[pos] && d[pos] >= pressThr(pos) && (best < 0 || d[pos] > d[best])) best = pos;
    if (best >= 0) { wakeGpio_ = chans_[best]; use[best] = false; n--; LOG_I(TAG, "wake pad identified from the first scans: P%u (%+.1f %%)", best + 1, d[best]); }
    else LOG_W(TAG, "wake touch: no pad still pressed when the driver came up; the tap is lost");
  }
  if (!n) {                                                    // nothing left to judge: a held wake press alone is not a reason to recalibrate
    bool anyPressed = false;
    for (uint8_t pos = 0; pos < 4; pos++) if (pad_[pos].pressed) anyPressed = true;
    if (anyPressed) { phase_ = Phase::Ready; return; }
    calibrate(); return;
  }
  bool allWithin = true, allPos = true, allNeg = true;
  float sorted[4]; uint8_t m = 0;
  for (uint8_t pos = 0; pos < 4; pos++) {
    if (!use[pos]) continue;
    if (d[pos] > release_ || d[pos] < -release_) allWithin = false;
    if (d[pos] <= 0) allPos = false;
    if (d[pos] >= 0) allNeg = false;
    sorted[m++] = d[pos];
  }
  for (uint8_t i = 1; i < m; i++) for (uint8_t j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) { float t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t; }
  float median = (m & 1) ? sorted[m / 2] : 0.5f * (sorted[m / 2 - 1] + sorted[m / 2]);
  if (allWithin) {
    LOG_I(TAG, "baselines accepted (deltas %+.1f %+.1f %+.1f %+.1f %%)", d[0], d[1], d[2], d[3]);
  } else if ((allPos || allNeg) && (median <= CHECK_SHIFT_MAX && median >= -CHECK_SHIFT_MAX)) {
    for (uint8_t k = 0; k < 4; k++) base_[k] = (uint32_t)((float)base_[k] * (1.0f + median / 100.0f));
    LOG_I(TAG, "baselines shifted by the common %+.1f %% (deltas %+.1f %+.1f %+.1f %+.1f)", median, d[0], d[1], d[2], d[3]);
    storeBaselines();
  } else if (allPos || allNeg) {
    LOG_W(TAG, "baselines off by a common %+.1f %%: fresh calibration", median);
    calibrate();
    return;
  } else {
    for (uint8_t pos = 0; pos < 4; pos++) {
      if (!use[pos]) continue;
      if (d[pos] >= pressThr(pos)) {
        pad_[pos].pressed = true; pad_[pos].held = true; pad_[pos].tDown = now; pad_[pos].pressId = 0;
        LOG_I(TAG, "P%u reads %+.1f %% at boot: held until it releases (no press reported)", pos + 1, d[pos]);
      } else if (d[pos] < -release_ || d[pos] > release_) {
        LOG_I(TAG, "P%u reads %+.1f %% at boot: accepted, re-baselines if it stays", pos + 1, d[pos]);
      }
    }
  }
  phase_ = Phase::Ready;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void TouchInput::postDown(uint8_t pos, uint32_t now) {
  Pad& p = pad_[pos];
  p.pressed = true; p.held = false; p.stuck = false; p.tDown = now; p.pressId = ++pressId_; p.peak = p.delta;
  confirm_[pos] = 0; offSince_[pos] = 0;
  Event e = { Ev::PadDown, now, {} };
  e.pad.pos = pos; e.pad.ch = chans_[pos]; e.pad.delta = (int16_t)(p.delta * 10.0f); e.pad.pressId = p.pressId; e.pad.heldMs = 0;
  EventBus::post(e);
}

void TouchInput::postUp(uint8_t pos, uint32_t now) {
  Pad& p = pad_[pos];
  Event e = { Ev::PadUp, now, {} };
  uint32_t held = now - p.tDown;
  e.pad.pos = pos; e.pad.ch = chans_[pos]; e.pad.delta = (int16_t)(p.peak * 10.0f); e.pad.pressId = p.pressId; e.pad.heldMs = held > 65535 ? 65535 : (uint16_t)held;
  EventBus::post(e);
}

bool TouchInput::anyPressed() const { for (uint8_t i = 0; i < 4; i++) if (pad_[i].pressed) return true; return false; }

// ---------------------------------------------------------------------------
// Detection (§4.1 algorithm), one poll
// ---------------------------------------------------------------------------
void TouchInput::detect(uint32_t now) {
  uint32_t v[4];
  readAll(v);
  bool valid[4];
  for (uint8_t pos = 0; pos < 4; pos++) {
    Pad& p = pad_[pos];
    int8_t k = p.ch;
    valid[pos] = k >= 0 && v[k] != 0 && v[k] < 0x3FFFFF && base_[k] != 0 && v[k] < 4 * base_[k];
    if (valid[pos]) { p.raw = v[k]; p.delta = 100.0f * ((int32_t)v[k] - (int32_t)base_[k]) / (float)base_[k]; }
  }
  // Ranking among the eligible pads (stuck pads and role "none" are left out).
  int8_t top = -1, second = -1;
  for (uint8_t pos = 0; pos < 4; pos++) {
    if (!valid[pos] || !eligible(pos)) continue;
    if (wakeGpio_ && chans_[pos] == wakeGpio_) continue;       // the latched wake press is reported by consumeWake()
    if (top < 0 || pad_[pos].delta > pad_[top].delta) { second = top; top = pos; }
    else if (second < 0 || pad_[pos].delta > pad_[second].delta) second = pos;
  }
  float topSep = top >= 0 ? pad_[top].delta - (second >= 0 ? pad_[second].delta : 0.0f) : 0.0f;
  bool any = false;                                            // a pressed pad blocks new presses; a stuck one does not (§4.1)
  for (uint8_t pos = 0; pos < 4; pos++) if (pad_[pos].pressed && !pad_[pos].stuck) any = true;

  for (uint8_t pos = 0; pos < 4; pos++) {
    if (!valid[pos]) continue;
    if (wakeGpio_ && chans_[pos] == wakeGpio_) continue;
    Pad& p = pad_[pos];
    float d = p.delta;
    if (!p.pressed) {
      if (!eligible(pos)) { confirm_[pos] = 0; continue; }
      bool cand = d >= pressThr(pos) && pos == top && topSep >= sep_ && !any;
      if (cand) {
        if (++confirm_[pos] >= CONFIRM_SAMPLES) { postDown(pos, now); any = true; }
        continue;
      }
      confirm_[pos] = 0;
      int8_t k = p.ch;
      // Drift while idle.
      if (d < DRIFT_BELOW_PCT && d > -DRIFT_BELOW_PCT) {
        int32_t err = (int32_t)p.raw - (int32_t)base_[k];
        base_[k] = (uint32_t)((int32_t)base_[k] + err / (1 << DRIFT_SHIFT));
        offSince_[pos] = 0;
      } else if (d < -releaseThr(pos) || (d >= releaseThr(pos) && d < pressThr(pos))) {
        // Parked off the baseline: snap after REBASELINE_MS.
        if (!offSince_[pos]) offSince_[pos] = now ? now : 1;
        else if (due(now, offSince_[pos] + REBASELINE_MS)) {
          LOG_I(TAG, "P%u re-baselined: %lu -> %lu (%+.1f %% for %u s)", pos + 1, (unsigned long)base_[k], (unsigned long)p.raw, d, (unsigned)(REBASELINE_MS / 1000));
          base_[k] = p.raw; offSince_[pos] = 0;
        }
      } else offSince_[pos] = 0;
    } else {
      if (d > p.peak) p.peak = d;
      if (d < releaseThr(pos)) {
        if (++confirm_[pos] >= CONFIRM_SAMPLES) {
          bool report = !p.held;
          bool wasStuck = p.stuck;
          p.pressed = false; p.held = false; p.stuck = false; confirm_[pos] = 0;
          if (report) postUp(pos, now);
          else LOG_I(TAG, "P%u released (was held from boot%s)", pos + 1, wasStuck ? ", stuck" : "");
        }
      } else {
        confirm_[pos] = 0;
        if (!p.stuck && stuckMs_ && due(now, p.tDown + stuckMs_)) {
          if (!p.held) {
            p.stuck = true;
            Event e = { Ev::PadStuck, now, {} };
            e.pad.pos = pos; e.pad.ch = chans_[pos]; e.pad.delta = (int16_t)(d * 10.0f); e.pad.pressId = p.pressId; e.pad.heldMs = 0;
            EventBus::post(e);
          } else {                                             // §4.1: held since boot and never released is not a finger
            int8_t k = p.ch;
            LOG_W(TAG, "P%u held since boot for %lu s: not a finger, baseline %lu -> %lu", pos + 1, (unsigned long)(stuckMs_ / 1000), (unsigned long)base_[k], (unsigned long)p.raw);
            base_[k] = p.raw;
            p.pressed = false; p.held = false; confirm_[pos] = 0; offSince_[pos] = 0;
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void TouchInput::tick(uint32_t now) {
  if (phase_ == Phase::Down || !due(now, nextPoll_)) return;
  nextPoll_ += POLL_MS;
  if ((int32_t)(now - nextPoll_) > 5 * POLL_MS) nextPoll_ = now + POLL_MS;   // after a stall, do not burst
  switch (phase_) {
    case Phase::Checking: {
      uint32_t v[4];
      if (!readAll(v)) {                                 // not scanned yet, or warm-up garbage, or far off the stored copy
        if (++skipped_ >= CHECK_SKIP_MAX) { LOG_W(TAG, "readings stayed far from the stored baselines: fresh calibration"); calibrate(); }
        return;
      }
      for (uint8_t k = 0; k < 4; k++) sum_[k] += v[k];
      wakeReady_ = true;                                 // one good scan: the latched wake press can be consumed before the check ends
      if (++samples_ >= CHECK_SCANS) evaluateCheck(now);
      return;
    }
    case Phase::Settling:
      if (due(now, settleUntil_)) { phase_ = Phase::Averaging; memset(sum_, 0, sizeof sum_); samples_ = 0; }
      return;
    case Phase::Averaging: {
      uint32_t v[4];
      if (!readAll(v)) return;
      for (uint8_t k = 0; k < 4; k++) sum_[k] += v[k];
      if (++samples_ >= BASELINE_SAMPLES) finishCalibration(now);
      return;
    }
    case Phase::Ready:
      detect(now);
      return;
    default:
      return;
  }
}

// ---------------------------------------------------------------------------
// Sleep and wake (§4.1 wake press, §12.3 steps 1 and 4)
// ---------------------------------------------------------------------------
void TouchInput::latchWake(uint8_t gpio) { wakeGpio_ = (gpio >= 2 && gpio <= 5) ? gpio : 0; if (wakeGpio_) LOG_I(TAG, "wake by touch GPIO%u: latched as a pending press", gpio); }

void TouchInput::consumeWake(uint32_t now) {
  if (!wakeGpio_) return;
  int pos = -1;
  for (uint8_t i = 0; i < 4; i++) if (chans_[i] == wakeGpio_) pos = i;
  uint8_t gpio = wakeGpio_;
  wakeGpio_ = 0;
  if (pos < 0 || !(phase_ == Phase::Ready || phase_ == Phase::Checking) || pad_[pos].ch < 0 || pad_[pos].role == sb::Role::None) { LOG_W(TAG, "wake press on GPIO%u dropped: pad not usable", gpio); return; }
  Pad& p = pad_[pos];
  uint32_t v[4];
  readAll(v);
  int8_t k = p.ch;
  if (v[k] && v[k] < 0x3FFFFF && base_[k]) { p.raw = v[k]; p.delta = 100.0f * ((int32_t)v[k] - (int32_t)base_[k]) / (float)base_[k]; }
  postDown(pos, now);                                          // the latch is consumed exactly once
  if (p.delta >= pressThr(pos)) {
    LOG_I(TAG, "wake press P%u: still pressed (%+.1f %%), runs until release", pos + 1, p.delta);
  } else {
    p.pressed = false; p.held = false;
    postUp(pos, now);                                          // heldMs 0: nothing is left held
    LOG_I(TAG, "wake press P%u: released already (%+.1f %%), reported as a tap", pos + 1, p.delta);
  }
}

bool TouchInput::storeBaselinesIfMoved() {
  if (!driverUp_ || phase_ != Phase::Ready) return false;
  bool moved = pending_.magic != NVS_MAGIC;
  for (uint8_t k = 0; k < 4 && !moved; k++) {
    uint32_t s = pending_.baselines[k];
    if (!s) { moved = true; break; }
    uint32_t d = base_[k] > s ? base_[k] - s : s - base_[k];
    if (d * 100 > s) moved = true;                             // > 1 %
  }
  if (moved) { storeBaselines(); LOG_I(TAG, "baselines refreshed in NVS before sleep"); }
  return moved;
}

bool TouchInput::armSleepWake() {
  if (!driverUp_) return false;
  touch_sensor_stop_continuous_scanning(s_sens);
  touch_sensor_disable(s_sens);
  for (uint8_t k = 0; k < 4; k++) {
    int pos = -1;
    for (uint8_t i = 0; i < 4; i++) if (pad_[i].ch == (int8_t)k) pos = i;
    uint32_t thr = 0x3FFFFF;                                   // no pad on this channel, or role none: never wakes
    if (pos >= 0 && pad_[pos].role != sb::Role::None && base_[k]) {
      float pct = pressThr(pos) / 100.0f;
      uint32_t bench[1] = { 0 };
      touch_channel_read_data(s_chan[k], TOUCH_CHAN_DATA_TYPE_BENCHMARK, bench);
      uint32_t raw = pad_[pos].raw ? pad_[pos].raw : base_[k];
      bool resting = pad_[pos].delta >= releaseThr(pos);       // above its release threshold now: a resting hand must not wake the board
      if (resting) thr = (raw > bench[0] ? raw - bench[0] : 0) + (uint32_t)(raw * pct);
      else thr = (uint32_t)(base_[k] * pct);
      LOG_I(TAG, "sleep wake P%u (ch%u): threshold +%lu over benchmark %lu (raw %lu%s)", pos + 1, k + 2, (unsigned long)thr, (unsigned long)bench[0], (unsigned long)raw, resting ? ", resting: raised" : "");
    }
    touch_channel_config_t c = TOUCH_CHANNEL_DEFAULT_CONFIG();
    c.active_thresh[0] = thr;
    esp_err_t e = touch_sensor_reconfig_channel(s_chan[k], &c);
    if (e != ESP_OK) LOG_W(TAG, "reconfig channel %u: %d", k + 2, (int)e);
  }
  touch_sleep_config_t cfg = {};
  cfg.slp_wakeup_lvl = TOUCH_DEEP_SLEEP_WAKEUP;
  cfg.deep_slp_allow_pd = false;                               // RTC peripheral domain stays on: any pad wakes (§2.3 rule 10)
  cfg.deep_slp_chan = NULL;
  esp_err_t e = touch_sensor_config_sleep_wakeup(s_sens, &cfg);
  if (e != ESP_OK) { LOG_E(TAG, "config_sleep_wakeup: %d", (int)e); return false; }
  touch_sensor_enable(s_sens);
  touch_sensor_start_continuous_scanning(s_sens);
  esp_sleep_enable_touchpad_wakeup();
  return true;
}

void TouchInput::disableWake() {
  if (!driverUp_) return;
  touch_sensor_stop_continuous_scanning(s_sens);
  touch_sensor_disable(s_sens);
  touch_sensor_config_sleep_wakeup(s_sens, NULL);              // §12.3 OFF: pads dead
  phase_ = Phase::Down;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
static const char* phaseName(uint8_t p) {
  switch (p) { case 0: return "driver failed"; case 1: return "checking baselines"; case 2: return "calibrating (settle)"; case 3: return "calibrating (averaging)"; default: return "ready"; }
}

void TouchInput::printStatus(Print& out) const {
  out.printf("touch: %s | shield %s", phaseName((uint8_t)phase_), shield_ == sb::SHIELD_DRIVEN ? "driven" : "floating");
  if (shield_ == sb::SHIELD_DRIVEN) out.printf(" %u", drive_);
  out.printf(" | press %.2g%% release %.2g%% sep %.2g%% | stuck %lu s\n", press_, release_, sep_, (unsigned long)(stuckMs_ / 1000));
  for (uint8_t pos = 0; pos < 4; pos++) {
    const Pad& p = pad_[pos];
    out.printf("  P%u ch%u %-5s base %-6lu raw %-6lu %+6.2f%% thr %.2g/%.2g%s%s%s\n", pos + 1, chans_[pos],
               p.role == sb::Role::Level ? "level" : p.role == sb::Role::Sound ? "sound" : "none",
               p.ch >= 0 ? (unsigned long)base_[p.ch] : 0UL, (unsigned long)p.raw, p.delta, pressThr(pos), releaseThr(pos),
               p.pressed ? (p.held ? " HELD" : " PRESSED") : "", p.stuck ? " STUCK" : "", p.ch < 0 ? " (no channel)" : "");
  }
}

void TouchInput::printLive(Print& out) const {
  out.print("pads");
  for (uint8_t pos = 0; pos < 4; pos++) {
    const Pad& p = pad_[pos];
    out.printf(" | P%u %c %+6.2f%%", pos + 1, p.pressed ? (p.stuck ? 'S' : '#') : '.', p.delta);
  }
  out.printf(" | %s\n", phaseName((uint8_t)phase_));
}
