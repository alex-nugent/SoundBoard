#include "app/state.h"
#include "app/faults.h"
#include "hal/board.h"
#include "hal/storage.h"
#include "diag/log.h"
#include "diag/console.h"
#include "util/event.h"
#include "util/timer.h"
#include "util/strutil.h"
#include "config/loader.h"
#include "power/rtc_state.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <errno.h>
#include <freertos/task.h>
#include <SD.h>
#include <SPI.h>
#include "hal/pins.h"
#include <driver/gpio.h>
#include <esp_rom_crc.h>
#include <Preferences.h>

static AudioEngine* s_engine = nullptr;

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char* TAG = "app";

const char* AppState::modeName() const {
  switch (mode_) { case AppMode::Boot: return "BOOT"; case AppMode::Active: return "ACTIVE"; case AppMode::Dimmed: return "DIMMED"; case AppMode::Menu: return "MENU"; case AppMode::Updating: return "UPDATING"; default: return "FAULT"; }
}

sb::ButtonDurations AppState::durations() const {
  sb::ButtonDurations d;
  d.attendantHoldMs = cfg_.levelChange.attendantHoldMs;
  d.bothTapMs = cfg_.levelChange.bothTapMs;
  d.offHoldMs = cfg_.power.offHoldMs;
  d.menuHoldMs = cfg_.menu.enabled ? cfg_.menu.holdMs : 0;   // §14.1: menu.enabled false removes the command (the both-hold stops at OFF)
  return d;
}

// ---------------------------------------------------------------------------
// Boot (§17.2, cold path; the BLE and power steps come by phase)
// ---------------------------------------------------------------------------
bool AppState::audioIdleThunk() { return !s_engine || s_engine->idleFor(200); }   // §19.7 rule 11

void AppState::audioEarlyStart() {
  if (audioStarted_) return;
  audioStarted_ = true;
  Storage::begin();
  EventBus::begin();
  s_engine = &audio_;
  Storage::setIdleCheck(&AppState::audioIdleThunk);
  cache_.begin();
  if (!audio_.begin(&cache_, &kcx_)) { LOG_E(TAG, "audio task not created"); return; }
  audio_.railUp();                                             // §6.7: the rail rises while the screen and card come up
}

void AppState::begin(const BootInfo& bi) {
  bi_ = bi;
  mode_ = AppMode::Boot;
  audioEarlyStart();
  Console::begin(this);

  // Flash file system first (a first-boot format takes seconds; the task WDT is subscribed after begin()).
  store_.begin();

  // A. Gated rail, screen, black, backlight fade-in; provisional defaults until the card is read.
  sb::config::defaults(cfg_);
  const RtcState& r = rtc::get();
  cfg_.display.theme = sb::THEME_AMBER;
  if (!display_.begin(cfg_, bi.kind != BootKind::SleepWake)) { enterFault("screen init failed"); return; }

  // B. Owner label from RtcState.early (defaults when there is no valid RTC copy), or the recovery prompt.
  bootScreen_.nLines = 0;
  for (uint8_t i = 0; i < r.early.ownerLines && i < 3; i++) bootScreen_.lines[bootScreen_.nLines++] = r.early.ownerLabel[i];
  bootScreen_.version = FW_VERSION;
  bootScreen_.recoveryPrompt = bi.bothButtonsAtReset;
  uint32_t now = millis();
  uint16_t labelMs = r.early.ownerLabelMs;
  bool showLabel = bi.kind != BootKind::SleepWake && (bootScreen_.nLines > 0 || bi.bothButtonsAtReset) && (labelMs > 0 || bi.bothButtonsAtReset);
  if (showLabel) {
    display_.setScreen(&bootScreen_);
    display_.drawNow();
    bootUntil_ = now + labelMs;
    if (bi.bothButtonsAtReset) { recoveryCheckAt_ = now + 3000; bootUntil_ = now + 3000; }
  } else {
    bootUntil_ = now;
  }
  LOG_I(TAG, "screen up at +%lu ms", (unsigned long)millis());

  // C. Card, configuration, mirror.
  store_.load(cfg_, rep_);
  if (bi.safeMode) {                                           // §18 safe mode: the card's configuration is not applied (it may be what crashes)
    auto hw = cfg_.hardware;
    sb::config::defaults(cfg_);
    cfg_.hardware = hw;                                        // the pad map is physical, keep it
    cfg_.keyboard.enabled = false;
    cfg_.bluetoothSpeaker.enabled = false;
    cfg_.audio.cacheMaxMB = 1;
    setFault(F_SAFE, true);
  }
  configLoaded_ = true;
  setFault(F_CARD, !store_.cardMounted());
  setFault(F_CONFIG, store_.cardConfigBad());
  Log::setLevel((LogLevel)cfg_.diag.logLevel);
  Log::setCardSink(cfg_.diag.logToCard && store_.cardMounted() && !bi.safeMode);
  display_.onConfig(cfg_);
  refreshRtcEarly();
  if (store_.loadMessage()[0]) message(store_.loadMessage(), 5000);
  if (store_.lastParseError()[0]) message(store_.lastParseError(), 5000);

  const bool sleepWake = bi.kind == BootKind::SleepWake && bi.rtcValid;
  // D. Battery: method and first reading; a sleep-wake carries the filter over.
  battery_.begin();
  if (sleepWake) battery_.restore(r);
  setFault(F_BATT, !battery_.valid());
  // F. Touch driver, shield, baselines (the RTC copy on a sleep-wake, else NVS) and the boot check;
  //    a fresh calibration shows the hint from the Boot tick. A touch wake is latched first (§4.1).
  //    Buttons and the command engine. G (BLE) is Phase 7.
  if (sleepWake && bi.wakeTouchGpio) touch_.latchWake(bi.wakeTouchGpio);
  else if (sleepWake && bi.wake == ESP_SLEEP_WAKEUP_TOUCHPAD) touch_.latchWakeUnknown();
  const uint32_t* rtcBase = sleepWake ? r.baselines : nullptr;
  if (!touch_.begin(cfg_, rtcBase)) setFault(F_TOUCH, true);
  buttons_.begin();
  cmds_.setDurations(durations());
  levels_.configure(levelPolicy(), cfg_.levels.count);
  press_.begin(this);
  haptics_.begin(); jacks_.begin();
  haptics_.configure(cfg_); jacks_.configure(cfg_);
  kbdInitPending_ = !bi.safeMode;                              // G runs from the first tick, after a latched wake press is consumed; never in safe mode
  // E/H. Volume (NVS, else the configuration), the sound cache (loader task), cue at J once the rail is ready.
  volume_.configure(cfg_.audio.maxGain, cfg_.audio.stepPct, cfg_.audio.clickVolume);
  if (sleepWake) { volume_.init(r.volumePct, r.muted); LOG_I(TAG, "volume %u%%%s (RtcState)", (unsigned)volume_.master(), r.muted ? ", muted" : ""); }
  else loadVolume();
  cache_.setBudget((uint32_t)cfg_.audio.cacheMaxMB * 1024u * 1024u);
  refHash_ = SoundCache::referenceHash(cfg_);
  if (!bi.safeMode) cache_.reload(cfg_, 0);                    // safe mode streams from the card instead
  startupCuePending_ = cfg_.audio.startupCue && bi.kind != BootKind::SleepWake;
  // I. Level 1 and mute cleared on a cold boot and an Off-wake; a sleep-wake restores them (§5.3, §5.4).
  levels_.restore(sleepWake ? r.level : 0); level_ = levels_.current();
  wokeFromOff_ = bi.kind == BootKind::OffWake;                 // §3.2 off-return rule
  padSinceWake_ = false;
  if (wokeFromOff_) cmds_.suppressOffUntilRelease();           // §4.2 rule 3
  { RtcState& rs = rtc::get(); rs.level = level_; rs.volumePct = volume_.master(); rs.muted = volume_.muted(); rs.wokeFromOff = wokeFromOff_; rtc::commit(); }
  normalScreen_.view = &view_;
  menu_.build();                                               // §14.3: the items from the descriptor table
  updater_.begin(*this, portal_);
  {                                                            // §16: the one-shot flag from an install or rollback, and what the boot record says happened
    Preferences p;
    if (p.begin("sb-state", false)) { resumeSetup_ = p.getUChar("resumeSetup", 0) != 0; if (resumeSetup_) p.remove("resumeSetup"); p.end(); }
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    esp_ota_img_states_t ost; bool otherBad = other && esp_ota_get_state_partition(other, &ost) == ESP_OK && (ost == ESP_OTA_IMG_INVALID || ost == ESP_OTA_IMG_ABORTED);
    const esp_partition_t* run = esp_ota_get_running_partition(); esp_ota_img_states_t rst;
    bool pending = run && esp_ota_get_state_partition(run, &rst) == ESP_OK && rst == ESP_OTA_IMG_PENDING_VERIFY;
    if (resumeSetup_ && otherBad && !pending) { rolledBack_ = true; LOG_E(TAG, "update failed, previous version restored (%s runs, the other slot is marked bad)", FW_VERSION); message("update failed, previous version restored", 8000); }
    else if (pending) LOG_W(TAG, "this image (%s) is pending: valid after the health check of §16, else rolled back at 90 s", FW_VERSION);
    if (resumeSetup_) LOG_I(TAG, "SETUP resumes once ACTIVE (an update or rollback asked for it)");
  }
  refreshView(R_ALL);
  LOG_I(TAG, "configuration loaded at +%lu ms; level 1", (unsigned long)millis());
  if (bi.kind == BootKind::SleepWake) LOG_I(TAG, "sleep-wake: level %u, %s%s", (unsigned)(level_ + 1), bi.wakeTouchGpio ? "touch wake" : "button wake", bi.rtcValid ? "" : " (RTC copy invalid: cold values)");
  if (bi.kind == BootKind::OffWake) LOG_I(TAG, "off-wake by button %s: OFF again after %u s without a pad press", (bi.wakeButtons & 1) ? "-" : "+", (unsigned)cfg_.power.offReturnS);
  next1s_ = millis() + 1000;
  lastInputMs_ = millis();
  levels_.onInput(lastInputMs_);
}

void AppState::enterActive(uint32_t now) {
  mode_ = AppMode::Active;
  hintShown_ = false;
  bootScreen_.calibrationHint = false;
  display_.setScreen(&normalScreen_);
  display_.setBrightness(cfg_.display.brightnessPct);
  cmds_.enableAt(now + 500);                                   // §17.2 K, §4.2 rule 7
  lastInputMs_ = now;
  levels_.onInput(now);
  LOG_I(TAG, "ACTIVE at +%lu ms (normal view)", (unsigned long)now);
  if (recoveryRequested_ && !setupOn_) {                       // §17.2: recovery starts SETUP with the current password on the card
    startupCuePending_ = false;
    startSetup("recovery (both buttons held through the reset)", true);
  } else if (resumeSetup_ && !setupOn_) {                      // §16: the page reconnects and sees the result
    resumeSetup_ = false;
    startSetup("after an update or rollback", false);
  }
}

void AppState::enterDimmed(uint32_t now) {
  mode_ = AppMode::Dimmed;
  display_.setBrightness(cfg_.display.dimPct);
  LOG_I(TAG, "DIMMED after %u s without input", (unsigned)cfg_.display.dimAfterS);
  (void)now;
}

void AppState::enterFault(const char* reason) {
  jacks_.allOff(); haptics_.stop();                            // §10: relays LOW in FAULT; §9.4
  kbd_.releaseAll("fault", millis());
  mode_ = AppMode::Fault;
  sb::copyStr(faultReason_, sizeof faultReason_, reason);
  faultScreen_.reason = faultReason_;
  if (display_.ready()) { display_.setScreen(&faultScreen_); display_.drawNow(); }
  LOG_E(TAG, "FAULT: %s", reason);
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------
void AppState::refreshView(uint8_t regions) {
  if (level_ >= cfg_.levels.count) level_ = 0;
  const sb::Level& L = cfg_.levels.levels[level_];
  view_.level = level_;
  view_.levelCount = cfg_.levels.count;
  sb::copyStr(view_.name, sizeof view_.name, L.name);
  view_.nSoundPads = sb::nSoundPads(cfg_);
  for (uint8_t i = 0; i < 4; i++) {
    if (i < view_.nSoundPads) sb::entryLabel(L.buttons[i], view_.labels[i], sizeof view_.labels[i]);
    else view_.labels[i][0] = 0;
  }
  view_.showLabels = cfg_.display.showLabels;
  view_.battValid = battery_.valid();
  view_.battPct = battery_.percent();
  view_.usb = Board::usbPresent();
  view_.lowBattery = battery_.valid() && battery_.percent() < cfg_.power.lowBatteryWarnPct;
  view_.volumePct = volume_.master();
  view_.muted = volume_.muted();
  updateStateWord(millis());                                   // §11.4 bottom line
  view_.faults = faults_;
  display_.dirty(regions);
}

void AppState::setFault(uint16_t bit, bool on) {
  uint16_t before = faults_;
  if (on) faults_ |= bit; else faults_ &= (uint16_t)~bit;
  if (faults_ != before) {
    for (uint8_t i = 0; i < N_FAULTS; i++) if (FAULTS[i].bit == bit) {
      if (on) LOG_E(TAG, "fault %s: %s", FAULTS[i].token, FAULTS[i].message); else LOG_I(TAG, "fault %s cleared", FAULTS[i].token);
    }
    view_.faults = faults_;
    display_.dirty(R_BOTTOM);
  }
}

void AppState::message(const char* text, uint32_t ms) {
  sb::copyStr(view_.message, sizeof view_.message, text);
  msgUntil_ = millis() + ms;
  if (!msgUntil_) msgUntil_ = 1;
  display_.dirty(R_NAME);
}

void AppState::registerInput(uint32_t now) {
  lastInputMs_ = now;
  levels_.onInput(now);                                        // §5.3: the return-to-level-1 timer runs from the last input of any kind
  if (mode_ == AppMode::Dimmed) {
    mode_ = AppMode::Active;
    display_.setBrightness(cfg_.display.brightnessPct);
    LOG_I(TAG, "input: ACTIVE again");
  }
}

// ---------------------------------------------------------------------------
// Configuration changes
// ---------------------------------------------------------------------------
void AppState::onConfigChanged() {
  Log::setLevel((LogLevel)cfg_.diag.logLevel);
  Log::setCardSink(cfg_.diag.logToCard && store_.cardMounted() && !bi_.safeMode);
  display_.onConfig(cfg_);
  touch_.onConfig(cfg_);
  cmds_.setDurations(durations());
  volume_.configure(cfg_.audio.maxGain, cfg_.audio.stepPct, cfg_.audio.clickVolume);
  if (playingPress_) audio_.setGain(volume_.gainFor(playingEntryVol_));
  cache_.setBudget((uint32_t)cfg_.audio.cacheMaxMB * 1024u * 1024u);
  uint32_t h = SoundCache::referenceHash(cfg_);
  if (h != refHash_) { refHash_ = h; if (audioStarted_) cache_.reload(cfg_, level_); }
  if (!cfg_.audio.outputs.speakers && ampOn_) ampEnable(false);
  refreshRtcEarly();
  levels_.configure(levelPolicy(), cfg_.levels.count);        // §5.3: clamp after a level edit
  haptics_.configure(cfg_); jacks_.configure(cfg_);
  kbd_.configure(cfg_);                                        // §8.4: the enable toggle acts at once
  level_ = levels_.current();
  press_.cancel();                                             // §5.2: a change to the levels ends a repeat
  jacks_.allOff();
  refreshView(R_ALL);
  Event e = { Ev::ConfigChanged, millis(), {} };
  EventBus::postCoalesced(e);
}

void AppState::refreshRtcEarly() {
  RtcState& r = rtc::get();
  r.early.wakeHoldMs = cfg_.power.wakeHoldMs;
  r.early.chargeCheckMin = cfg_.power.chargeCheckMin;
  r.early.ownerLabelMs = cfg_.device.ownerLabelMs;
  r.early.showChargingWhenOff = cfg_.power.showChargingWhenOff;
  memset(r.early.ownerLabel, 0, sizeof r.early.ownerLabel);
  for (uint8_t i = 0; i < cfg_.device.ownerLines && i < 3; i++) strlcpy(r.early.ownerLabel[i], cfg_.device.ownerLabel[i], 21);
  r.early.ownerLines = cfg_.device.ownerLines;
  r.early.theme = cfg_.display.theme; r.early.flip = cfg_.display.flip; r.early.dimPct = cfg_.display.dimPct;
  rtc::commit();
}

bool AppState::setSetting(const char* path, const char* value, char* err, size_t errLen) {
  if (!store_.setPath(path, value, cfg_, rep_, err, errLen)) return false;
  onConfigChanged();
  return true;
}

bool AppState::mergeSettings(const char* json, char* err, size_t errLen) {
  if (!store_.mergeText(json, cfg_, rep_, err, errLen)) return false;
  onConfigChanged();
  return true;
}

void AppState::saveThunk(void* arg) {
  AppState* self = static_cast<AppState*>(arg);
  self->lastSaveOk_ = self->store_.save(self->cfg_, true, self->lastSaveErr_, sizeof self->lastSaveErr_);
  if (self->lastSaveOk_) {
    self->setFault(F_CONFIG, self->store_.cardConfigBad());
    self->refreshRtcEarly();
    if (self->saveUserAction_) self->playCue(self->cfg_.audio.cues.saved, sb::ToneKind::Saved, self->volume_.clickGain());   // §6.5
  }
}

bool AppState::requestSave(bool userAction, char* err, size_t errLen) {
  saveUserAction_ = userAction;
  lastSaveOk_ = false; lastSaveErr_[0] = 0;
  if (!Storage::deferredWrite(&AppState::saveThunk, this)) { snprintf(err, errLen, "write queue full"); return false; }
  if (Storage::pending()) { snprintf(err, errLen, "queued until audio is idle"); return false; }
  if (!lastSaveOk_) snprintf(err, errLen, "%s", lastSaveErr_);
  return lastSaveOk_;
}

bool AppState::factory(char* err, size_t errLen) {
  bool ok = store_.factory(cfg_, err, errLen);
  rep_.clear();
  onConfigChanged();
  setFault(F_CONFIG, store_.cardConfigBad());
  { RtcState& rs = rtc::get(); memset(rs.baselines, 0, sizeof rs.baselines); rtc::commit(); }   // the next boot calibrates
  return ok;
}

// ---------------------------------------------------------------------------
// Inputs (§4), level and volume (Phase 2 subset of §5.3 and §6.2), overlays (§11.5)
// ---------------------------------------------------------------------------
void AppState::handleEvent(const Event& e, uint32_t now) {
  switch (e.type) {
    case Ev::PadDown: {
      if (mode_ == AppMode::Updating) break;                       // §16: pads ignored
      registerInput(now);
      if (setupCardShown_) hideSetupCard(now);                     // §15.2: the card gives way to the normal view for 10 s
      if (mode_ == AppMode::Menu) {                                // §14.4 (CP-9 layout): left half back / down, right half up (OK) / next; no sound plays
        LOG_I(TAG, "PadDown P%u in the menu [press %u]", e.pad.pos + 1, e.pad.pressId);
        menuKey(e.pad.pos == 0 ? MenuKey::Prev : e.pad.pos == 1 ? MenuKey::Down : e.pad.pos == 2 ? MenuKey::Up : MenuKey::Next, now);
        break;
      }
      padSinceWake_ = true;                                        // §3.2: a pad press ends the off-return rule
      const sb::Role role = e.pad.pos < 4 ? cfg_.pads.roles[e.pad.pos] : sb::Role::None;
      LOG_I(TAG, "PadDown P%u (ch%u) %+.1f%% [press %u]", e.pad.pos + 1, e.pad.ch, e.pad.delta / 10.0f, e.pad.pressId);
      identSeq_++; identCh_ = e.pad.ch; identPos_ = (uint8_t)(e.pad.pos + 1);   // §15.3 Identify pads
      if (role == sb::Role::Sound) { view_.pressedPad = (int8_t)sb::soundIndex(cfg_, e.pad.pos); display_.dirty(R_LABELS); }   // §5.2 step 0
      if (mode_ == AppMode::Active || mode_ == AppMode::Dimmed) press_.onPadDown(e.pad.pos, e.pad.pressId, now);           // steps 1, 5-7 or §5.3
      break;
    }
    case Ev::PadUp:
      registerInput(now);
      LOG_I(TAG, "PadUp P%u after %u ms (peak %+.1f%%) [press %u]", e.pad.pos + 1, e.pad.heldMs, e.pad.delta / 10.0f, e.pad.pressId);
      if (view_.pressedPad >= 0) { view_.pressedPad = -1; display_.dirty(R_LABELS); }
      press_.onPadUp(e.pad.pos, e.pad.pressId, now);                   // the sound plays on [V3]; a repeat ends
      break;
    case Ev::PadStuck:
      LOG_W(TAG, "PadStuck P%u: left out of the ranking until it releases", e.pad.pos + 1);
      { char m[32]; snprintf(m, sizeof m, "pad P%u stuck", e.pad.pos + 1); message(m, 3000); }
      press_.onPadStuck(e.pad.pos, now);
      break;
    case Ev::BtnEdge:
      registerInput(now);
      if (setupCardShown_ && (e.u32 >> 8)) hideSetupCard(now);
      LOG_D(TAG, "BtnEdge %c %s", (e.u32 & 0xFF) == 0 ? '-' : '+', (e.u32 >> 8) ? "down" : "up");
      break;
    case Ev::ButtonCommand:
      onButtonCommand((sb::ButtonCmd)e.command.id, now);
      break;
    case Ev::AudioStarted:
      digitalWrite(pins::SCOPE, LOW);
      if (e.audio.requestId == padDownReq_ && padDownReq_) LOG_I(TAG, "audio started %lu ms after PadDown [press %u]", (unsigned long)(now - padDownAt_), e.audio.requestId);
      else LOG_D(TAG, "audio started [request %u]", e.audio.requestId);
      break;
    case Ev::AudioDone:
      if (e.audio.requestId == padDownReq_ && padDownReq_) LOG_I(TAG, "audio done [request %u]%s after %lu ms", e.audio.requestId, e.audio.interrupted ? " (interrupted)" : "", (unsigned long)(now - padDownAt_));
      else LOG_I(TAG, "audio done [request %u]%s", e.audio.requestId, e.audio.interrupted ? " (interrupted)" : "");
      if (e.audio.requestId == playingPress_) playingPress_ = 0;
      press_.onAudioDone(e.audio.requestId, e.audio.interrupted, now);   // §5.2 step 7
      if (ampOn_ && !audio_.playing()) { ampOffAt_ = now + 300; if (!ampOffAt_) ampOffAt_ = 1; }   // §6.1 hold-off
      break;
    case Ev::RailReady:
      onRailReady(e.u32, now);
      break;
    case Ev::RailDown:
      railReady_ = false;
      break;
    case Ev::ConfigChanged:
    default:
      break;
  }
}

void AppState::onButtonCommand(sb::ButtonCmd c, uint32_t now) {
  LOG_I(TAG, "command: %s", sb::buttonCmdName(c));
  if (mode_ == AppMode::Menu) {                                  // §4.2 rule 5 / §14.4: the menu meanings
    switch (c) {
      case sb::ButtonCmd::VolumeDown: menuKey(MenuKey::Down, now); break;
      case sb::ButtonCmd::VolumeUp:   menuKey(MenuKey::Up, now); break;
      case sb::ButtonCmd::Off:        closeMenu(true, "both buttons held and released"); break;
      default: break;
    }
    return;
  }
  switch (c) {
    case sb::ButtonCmd::VolumeDown: stepVolume(-1, now, false); break;
    case sb::ButtonCmd::VolumeUp:   stepVolume(+1, now, false); break;
    case sb::ButtonCmd::Level1:     applyLevel(levels_.set(0, sb::LevelSource::Attendant), "both-button tap", now); break;
    case sb::ButtonCmd::PrevLevel:  applyLevel(levels_.prev(sb::LevelSource::Attendant), "long hold -", now); break;
    case sb::ButtonCmd::NextLevel:  applyLevel(levels_.next(sb::LevelSource::Attendant), "long hold +", now); break;
    case sb::ButtonCmd::Off:        enterOff("both buttons held and released"); break;
    case sb::ButtonCmd::Menu:       openMenu("both buttons held"); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Levels (§5.3), actions (§5.4), mute (§6.2) and the press host (§5.2)
// ---------------------------------------------------------------------------
sb::LevelPolicy AppState::levelPolicy() const {
  sb::LevelPolicy p;
  p.click = cfg_.levelChange.click;
  p.attendantCues = cfg_.levelChange.attendantCues;
  p.returnCue = cfg_.levelChange.returnCue;
  p.returnToFirstAfterS = cfg_.levelChange.returnToFirstAfterS;
  return p;
}

void AppState::applyLevel(const sb::LevelChange& c, const char* why, uint32_t now) {
  if (c.changed) {
    level_ = c.level;
    press_.onLevelChanged();                                     // a sound pad's repeat ends with the level
    jacks_.levelChanged();                                       // §10: a level change opens a followed jack
    kbd_.releaseAll("level change", now);                        // §8.3: a held key goes up with the level
    refreshView(R_MAIN | R_NAME | R_LABELS);                     // step 2: numeral, name, labels
    display_.drawSoon();
    { RtcState& rs = rtc::get(); rs.level = level_; rtc::commit(); }
    levelChangeAt_ = now ? now : 1;
    LOG_I(TAG, "level %u \"%s\" (%s)%s", (unsigned)(level_ + 1), cfg_.levels.levels[level_].name, why, c.cues ? ", click" : "");
  } else LOG_I(TAG, "level %u already (%s): numeral flashes%s", (unsigned)(level_ + 1), why, c.cues ? ", click" : "");
  view_.flashNumeral = true; flashUntil_ = now + 150; if (!flashUntil_) flashUntil_ = 1;   // §11.5
  display_.dirty(R_MAIN);
  if (c.cues) playCue(cfg_.audio.cues.click, sb::ToneKind::Click, volume_.clickGain());   // step 3, interrupting any sound
  if (c.changed && cfg_.vibration.enabled) {                   // §5.3 step 4 / §9.3, by source (the click setting does not gate the motor)
    sb::LevelPolicy pol = levelPolicy();
    bool pattern = false;
    switch (c.source) {
      case sb::LevelSource::Pad:       pattern = true; break;
      case sb::LevelSource::Attendant: pattern = pol.attendantCues; break;
      case sb::LevelSource::Action:    pattern = true; break;
      case sb::LevelSource::Return:    if (pol.returnCue) haptics_.singlePulse(now); break;   // "a single pulse"
      default: break;                                          // Portal: screen only
    }
    if (pattern) haptics_.levelCue(cfg_, level_, now);
  }
}

void AppState::keyPress(const sb::Entry& e, uint16_t pressId, uint32_t now) { kbd_.press(e, pressId, now); }      // §5.2 step 2
void AppState::keyRelease(uint16_t pressId, uint32_t now) { kbd_.release(pressId, now); }
void AppState::keyStuck(uint32_t now) { kbd_.releaseAll("stuck pad", now); }

void AppState::jackPress(uint8_t pos, const sb::Entry& e, uint16_t pressId, uint32_t now, bool repeat) {
  sb::JackResolved mode = sb::resolveJack(cfg_, level_, &e);
  if (repeat && mode != sb::JackResolved::Pulse) return;      // §5.2 step 7: step 3 again in pulse mode only
  jacks_.press(pos, mode, pressId, now);
}
void AppState::jackRelease(uint8_t pos, uint16_t pressId) { jacks_.release(pos, pressId); }
void AppState::jackStuck(uint8_t pos) { jacks_.stuck(pos); }
void AppState::confirmPulse(const sb::Entry& e, uint32_t now) { if (sb::resolveVibrate(cfg_, e)) haptics_.confirmPulse(now); }

void AppState::buzzTest() { haptics_.buzzTest(cfg_, level_, millis()); }
void AppState::jackTest(uint8_t jack) { jacks_.closeTest(jack, 1000, millis()); }

void AppState::toggleMute(uint32_t now) {
  bool on = !volume_.muted();
  volume_.mute(on);
  { RtcState& rs = rtc::get(); rs.muted = on; rtc::commit(); }
  view_.muted = on; view_.volumePct = volume_.master(); display_.dirty(R_TOP);
  if (on) {
    audio_.stop();                                               // §5.4: the current sound stops at once; every output is silent until un-muted
    playingPress_ = 0;
    sb::copyStr(view_.overlay, sizeof view_.overlay, "MUTE"); view_.overlayBar = false;
    LOG_I(TAG, "muted");
  } else {
    snprintf(view_.overlay, sizeof view_.overlay, "VOL %u", (unsigned)volume_.master()); view_.overlayBar = true; view_.overlayPct = volume_.master();
    playCue(cfg_.audio.cues.click, sb::ToneKind::Click, volume_.unmuteClickGain());   // §6.2: the one cue that plays through an un-mute
    LOG_I(TAG, "un-muted: volume %u%%, click", (unsigned)volume_.master());
  }
  overlayUntil_ = now + cfg_.display.volumePopupMs; if (!overlayUntil_) overlayUntil_ = 1;
  display_.dirty(R_MAIN);
}

bool AppState::padHeld(uint8_t pos) const { return simPos_ == (int8_t)pos || touch_.pad(pos).pressed; }

void AppState::pressLabel(const sb::Entry& e, uint32_t now) {
  if (!cfg_.display.pressLabelMs || overlayUntil_) return;      // §5.5: below the volume popup
  sb::entryLabel(e, view_.overlay, sizeof view_.overlay); view_.overlayBar = false;
  overlayUntil_ = now + cfg_.display.pressLabelMs; if (!overlayUntil_) overlayUntil_ = 1;
  display_.dirty(R_MAIN);
}

void AppState::playSound(const char* sound, uint8_t volumePct, uint16_t pressId, uint32_t now, bool repeat) {
  if (cfg_.audio.outputs.speakers) ampEnable(true);              // IO42 leads the first frame
  if (!audio_.ready()) { LOG_W(TAG, "%s: audio rail not ready", sound); return; }
  digitalWrite(pins::SCOPE, HIGH);                               // latency: HIGH here, LOW at AudioStarted
  padDownAt_ = now; padDownReq_ = pressId;
  playingPress_ = pressId; playingEntryVol_ = volumePct;
  audio_.play(sound, volume_.gainFor(volumePct), pressId);      // step 1 (the engine fades the old sound) and step 6
  if (repeat) LOG_I(TAG, "repeat %s (still held) [press %u]", sound, pressId);
}

void AppState::runAction(const sb::Entry& e, uint16_t pressId, uint32_t now) {
  switch (e.action) {
    case sb::Action::VolumeUp:
    case sb::Action::VolumeDown:
      stepVolume(e.action == sb::Action::VolumeUp ? +1 : -1, now, !e.sound[0]);   // the click at the new master volume ...
      if (e.sound[0]) playSound(e.sound, e.volumePct, pressId, now, false);       // ... unless the entry brings its own sound
      break;
    case sb::Action::Mute:          toggleMute(now); break;
    case sb::Action::NextLevel:     applyLevel(levels_.next(sb::LevelSource::Action), "next-level entry", now); break;
    case sb::Action::PreviousLevel: applyLevel(levels_.prev(sb::LevelSource::Action), "previous-level entry", now); break;
    case sb::Action::GoToLevel:     applyLevel(levels_.set(e.gotoLevel ? (uint8_t)(e.gotoLevel - 1) : 0, sb::LevelSource::Action), "go-to-level entry", now); break;
    default: break;
  }
}

void AppState::levelPad(uint16_t pressId, uint32_t now, bool repeat) {
  applyLevel(levels_.next(sb::LevelSource::Pad), repeat ? "level pad held" : "level pad", now);
  if (cfg_.jacks.levelPad && !repeat) {                        // §5.3 step 5: the level pad's jack, global mode, after the change opened the followed ones
    for (uint8_t pos = 0; pos < sb::N_PADS; pos++)
      if (cfg_.pads.roles[pos] == sb::Role::Level) { jacks_.press(pos, sb::resolveJack(cfg_, level_, nullptr), pressId, now); break; }
  }
}

void AppState::volumeChanged(uint32_t now, bool popup) {
  if (playingPress_) audio_.setGain(volume_.gainFor(playingEntryVol_));        // §6.4 SetGain: live during the sound
  { RtcState& rs = rtc::get(); rs.volumePct = volume_.master(); rs.muted = false; rtc::commit(); }
  view_.volumePct = volume_.master(); view_.muted = false; display_.dirty(R_TOP);
  if (popup) {
    snprintf(view_.overlay, sizeof view_.overlay, "VOL %u", (unsigned)volume_.master());
    view_.overlayBar = true; view_.overlayPct = volume_.master();
    overlayUntil_ = now + cfg_.display.volumePopupMs; if (!overlayUntil_) overlayUntil_ = 1;
    display_.dirty(R_MAIN);
  }
  volPersistAt_ = now + 2000; if (!volPersistAt_) volPersistAt_ = 1;             // §6.2: NVS once silent for 2 s (through the deferred write)
}

void AppState::stepVolume(int dir, uint32_t now, bool confirmClick) {
  bool wasMuted = volume_.muted();
  volume_.step(dir);
  volumeChanged(now, true);
  LOG_I(TAG, "volume %u%%%s", (unsigned)volume_.master(), wasMuted ? " (un-muted)" : "");
  if (confirmClick) playCue(cfg_.audio.cues.click, sb::ToneKind::Click, volume_.volumeActionClickGain());   // §5.4: at the new master volume
  else if (wasMuted) playCue(cfg_.audio.cues.click, sb::ToneKind::Click, volume_.unmuteClickGain());       // §6.5: the un-mute click
}

// ---------------------------------------------------------------------------
// Audio (Phase 3 subset of §5.2, §6.1, §6.5, §7.1)
// ---------------------------------------------------------------------------
void AppState::loadVolume() {
  Preferences p;
  uint8_t v = 255;
  if (p.begin("sb-state", true)) { v = p.getUChar("volume", 255); p.end(); }
  volume_.init(v <= 100 ? v : cfg_.audio.volumePct, false);
  LOG_I(TAG, "volume %u%% (%s)", (unsigned)volume_.master(), v <= 100 ? "NVS" : "configuration");
}

void AppState::tickPower(uint32_t now) {
  if (mode_ == AppMode::Fault) return;
  battery_.tick(now, !audio_.playing() && !ampOn_);            // §12.1: quiet windows preferred
  if (battery_.changed()) onBatterySample(now);
  if (emptyAt_ && due(now, emptyAt_)) { emptyAt_ = 0; enterOff("battery empty"); }
}

void AppState::onBatterySample(uint32_t now) {
  setFault(F_BATT, !battery_.valid());
  view_.battValid = battery_.valid(); view_.battPct = battery_.percent(); view_.usb = battery_.usb();
  view_.lowBattery = battery_.valid() && battery_.percent() < cfg_.power.lowBatteryWarnPct;
  display_.dirty(R_TOP);
  if (!battery_.valid()) return;
  if (view_.lowBattery && !lowWarned_) {                       // §12.6: red row, the cue once per boot (only if a file is set)
    lowWarned_ = true;
    LOG_W(TAG, "battery low: %u %% (%u mV)", (unsigned)battery_.percent(), (unsigned)battery_.millivolts());
    if (cfg_.audio.cues.lowBattery[0] && cache_.stateOf(cfg_.audio.cues.lowBattery) == S_CACHED) playCue(cfg_.audio.cues.lowBattery, sb::ToneKind::Click, volume_.clickGain());
  }
  if (!battery_.usb() && battery_.percent() <= cfg_.power.shutdownPct && !emptyAt_ && (mode_ == AppMode::Active || mode_ == AppMode::Dimmed || mode_ == AppMode::Menu)) {
    LOG_E(TAG, "battery empty (%u %%, %u mV): OFF in 3 s", (unsigned)battery_.percent(), (unsigned)battery_.millivolts());
    if (mode_ == AppMode::Menu) closeMenu(true, "battery empty");
    msgScreen_.text = "BATTERY EMPTY"; msgScreen_.sub = nullptr; msgScreen_.scale = 3;
    display_.setScreen(&msgScreen_);
    emptyAt_ = now + 3000; if (!emptyAt_) emptyAt_ = 1;
  }
}

void AppState::volumeWriteThunk(void* arg) {
  AppState* self = static_cast<AppState*>(arg);
  Preferences p;
  if (p.begin("sb-state", false)) { p.putUChar("volume", self->volume_.master()); p.end(); }
  self->volume_.clearDirty();
  LOG_D(TAG, "volume %u%% written to NVS", (unsigned)self->volume_.master());
}

void AppState::ampEnable(bool on) {
  if (on == ampOn_) return;
  ampOn_ = on;
  digitalWrite(pins::AMP_EN, on ? HIGH : LOW);                                 // §6.1: amps on demand, hold-off after AudioDone
  ampOffAt_ = 0;
}

void AppState::playCue(const char* file, sb::ToneKind fallback, float gain) {
  if (!audio_.ready() || gain <= 0.0f) return;                  // muted, or clickVolume 0: silent, and the amp stays off
  uint16_t id = touch_.nextPressId();
  if (cfg_.audio.outputs.speakers) ampEnable(true);
  if (file && *file && cache_.stateOf(file) == S_CACHED) audio_.play(file, gain, id);
  else audio_.tone(fallback, gain, id);
  playingPress_ = 0;
}

void AppState::onRailReady(uint32_t flags, uint32_t now) {
  railReady_ = true;
  bool i2sOk = flags & 2, banner = flags & 1;
  setFault(F_AUDIO, !i2sOk);
  // A reset (flash, `reboot`, crash) leaves the 5 V rail up and the module running silently in whatever state it had (bench 2026-09-14):
  // no banner is not proof of a dead module, so ask it (AT+ -> OK+) before raising !BT; and AT+POWER_OFF goes out whether or not it spoke.
  if (!banner && kcx_.open()) { kcx_.send("AT+"); btProbeAt_ = now + 600; if (!btProbeAt_) btProbeAt_ = 1; }
  else setFault(F_BT, false);
  if (!cfg_.bluetoothSpeaker.enabled) { kcxPowerOffAt_ = (banner ? kcx_.powerOnAtMs() : now) + 500; if ((int32_t)(now - kcxPowerOffAt_) > 0) kcxPowerOffAt_ = now; if (!kcxPowerOffAt_) kcxPowerOffAt_ = 1; }   // §7.1
  if (cfg_.bluetoothSpeaker.enabled) { btNotLinkedSince_ = now ? now : 1; btWasLinked_ = false; }   // §7.5: a wake or an enable restarts the 10 s before NO BT SPEAKER
  // §17.2 J: the startup cue plays from tick() once its file is cached (or known missing), so the file wins over the built-in.
}

void AppState::playFile(const char* name) {
  if (!audio_.ready()) { Serial.println("audio rail not ready"); return; }
  uint16_t id = touch_.nextPressId();
  if (cfg_.audio.outputs.speakers) ampEnable(true);
  digitalWrite(pins::SCOPE, HIGH);
  padDownAt_ = millis(); padDownReq_ = id;
  playingPress_ = id; playingEntryVol_ = 100;
  audio_.play(name, volume_.gainFor(100), id);
  Serial.printf("playing %s [request %u]\n", name, id);
}

bool AppState::setSpeakers(bool on) {
  if (on == cfg_.audio.outputs.speakers) return true;
  char err[64];
  if (!setSetting("audio.outputs.speakers", on ? "true" : "false", err, sizeof err)) { LOG_W(TAG, "speakers: %s", err); return false; }
  if (!on) ampEnable(false);
  LOG_I(TAG, "on-board speakers %s (RAM; `save` or the menu exit keeps it)", on ? "ON" : "off");
  return true;
}

bool AppState::toggleSpeakers() { setSpeakers(!cfg_.audio.outputs.speakers); return cfg_.audio.outputs.speakers; }

bool AppState::toggleKeyboard() {
  char err[64];
  bool on = !cfg_.keyboard.enabled;
  if (!setSetting("keyboard.enabled", on ? "true" : "false", err, sizeof err)) { LOG_W(TAG, "keyboard: %s", err); return cfg_.keyboard.enabled; }
  return on;
}

void AppState::linkMessage(const char* text, uint32_t ms, uint32_t now) {   // §11.4 priority 3: a link change, in plain words
  sb::copyStr(view_.link, sizeof view_.link, text);
  linkUntil_ = now + ms; if (!linkUntil_) linkUntil_ = 1;
  display_.dirty(R_BOTTOM);
  LOG_I(TAG, "screen: %s", text);
}

void AppState::updateStateWord(uint32_t now) {                // §11.4 priority 4: one dim word while something is missing
  const char* w = "";
  if (cfg_.keyboard.enabled && kbd_.initOk() && kbd_.link() != BleKeyboard::Link::Ready && kbd_.notReadySince() && due(now, kbd_.notReadySince() + 10000)) w = "NO TABLET";
  else if (cfg_.bluetoothSpeaker.enabled && !kcx_.linked() && btNotLinkedSince_ && due(now, btNotLinkedSince_ + 10000)) w = "NO BT SPEAKER";   // §7.5
  else if (cfg_.audio.outputs.speakers) w = "SPEAKERS ON";
  if (strcmp(w, view_.stateWord)) { sb::copyStr(view_.stateWord, sizeof view_.stateWord, w); display_.dirty(R_BOTTOM); }
}

bool AppState::toggleBluetooth() { setBluetooth(!cfg_.bluetoothSpeaker.enabled); return cfg_.bluetoothSpeaker.enabled; }

bool AppState::setBluetooth(bool on) {
  if (on == cfg_.bluetoothSpeaker.enabled) return true;
  char err[64];
  if (!setSetting("bluetoothSpeaker.enabled", on ? "true" : "false", err, sizeof err)) { LOG_W(TAG, "bluetooth: %s", err); return false; }
  uint32_t now = millis();
  if (on) {
    setFault(F_BT, false);
    btNotLinkedSince_ = now ? now : 1; btWasLinked_ = false;                    // §7.5: the 10 s restarts
    if (audio_.playing()) { btCyclePending_ = true; LOG_I(TAG, "bluetooth speaker ON: the rail cycle waits for the sound to end (§7.1)"); }
    else { LOG_I(TAG, "bluetooth speaker ON: cycling the 5 V rail so the module boots"); audio_.railDown(); audio_.railUp(); }   // queued in order; ~0.5 s without wired audio
  } else {
    LOG_I(TAG, "bluetooth speaker off: AT+POWER_OFF");
    btCyclePending_ = false;
    if (kcx_.pair() != KcxLink::Pair::None) { kcx_.cancelPair(); overlayUntil_ = 0; view_.overlay[0] = 0; view_.overlayBar = false; display_.dirty(R_MAIN); }
    kcx_.powerOff();
    btWasLinked_ = false; btNotLinkedSince_ = 0;                                // no SPEAKER LOST for a deliberate off; NO SPEAKER clears
    if (linkUntil_ && !strncmp(view_.link, "BT SPEAKER", 10)) { linkUntil_ = 0; view_.link[0] = 0; display_.dirty(R_BOTTOM); }
    setFault(F_BT, false);
  }
  updateStateWord(now);
  return true;
}

bool AppState::startPairing(bool wipe) {                       // §7.3
  uint32_t now = millis();
  if (!cfg_.bluetoothSpeaker.enabled) { LOG_W(TAG, "pairing: the Bluetooth speaker is disabled (`bt` first)"); return false; }
  if (!railReady_) { LOG_W(TAG, "pairing: the audio rail is not up yet"); return false; }
  if (kcx_.pair() == KcxLink::Pair::Wiping || kcx_.pair() == KcxLink::Pair::Searching) { LOG_W(TAG, "pairing: already searching"); return false; }
  if (!kcx_.startPair(wipe, now)) return false;
  pairTickAt_ = 0;                                              // the overlay draws on the next tick
  linkMessage("PAIR THE BT SPEAKER", KcxLink::PAIR_MS + 1000, now);
  return true;
}

void AppState::tickBluetooth(uint32_t now) {                   // §7
  // A deferred rail cycle (`bt` on while a sound played, §7.1).
  if (btCyclePending_ && !audio_.playing()) { btCyclePending_ = false; LOG_I(TAG, "bluetooth speaker ON: cycling the 5 V rail now"); audio_.railDown(); audio_.railUp(); }
  if (btProbeAt_ && due(now, btProbeAt_)) {
    btProbeAt_ = 0;
    bool alive = kcx_.alive();
    LOG_I(TAG, "KCX after a silent rail-up: %s", alive ? "alive (answered AT+), it kept its state through the reset" : "no reply: soft-off from before the reset, or missing");
    if (cfg_.bluetoothSpeaker.enabled && !alive && !btRecycled_ && !audio_.playing()) {
      btRecycled_ = true;                                        // once: a soft-off module only reboots with its rail
      LOG_I(TAG, "bluetooth speaker enabled but the module is silent: cycling the 5 V rail once");
      audio_.railDown(); audio_.railUp();
    } else setFault(F_BT, cfg_.bluetoothSpeaker.enabled && !alive);
  }
  // Link edges in plain words (§7.5, §11.4).
  bool linked = kcx_.linked();
  if (linked != btWasLinked_) {
    btWasLinked_ = linked;
    if (cfg_.bluetoothSpeaker.enabled && railReady_) {
      if (linked) { linkMessage("BT SPEAKER LINKED", 4000, now); btNotLinkedSince_ = 0; }
      else        { linkMessage("BT SPEAKER LOST", 4000, now); btNotLinkedSince_ = now ? now : 1; }
    }
  }
  // Pairing (§7.3): the overlay and the outcome.
  KcxLink::Pair p = kcx_.pair();
  if (p == KcxLink::Pair::Wiping || p == KcxLink::Pair::Searching) {
    if (due(now, pairTickAt_)) {
      pairTickAt_ = now + 1000;
      uint32_t left = kcx_.pairLeftMs(now);
      sb::copyStr(view_.overlay, sizeof view_.overlay, "PAIRING");
      view_.overlayBar = true; view_.overlayPct = (uint8_t)(left * 100 / KcxLink::PAIR_MS);
      overlayUntil_ = now + 1500; if (!overlayUntil_) overlayUntil_ = 1;   // re-asserted every second, so a volume popup only borrows the spot
      if (!view_.link[0]) linkMessage("PAIR THE BT SPEAKER", left + 1000, now);
      display_.dirty(R_MAIN);
    }
  } else if (p == KcxLink::Pair::Connected || p == KcxLink::Pair::Timeout) {
    kcx_.pairAck();
    overlayUntil_ = 0; view_.overlay[0] = 0; view_.overlayBar = false; display_.dirty(R_MAIN);
    if (p == KcxLink::Pair::Connected) {
      if (strcmp(view_.link, "BT SPEAKER LINKED")) linkMessage("BT SPEAKER LINKED", 4000, now);   // usually the link edge above said it already
      playCue(cfg_.audio.cues.saved, sb::ToneKind::Saved, volume_.clickGain());                    // the module has saved it; the cue confirms
    } else linkMessage("NO BT SPEAKER FOUND", 5000, now);           // the menu/portal then offer forget-and-pair (§7.3 step 4)
    if (mode_ == AppMode::Menu) menuPairResult(p == KcxLink::Pair::Connected, now);
  }
}

void AppState::updateHoldBar(uint32_t now) {
  sb::HoldProgress p = cmds_.progress(now);
  p.pct = (uint8_t)(p.pct / 4 * 4);                               // coarse steps: one redraw per 4 %
  if (p == hold_) return;
  hold_ = p;
  view_.holdKind = (uint8_t)p.kind; view_.holdPct = p.pct; view_.holdOffReached = p.offReached; view_.holdSeconds = p.secondsLeft;
  display_.dirty(R_BOTTOM);
}

void AppState::tickOverlays(uint32_t now) {
  if (overlayUntil_ && due(now, overlayUntil_)) { overlayUntil_ = 0; view_.overlay[0] = 0; view_.overlayBar = false; display_.dirty(R_MAIN); }
  if (flashUntil_ && due(now, flashUntil_)) { flashUntil_ = 0; view_.flashNumeral = false; display_.dirty(R_MAIN); }
  if (simPos_ >= 0 && due(now, simUntil_)) {
    Event e = { Ev::PadUp, now, {} };
    e.pad.pos = (uint8_t)simPos_; e.pad.ch = cfg_.hardware.padChannels[simPos_]; e.pad.delta = 50; e.pad.pressId = simPressId_;
    uint32_t held = now - simStart_;
    e.pad.heldMs = held > 65535 ? 65535 : (uint16_t)held;
    simPos_ = -1;
    EventBus::post(e);
  }
}

void AppState::simulatePress(uint8_t pos, uint32_t ms) {
  if (pos > 3) return;
  if (simPos_ >= 0 || touch_.anyPressed()) { Serial.println("a press is already in progress"); return; }
  uint32_t now = millis();
  if (ms < 20) ms = 100; if (ms > 60000) ms = 60000;
  simPos_ = (int8_t)pos; simPressId_ = touch_.nextPressId(); simUntil_ = now + ms; simStart_ = now;
  Event e = { Ev::PadDown, now, {} };
  e.pad.pos = pos; e.pad.ch = cfg_.hardware.padChannels[pos]; e.pad.delta = 50; e.pad.pressId = simPressId_; e.pad.heldMs = 0;
  EventBus::post(e);
  Serial.printf("simulated press P%u for %lu ms\n", pos + 1, (unsigned long)ms);
}

void AppState::simulateCommand(sb::ButtonCmd c) {
  Event e = { Ev::ButtonCommand, millis(), {} };
  e.command.id = (uint8_t)c;
  EventBus::post(e);
}

void AppState::simulateClick(int dir) { simulateCommand(dir < 0 ? sb::ButtonCmd::VolumeDown : sb::ButtonCmd::VolumeUp); }

void AppState::recalibrate() {
  if (touch_.failed()) { Serial.println("touch driver is down"); return; }
  touch_.calibrate();
  if (mode_ == AppMode::Active || mode_ == AppMode::Dimmed || mode_ == AppMode::Menu) {
    bootScreen_.calibrationHint = true; bootScreen_.recoveryPrompt = false;
    display_.setScreen(&bootScreen_);
    hintShown_ = true;
  }
}

// ---------------------------------------------------------------------------
// Card recovery and bench test
// ---------------------------------------------------------------------------
bool AppState::cardRecover(uint32_t offMs) {
  LOG_W(TAG, "card recovery: gated rail off for %lu ms", (unsigned long)offMs);
  store_.unmountCard();
  Screen* current = mode_ == AppMode::Active || mode_ == AppMode::Dimmed ? (Screen*)&normalScreen_ : mode_ == AppMode::Menu ? (Screen*)&menuScreen_ : mode_ == AppMode::Boot ? (Screen*)&bootScreen_ : (Screen*)&faultScreen_;
  display_.powerDown();
  Board::gatedRail(false);
  uint32_t t0 = millis();
  while (millis() - t0 < offMs) { esp_task_wdt_reset(); delay(50); }
  // Is the card really unpowered? With SD CS low it would drive MISO high if it still had power.
  pinMode(pins::SPI_MISO, INPUT_PULLDOWN); delay(5);
  int miso = digitalRead(pins::SPI_MISO);
  pinMode(pins::SPI_MISO, INPUT);
  LOG_W(TAG, "card recovery: after %lu ms off, MISO reads %d with a pull-down (1 = the card is still powered)", (unsigned long)offMs, miso);
  display_.begin(cfg_);                      // rail on, screen re-init, fade in
  display_.setScreen(current);
  display_.drawNow();
  bool ok = store_.mountCard();
  setFault(F_CARD, !ok);
  LOG_I(TAG, "card recovery: remount %s", ok ? "ok" : "FAILED");
  return ok;
}

bool AppState::checkCard() {
  bool ok = store_.probeCard();
  setFault(F_CARD, !ok);
  return ok;
}

bool AppState::cardWriteTest(bool psramBuffer, uint32_t chunk, Print& out) {
  if (chunk < 64 || chunk > 4096) chunk = 4096;
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, psramBuffer ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL));   // on demand: bench only
  if (!buf) { out.println(psramBuffer ? "no PSRAM" : "no internal RAM"); return false; }
  if (!store_.mountCard()) { out.println("no card"); free(buf); return false; }
  const char* path = "/sbtest.bin";
  const uint32_t total = 16 * 1024;
  bool ok = true;
  uint32_t crcW = 0, crcR = 0;
  {
    Storage::Guard g;
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { out.println("cannot create the test file"); ok = false; }
    else {
      uint32_t x = 0xC0FFEE42;
      uint32_t t0 = millis();
      for (uint32_t done = 0; done < total && ok; done += chunk) {
        for (uint32_t i = 0; i < chunk; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (uint8_t)x; }
        crcW = esp_rom_crc32_le(crcW, buf, chunk);
        size_t w = f.write(buf, chunk);
        if (w != chunk) { out.printf("write failed at %lu: %u bytes, errno %d\n", (unsigned long)done, (unsigned)w, errno); ok = false; }
      }
      out.printf("wrote %lu bytes in %lu ms (%lu-byte writes)\n", (unsigned long)total, (unsigned long)(millis() - t0), (unsigned long)chunk);
      f.flush(); f.close();
    }
    if (ok) {
      File r = SD.open(path, FILE_READ);
      if (!r || r.size() != total) { out.println("reopen failed"); ok = false; }
      else {
        for (uint32_t done = 0; done < total && ok; done += 4096) {
          int n = r.read(buf, 4096);
          if (n != 4096) { out.printf("read failed at %lu: %d\n", (unsigned long)done, n); ok = false; break; }
          crcR = esp_rom_crc32_le(crcR, buf, 4096);
        }
        r.close();
      }
    }
    SD.remove(path);
  }
  if (ok && crcW != crcR) { out.println("verify mismatch"); ok = false; }
  out.printf("card write test (%s buffer, %lu-byte writes, %lu Hz): %s\n", psramBuffer ? "PSRAM" : "internal", (unsigned long)chunk, (unsigned long)store_.sdHz(), ok ? "OK" : "FAILED");
  if (!ok) checkCard();
  free(buf);
  return ok;
}

// Raw SD SPI helpers for the bench test (SPI mode).
static bool s_rawCrc = false;
static uint8_t rawXfer(uint8_t b) { return SPI.transfer(b); }
static uint8_t crc7(const uint8_t* d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) { uint8_t b = d[i]; for (int k = 0; k < 8; k++) { crc <<= 1; if ((b ^ crc) & 0x80) crc ^= 0x09; b <<= 1; } crc &= 0x7F; }
  return crc;
}
static uint16_t crc16(const uint8_t* d, int n) {
  uint16_t crc = 0;
  for (int i = 0; i < n; i++) { crc ^= (uint16_t)d[i] << 8; for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1); }
  return crc;
}
static uint8_t rawCmd(uint8_t cmd, uint32_t arg) {
  uint8_t pkt[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0x01 };
  if (s_rawCrc || cmd == 0 || cmd == 8) pkt[5] = (uint8_t)((crc7(pkt, 5) << 1) | 1);
  else if (cmd == 0) pkt[5] = 0x95;
  rawXfer(0xFF);
  for (int i = 0; i < 6; i++) rawXfer(pkt[i]);
  uint8_t r = 0xFF;
  for (int i = 0; i < 16 && (r & 0x80); i++) r = rawXfer(0xFF);
  return r;
}
static bool rawWaitBusy(uint32_t ms) {          // like the driver's sdWait: returns at the first non-zero byte
  uint32_t t0 = millis();
  while (rawXfer(0xFF) == 0x00) { if (millis() - t0 > ms) return false; }
  return true;
}
static bool rawReadSector(uint32_t addr, uint8_t* dst, uint8_t& r1, uint8_t& tok) {
  r1 = rawCmd(17, addr);
  tok = 0xFF; uint32_t t = millis();
  while (tok == 0xFF && millis() - t < 500) tok = rawXfer(0xFF);
  if (r1 != 0 || tok != 0xFE) return false;
  for (int i = 0; i < 512; i++) dst[i] = rawXfer(0xFF);
  rawXfer(0xFF); rawXfer(0xFF);
  return true;
}
static uint32_t rd32le(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool AppState::cardRawTest(uint32_t hz, bool crcOn, bool libSeq, bool lowRegion, bool dark, int nSectors, Print& out) {
  uint8_t savedBrightness = display_.brightness();
  if (dark) { display_.setBrightness(0); for (int i = 0; i < 40; i++) { display_.tick(millis()); delay(5); } }
  if (hz < 100000 || hz > 20000000) hz = 4000000;
  if (nSectors < 1 || nSectors > 32) nSectors = 32;
  uint64_t bytes = store_.cardBytes();
  store_.unmountCard();
  if (!bytes) { bytes = 4ULL * 1024 * 1024 * 1024; out.println("  card size unknown: assuming 4 GB"); }
  typedef uint8_t Sector[512];
  static Sector* orig = static_cast<Sector*>(heap_caps_malloc(32 * 512, MALLOC_CAP_SPIRAM));   // bench only; PSRAM (internal RAM is Wi-Fi's, CP-10)
  static uint8_t pat[512], back[512];
  if (!orig) { out.println("no PSRAM"); return false; }
  bool ok = true;
  Storage::Guard g;
  s_rawCrc = false;
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  pinMode(pins::SD_CS, OUTPUT); digitalWrite(pins::SD_CS, HIGH);
  digitalWrite(pins::TFT_CS, HIGH);
  for (int i = 0; i < 10; i++) rawXfer(0xFF);
  digitalWrite(pins::SD_CS, LOW);
  uint8_t r = rawCmd(0, 0);
  if (r != 0x01) { out.printf("  CMD0 -> 0x%02X: card does not answer\n", r); digitalWrite(pins::SD_CS, HIGH); SPI.endTransaction(); store_.mountCard(); return false; }
  r = rawCmd(8, 0x1AA); for (int i = 0; i < 4; i++) rawXfer(0xFF);
  uint32_t t0 = millis();
  do { rawCmd(55, 0); r = rawCmd(41, 0x40000000); } while (r != 0x00 && millis() - t0 < 2000);
  r = rawCmd(58, 0);
  uint8_t ocr[4]; for (int i = 0; i < 4; i++) ocr[i] = rawXfer(0xFF);
  bool sdhc = ocr[0] & 0x40;
  if (crcOn) { r = rawCmd(59, 1); s_rawCrc = true; out.printf("  CMD59 (CRC on) -> 0x%02X\n", r); }
  out.printf("  init ok: ACMD41 in %lu ms, %s addressing, CRC %s, sequence %s\n", (unsigned long)(millis() - t0), sdhc ? "block" : "byte", crcOn ? "on" : "off", libSeq ? "driver-style (deselect, reselect, CMD13)" : "plain");

  uint32_t sector = (uint32_t)(bytes / 512) - 2 - nSectors;
  {
    // Prefer the partition table: the card's own size field came back corrupted once.
    uint8_t r1, tok, mbr[512];
    if (rawReadSector(0, mbr, r1, tok) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
      uint32_t pStart = rd32le(mbr + 0x1BE + 8), pLen = rd32le(mbr + 0x1BE + 12);
      if (pStart && pLen > 4096) { sector = pStart + pLen - 64 - nSectors; out.printf("  partition %lu..%lu; testing near its end\n", (unsigned long)pStart, (unsigned long)(pStart + pLen)); }
    }
  }
  if (lowRegion) {
    // MBR -> partition start; VBR -> data area start (where FATFS allocates the first files).
    uint8_t r1, tok, mbr[512];
    if (!rawReadSector(0, mbr, r1, tok)) { out.println("  cannot read the MBR"); ok = false; }
    else {
      uint32_t part = rd32le(mbr + 0x1BE + 8);
      if (mbr[510] != 0x55 || mbr[511] != 0xAA) out.println("  MBR signature missing");
      uint8_t vbr[512];
      uint32_t vbrAddr = sdhc ? part : part * 512;
      if (part == 0 || !rawReadSector(vbrAddr, vbr, r1, tok)) { out.printf("  cannot read the VBR at %lu\n", (unsigned long)part); ok = false; }
      else {
        uint16_t bps = vbr[11] | (vbr[12] << 8); uint8_t spc = vbr[13]; uint16_t rsv = vbr[14] | (vbr[15] << 8); uint8_t nf = vbr[16];
        uint32_t fatSz = rd32le(vbr + 36);
        uint32_t dataStart = part + rsv + nf * fatSz;
        out.printf("  partition at %lu, %u B/sector, %u sectors/cluster, %u reserved, %u FATs of %lu sectors, data from sector %lu\n",
                   (unsigned long)part, bps, spc, rsv, nf, (unsigned long)fatSz, (unsigned long)dataStart);
        sector = dataStart + (uint32_t)spc * 4;     // a few clusters in: where the first files land
      }
    }
  }
  out.printf("  testing %d sectors from %lu\n", nSectors, (unsigned long)sector);

  int failWrite = 0, failStatus = 0, failVerify = 0, failRead = 0;
  bool cardReset = false;
  uint8_t firstBadTok = 0, firstBadR2 = 0;
  for (int n = 0; ok && n < nSectors; n++) {
    uint32_t addr = sdhc ? (sector + n) : (sector + n) * 512;
    uint8_t r1, tok;
    if (!rawReadSector(addr, orig[n], r1, tok)) { out.printf("  sector %d: original read failed (R1 0x%02X token 0x%02X)\n", n, r1, tok); ok = false; break; }
  }
  for (int n = 0; ok && n < nSectors; n++) {
    uint32_t addr = sdhc ? (sector + n) : (sector + n) * 512;
    for (int i = 0; i < 512; i++) pat[i] = (uint8_t)(i * 7 + n * 13 + 3);
    uint16_t c = crcOn ? crc16(pat, 512) : 0xFFFF;
    if (libSeq) { digitalWrite(pins::SD_CS, LOW); rawWaitBusy(500); }
    uint8_t r1 = rawCmd(24, addr);
    if (r1 & 0x01) { out.printf("  sector %d: CMD24 -> 0x%02X: the card is back in idle state (it reset itself); stopping\n", n, r1); failWrite++; cardReset = true; break; }
    if (r1 != 0) { out.printf("  sector %d: CMD24 -> 0x%02X\n", n, r1); failWrite++; continue; }
    rawWaitBusy(500);
    rawXfer(0xFE);
    for (int i = 0; i < 512; i++) rawXfer(pat[i]);
    rawXfer(c >> 8); rawXfer(c & 0xFF);
    uint8_t tok = rawXfer(0xFF) & 0x1F;
    if (tok != 0x05) { if (!failWrite) firstBadTok = tok; failWrite++; }
    if (libSeq) {
      digitalWrite(pins::SD_CS, HIGH); rawXfer(0xFF);
      digitalWrite(pins::SD_CS, LOW);
      rawWaitBusy(500);                              // the driver's sdWait: first non-zero byte ends it
      uint8_t s1 = rawCmd(13, 0); uint8_t s2 = rawXfer(0xFF);
      if (s1 != 0 || s2 != 0) { if (!failStatus) firstBadR2 = s2; failStatus++; }
      digitalWrite(pins::SD_CS, HIGH); rawXfer(0xFF);
      digitalWrite(pins::SD_CS, LOW);
      rawWaitBusy(1000);
    } else {
      rawWaitBusy(1000);
      uint8_t s1 = rawCmd(13, 0); uint8_t s2 = rawXfer(0xFF);
      if (s1 != 0 || s2 != 0) { if (!failStatus) firstBadR2 = s2; failStatus++; }
    }
    uint8_t rr1, rtok;
    if (!rawReadSector(addr, back, rr1, rtok)) failRead++;
    else { int bad = 0; for (int i = 0; i < 512; i++) if (back[i] != pat[i]) bad++; if (bad) failVerify++; }
  }
  // Restore (after a re-init if the card reset itself).
  s_rawCrc = false;
  if (cardReset) {
    digitalWrite(pins::SD_CS, HIGH); for (int i = 0; i < 10; i++) rawXfer(0xFF); digitalWrite(pins::SD_CS, LOW);
    rawCmd(0, 0); rawCmd(8, 0x1AA); for (int i = 0; i < 4; i++) rawXfer(0xFF);
    t0 = millis(); do { rawCmd(55, 0); r = rawCmd(41, 0x40000000); } while (r != 0x00 && millis() - t0 < 2000);
    out.printf("  re-init after the reset: ACMD41 -> 0x%02X\n", r);
  } else if (crcOn) rawCmd(59, 0);
  int failRestore = 0;
  for (int n = 0; ok && n < nSectors; n++) {
    uint32_t addr = sdhc ? (sector + n) : (sector + n) * 512;
    uint8_t r1 = rawCmd(24, addr);
    if (r1 != 0) { failRestore++; continue; }
    rawWaitBusy(500); rawXfer(0xFE);
    for (int i = 0; i < 512; i++) rawXfer(orig[n][i]);
    rawXfer(0xFF); rawXfer(0xFF);
    if ((rawXfer(0xFF) & 0x1F) != 0x05) failRestore++;
    rawWaitBusy(1000);
  }
  digitalWrite(pins::SD_CS, HIGH); rawXfer(0xFF);
  SPI.endTransaction();
  bool pass = ok && !failWrite && !failStatus && !failVerify && !failRead;
  out.printf("raw test %lu Hz, %d sectors%s: %s | card reset itself: %s | data-response rejects %d (first token 0x%02X) | status errors %d (first R2 0x%02X) | read fails %d | verify mismatches %d | restore fails %d\n",
             (unsigned long)hz, nSectors, dark ? ", backlight off" : "", pass ? "OK" : "FAILED", cardReset ? "YES" : "no", failWrite, firstBadTok, failStatus, firstBadR2, failRead, failVerify, failRestore);
  if (dark) display_.setBrightness(savedBrightness);
  bool mounted = store_.mountCard();
  out.printf("  remount: %s\n", mounted ? "ok" : "FAILED");
  setFault(F_CARD, !mounted);
  return pass;
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
void AppState::tick() {
  uint32_t now = millis();
  Log::tick();
  Console::tick(now);
  Storage::tick(now);
  now = millis();                                              // a deferred write (a save: ~600 ms) may have run just now

  kcx_.tick(now, cfg_.bluetoothSpeaker.enabled);
  if (kcxPowerOffAt_ && due(now, kcxPowerOffAt_)) { kcxPowerOffAt_ = 0; if (!cfg_.bluetoothSpeaker.enabled) kcx_.powerOff(); }
  tickBluetooth(now);
  if (ampOffAt_ && due(now, ampOffAt_)) { ampOffAt_ = 0; if (!audio_.playing()) ampEnable(false); }
  if (volPersistAt_ && due(now, volPersistAt_) && !audio_.playing()) { volPersistAt_ = 0; if (volume_.dirty()) Storage::deferredWrite(&AppState::volumeWriteThunk, this); }
  if (startupCuePending_ && railReady_) {                       // §6.5 / §17.2 J: the cue file is cached first; wait for it (3 s cap), else the built-in
    uint8_t st = cfg_.audio.cues.startup[0] ? cache_.stateOf(cfg_.audio.cues.startup) : (uint8_t)S_MISSING;
    if ((st != S_QUEUED && st != S_LOADING) || due(now, bi_.appStartMs + 3000)) {
      startupCuePending_ = false;
      LOG_I(TAG, "startup cue (%s) at +%lu ms", st == S_CACHED ? cfg_.audio.cues.startup : "built-in arpeggio", (unsigned long)now);
      playCue(cfg_.audio.cues.startup, sb::ToneKind::Startup, volume_.clickGain());
    }
  }
  tickPower(now);
  // §17.2 G: the BLE keyboard, from the first tick with no latched wake press (its init would delay a wake-and-play).
  if (kbdInitPending_ && (!touch_.wakePending() || due(now, bi_.appStartMs + 3000))) {
    kbdInitPending_ = false;
    if (!kbd_.begin(cfg_)) setFault(F_KBD, true);
  }
  if (kbd_.initOk()) {
    kbd_.tick(now, battery_.percent(), battery_.valid());
    if (kbd_.takeMemoryFull()) linkMessage("TABLET MEMORY FULL", 10000, now);   // §8.2
    bool ready = kbd_.link() == BleKeyboard::Link::Ready;
    if (ready != kbdWasReady_) {                                 // §11.4: the link change in plain words
      kbdWasReady_ = ready;
      if (ready) linkMessage("TABLET CONNECTED", 4000, now);
      else if (cfg_.keyboard.enabled) linkMessage("TABLET LOST", 4000, now);
    }
  }
  if (linkUntil_ && due(now, linkUntil_)) { linkUntil_ = 0; view_.link[0] = 0; display_.dirty(R_BOTTOM); }
  updateStateWord(now);
  // Inputs: pads every 15 ms, buttons every tick; the command engine runs once ACTIVE (§17.2 K).
  touch_.tick(now);
  buttons_.tick(now);
  if (mode_ == AppMode::Active || mode_ == AppMode::Dimmed || mode_ == AppMode::Menu) {
    sb::ButtonCmd c = cmds_.feed(now, buttons_.minusDown(), buttons_.plusDown());
    if (c != sb::ButtonCmd::None) { Event e = { Ev::ButtonCommand, now, {} }; e.command.id = (uint8_t)c; EventBus::post(e); }
    if (mode_ != AppMode::Menu) updateHoldBar(now);              // the menu screen draws its own bars (tickMenu)
  }
  if (mode_ == AppMode::Active || mode_ == AppMode::Dimmed) {
    if (touch_.wakePending() && touch_.wakeReadable() && railReady_) touch_.consumeWake(now);   // §4.1: the latched wake press, once the rail is ready
    press_.tick(now);                                            // §5.2 step 7 / §5.3 step 7: repeats while held
    sb::LevelChange back = levels_.tickReturnTimer(now);         // §5.3: return to level 1
    if (back.changed) { char why[40]; snprintf(why, sizeof why, "no input for %u s", (unsigned)cfg_.levelChange.returnToFirstAfterS); applyLevel(back, why, now); }
  }

  haptics_.tick(now); jacks_.tick(now);                        // §9, §10: segment and timeout bookkeeping in every mode
  now = millis();
  Event e;
  while (EventBus::poll(e)) handleEvent(e, now);

  switch (mode_) {
    case AppMode::Boot:
      if (recoveryCheckAt_ && due(now, recoveryCheckAt_)) {
        recoveryCheckAt_ = 0;
        if (Board::buttonMinusDown() && Board::buttonPlusDown()) {
          recoveryRequested_ = true;
          LOG_W(TAG, "recovery requested (both buttons held 3 s): SETUP starts with the normal view");
        } else LOG_I(TAG, "recovery prompt released: normal start");
      }
      if (touch_.calibrating() && !hintShown_ && recoveryCheckAt_ == 0) {   // §4.1 fresh calibration: the hint replaces the owner label
        bootScreen_.calibrationHint = true; bootScreen_.recoveryPrompt = false;
        display_.setScreen(&bootScreen_);
        hintShown_ = true;
      }
      if (configLoaded_ && recoveryCheckAt_ == 0 && due(now, bootUntil_) && !touch_.calibrating()) enterActive(now);
      break;
    case AppMode::Active:
      if (hintShown_ && !touch_.calibrating()) { hintShown_ = false; bootScreen_.calibrationHint = false; showHome(); }
      if (cfg_.display.dimAfterS && !hintShown_ && due(now, lastInputMs_ + (uint32_t)cfg_.display.dimAfterS * 1000UL)) enterDimmed(now);
      break;     // Phase 5: sleep/off
    case AppMode::Dimmed:
      if (hintShown_ && !touch_.calibrating()) { hintShown_ = false; bootScreen_.calibrationHint = false; showHome(); }
      break;
    case AppMode::Menu:                                          // §14: keys and bars are handled above and in the events; sleep and dim are blocked here
      tickMenu(now);
      break;
    case AppMode::Updating:                                      // §16: pads and commands ignored, the progress screen follows the job
      tickUpdating(now);
      break;
    case AppMode::Fault:
      break;
  }
  tickOverlays(now);
  tickSetup(now);                                              // §15: the portal's calls on the app task, its timers and card
  updater_.tickApp();                                          // §16: a running job always has its own task

  if (due(now, next1s_)) { next1s_ = now + 1000; tick1s(now); }
  display_.tick(now);
  if (levelChangeAt_ && !display_.pending()) { LOG_I(TAG, "level view redrawn %lu ms after the change", (unsigned long)(millis() - levelChangeAt_)); levelChangeAt_ = 0; }   // CP-4: <= 50 ms
}

void AppState::tick1s(uint32_t now) {
  Log::cardTick();                                             // §18 diag.logToCard: the ring's new lines to /log.txt when the audio side is idle
  if (msgUntil_ && due(now, msgUntil_)) { msgUntil_ = 0; view_.message[0] = 0; display_.dirty(R_NAME); }
  // §3.2 timeouts, from the last input: off-return after an Off-wake, else SLEEP.
  if ((mode_ == AppMode::Active || mode_ == AppMode::Dimmed) && sleepAllowed()) {
    uint32_t idle = now - lastInputMs_;
    if (wokeFromOff_ && !padSinceWake_ && cfg_.power.offReturnS && idle >= (uint32_t)cfg_.power.offReturnS * 1000UL) {
      char why[56]; snprintf(why, sizeof why, "no pad press for %u s after an Off-wake", (unsigned)cfg_.power.offReturnS); enterOff(why);
    } else if (cfg_.power.sleepAfterMin && idle >= (uint32_t)cfg_.power.sleepAfterMin * 60000UL) {
      char why[40]; snprintf(why, sizeof why, "no input for %u min", (unsigned)cfg_.power.sleepAfterMin); enterSleep(why);
    }
  }
  if (liveDeltas_) touch_.printLive(Serial);
  if (view_.usb != Board::usbPresent()) { view_.usb = Board::usbPresent(); display_.dirty(R_TOP); }

  healthCheck(now);                                            // §16
  // §18: five minutes of uptime clears the crash window.
  if (rtc::get().crashCount && now >= 5UL * 60UL * 1000UL) { rtc::clearCrashes(); LOG_I(TAG, "crash counter cleared after 5 min of uptime"); }
}

// ---------------------------------------------------------------------------
// Status (`s`)
// ---------------------------------------------------------------------------
static const char* otaStateName(const esp_partition_t* run) {
  esp_ota_img_states_t st;
  if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return "unknown";
  switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined (flashed over USB)";
    default: return "?";
  }
}

void AppState::printStatus(Print& out) {
  const sb::Level& L = cfg_.levels.levels[level_ < cfg_.levels.count ? level_ : 0];
  out.printf("state %s | level %u/%u \"%s\" | volume %u%%%s | config from %s rev %lu, %u warning(s)%s\n",
             modeName(), (unsigned)(level_ + 1), (unsigned)cfg_.levels.count, L.name, (unsigned)volume_.master(), volume_.muted() ? " (muted)" : "",
             store_.sourceName(), (unsigned long)cfg_.revision, (unsigned)rep_.count, store_.schemaTooNew() ? " [schema too new: saves refused]" : "");
  out.printf("card %s%s | flash mirror %s | faults:", store_.cardMounted() ? "mounted " : "not mounted", store_.cardMounted() ? store_.cardInfo() : "",
             store_.mirrorValid() ? "ok" : "none");
  if (!faults_) out.print(" none"); else for (uint8_t i = 0; i < N_FAULTS; i++) if (faults_ & FAULTS[i].bit) out.printf(" %s", FAULTS[i].token);
  out.println();
  battery_.printStatus(out);
  haptics_.printStatus(out, millis()); jacks_.printStatus(out);
  out.printf("power: sleep after %u min%s | off-return %s | links: KBD %s, BT %s\n", (unsigned)cfg_.power.sleepAfterMin, cfg_.power.sleepAfterMin ? "" : " (never)",
             wokeFromOff_ ? (padSinceWake_ ? "cleared by a pad press" : "armed (Off-wake, no pad press yet)") : "n/a (not an Off-wake)",
             kbd_.link() == BleKeyboard::Link::Ready ? "ready" : kbd_.link() == BleKeyboard::Link::Connecting ? "connecting" : kbd_.link() == BleKeyboard::Link::Advertising ? "advertising" : "off",
             cfg_.bluetoothSpeaker.enabled ? kcx_.linkName() : "off");
  out.printf("bluetooth speaker: %s | module %s%s | link %s | pairing %s%s | last line \"%s\" | %lu CONNECT line(s) | NO BT SPEAKER %s | rail cycle %s\n",
             cfg_.bluetoothSpeaker.enabled ? "enabled" : "disabled", kcx_.open() ? (kcx_.powerOnSeen() ? "alive" : "no banner") : "unpowered", kcx_.softOff() ? " (soft off)" : "",
             kcx_.linkName(), kcx_.pairName(), kcx_.pair() == KcxLink::Pair::Searching ? " (AT+PAIR sent)" : "", kcx_.lastLine(), (unsigned long)kcx_.connects(),
             btNotLinkedSince_ ? "armed" : "clear", btCyclePending_ ? "pending" : "none");
  kbd_.printStatus(out);
  audio_.printStatus(out);
  out.printf("cache: %u of %u cached, %lu KB of %lu KB, loader %s | speakers %s (amp %s) | bluetooth %s | scope pin IO%d\n",
             (unsigned)cache_.cachedCount(), (unsigned)cache_.count(), (unsigned long)(cache_.bytesUsed() / 1024), (unsigned long)(cache_.budget() / 1024),
             cache_.loaderRunning() ? "running" : "idle", cfg_.audio.outputs.speakers ? "ON" : "off", ampOn_ ? "on" : "off",
             cfg_.bluetoothSpeaker.enabled ? "enabled" : "disabled", (int)pins::SCOPE);
  const esp_partition_t* run = esp_ota_get_running_partition();
  out.printf("app task: stack %u B (min free %u B), priority %u | heap free %u B, PSRAM free %u B\n",
             (unsigned)(16 * 1024), (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)), (unsigned)uxTaskPriorityGet(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  out.printf("OTA: running %s, state %s | uptime %lu s | boots %lu | crashes %u | reset %d | last input %lu s ago\n",
             run ? run->label : "?", otaStateName(run), (unsigned long)(millis() / 1000), (unsigned long)rtc::get().bootCount,
             (unsigned)rtc::get().crashCount, (int)bi_.reset, (unsigned long)((millis() - lastInputMs_) / 1000));
  if (setupOn_) out.printf("setup: ON%s | \"%s\" password \"%s\" http://%s/ | %u phone(s) | %lu request(s), last change %lu s ago, idle off after %u min | net stack min free %lu B\n",
                           setupRecovery_ ? " (recovery)" : "", portal_.ssid(), cfg_.setup.password, portal_.ip(), (unsigned)portal_.clients(), (unsigned long)portal_.requests(),
                           (unsigned long)((millis() - portal_.lastInputRequestMs()) / 1000), (unsigned)cfg_.setup.idleOffMin, (unsigned long)portal_.netStackMin());
  else out.printf("setup: off | menu item or `w` starts it | password \"%s\" | idle off after %u min%s\n", cfg_.setup.password, (unsigned)cfg_.setup.idleOffMin, cfg_.setup.pauseKeyboard ? " | keyboard paused during setup" : "");
  updater_.printStatus(out);
  out.printf("menu: %s | %u items | hold both %u ms (%s) | timeout %u s%s\n", mode_ == AppMode::Menu ? "OPEN" : "closed", (unsigned)menu_.count(),
             (unsigned)cfg_.menu.holdMs, cfg_.menu.enabled ? "enabled" : "menu.enabled false", (unsigned)cfg_.menu.timeoutS,
             menu_.changed(cfg_) && mode_ == AppMode::Menu ? " | changes pending" : "");
  if (mode_ == AppMode::Menu) out.printf("  item %u/%u \"%s\" = \"%s\"%s%s | key %lu s ago\n", (unsigned)(menu_.index() + 1), (unsigned)menu_.count(), menuView_.label, menuView_.value,
                                         menuView_.result[0] ? " result " : "", menuView_.result, (unsigned long)((millis() - menuLastKeyAt_) / 1000));
  out.printf("buttons: - %s, + %s | commands %s | dim after %u s (%s) | last input %lu s ago\n",
             buttons_.minusDown() ? "DOWN" : "up", buttons_.plusDown() ? "DOWN" : "up", cmds_.enabled() ? "enabled" : "not yet",
             (unsigned)cfg_.display.dimAfterS, cfg_.display.dimAfterS ? "on" : "off", (unsigned long)((millis() - lastInputMs_) / 1000));
  out.printf("press: last [%u], repeat %s, %u repeat(s) so far | repeatWhileHeld %s, levelRepeatWhileHeld %s, delay %u ms | return to level 1: %s%lu s%s\n",
             (unsigned)press_.current(), press_.repeatArmed() ? "armed" : "idle", (unsigned)press_.repeats(),
             cfg_.press.repeatWhileHeld ? "on" : "off", cfg_.press.levelRepeatWhileHeld ? "on" : "off", (unsigned)cfg_.press.repeatDelayMs,
             cfg_.levelChange.returnToFirstAfterS ? (level_ ? "in " : "after ") : "never (", (unsigned long)(level_ ? (levels_.returnInMs(millis()) + 999) / 1000 : cfg_.levelChange.returnToFirstAfterS), cfg_.levelChange.returnToFirstAfterS ? "" : ")");
  touch_.printStatus(out);
  for (uint8_t i = 0; i < rep_.count; i++) out.printf("  config warning: %s\n", rep_.items[i].text);
}
