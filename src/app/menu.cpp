// The Quick Menu (FirmwareSpec.md §14): AppState's MENU mode. The item list,
// the values and the snapshot are the pure MenuModel (lib/sbcore); this file
// is the glue: the keys, live apply (§14.5), the action items, the screen,
// the single save on exit, Cancel and the timeout.
#include "app/state.h"
#include "app/faults.h"
#include "diag/log.h"
#include "util/timer.h"
#include "util/strutil.h"
#include "config/loader.h"
#include "power/rtc_state.h"
#include <Arduino.h>

static const char* TAG = "menu";

void AppState::openMenu(const char* why) {
  uint32_t now = millis();
  if (mode_ == AppMode::Menu) return;
  if (mode_ != AppMode::Active && mode_ != AppMode::Dimmed) { LOG_W(TAG, "not from %s", modeName()); return; }
  if (!cfg_.menu.enabled) { LOG_I(TAG, "menu.enabled is false: the menu command does nothing"); return; }
  if (menu_.count() == 0) menu_.build();
  mode_ = AppMode::Menu;
  cmds_.setMenuMode(true);                                     // §4.2 rule 5: the hold that opened it is consumed
  hold_ = sb::HoldProgress(); view_.holdKind = 0; view_.holdPct = 0; view_.holdOffReached = false;
  menu_.first();
  menu_.setSetupOn(setupOn_);
  menu_.open(cfg_, volume_.master());                          // the snapshot Cancel restores
  menuLastKeyAt_ = now; menuResultUntil_ = 0; menuPairTickAt_ = 0; menuCalibrating_ = false;
  menuView_ = MenuView();
  menuHold_ = sb::HoldProgress();
  display_.setBrightness(cfg_.display.brightnessPct);          // from DIMMED
  menuScreen_.view = &menuView_;
  refreshMenuItem();
  display_.setScreen(&menuScreen_);
  lastInputMs_ = now; levels_.onInput(now);
  LOG_I(TAG, "MENU open (%s): %u items, timeout %u s", why, (unsigned)menu_.count(), (unsigned)cfg_.menu.timeoutS);
}

void AppState::closeMenu(bool save, const char* why) {
  if (mode_ != AppMode::Menu) return;
  uint32_t now = millis();
  cmds_.setMenuMode(false);
  const bool changed = menu_.changed(cfg_);
  bool saved = false;
  if (!save) {                                                 // Cancel: every touched value back, live, through the same paths
    for (uint8_t i = 0; i < menu_.count(); i++) {
      const sb::MenuItem& it = menu_.at(i);
      if (it.kind != sb::MenuKind::Setting) continue;
      char cur[sb::MENU_TEXT];
      if (sb::config::getScalarText(cfg_, *it.desc, cur, sizeof cur) && strcmp(cur, menu_.snapshotText(i))) menuSetValue(it.desc->path, menu_.snapshotText(i), now);
    }
    if (volume_.master() != menu_.snapshotVolume()) { volume_.set(menu_.snapshotVolume()); volumeChanged(now, false); LOG_I(TAG, "volume back to %u%%", (unsigned)volume_.master()); }
  } else if (changed) {                                        // §14.1: one save, the saved cue (from the write), SAVED on the screen
    char err[100];
    saved = requestSave(true, err, sizeof err);
    if (!saved && strstr(err, "queued")) saved = true;         // a sound is playing: the write follows once it ends
    if (!saved) { LOG_W(TAG, "save failed: %s", err); message(err, 5000); }
  }
  mode_ = AppMode::Active;
  menuCalibrating_ = false;
  if (!hintShown_) showHome();                                 // a calibration hint still up: the Active tick restores the view after it
  display_.setBrightness(cfg_.display.brightnessPct);
  lastInputMs_ = now; levels_.onInput(now);                    // back to ACTIVE with the timers restarted
  refreshView(R_ALL);
  if (saved) { sb::copyStr(view_.overlay, sizeof view_.overlay, "SAVED"); view_.overlayBar = false; overlayUntil_ = now + 1000; if (!overlayUntil_) overlayUntil_ = 1; display_.dirty(R_MAIN); }   // §11.5
  LOG_I(TAG, "MENU closed (%s): %s", why, !save ? (changed ? "changes discarded" : "nothing had changed") : changed ? (saved ? "saved" : "save FAILED") : "nothing to save");
}

void AppState::refreshMenuItem() {
  menuView_.index = menu_.index(); menuView_.count = menu_.count();
  menu_.label(menuView_.label, sizeof menuView_.label);
  menu_.valueText(cfg_, volume_.master(), menuView_.value, sizeof menuView_.value);
  menuView_.action = menu_.isAction();
  char t[sb::MENU_TEXT]; uint8_t v;
  if (menu_.item().kind == sb::MenuKind::Volume) {
    menuView_.canUp = sb::MenuModel::stepVolume(volume_.master(), volume_.stepPct(), +1, v);
    menuView_.canDown = sb::MenuModel::stepVolume(volume_.master(), volume_.stepPct(), -1, v);
  } else {
    menuView_.canUp = menu_.stepText(cfg_, +1, t, sizeof t);
    menuView_.canDown = menu_.stepText(cfg_, -1, t, sizeof t);
  }
  sb::copyStr(menuView_.keys[0], 8, "BACK");                  // the word above each pad (§14.4 as laid out at CP-9)
  sb::copyStr(menuView_.keys[1], 8, menuView_.action ? "" : "DOWN");
  sb::copyStr(menuView_.keys[2], 8, menuView_.action ? menu_.actionVerb() : "UP");
  sb::copyStr(menuView_.keys[3], 8, "NEXT");
  display_.dirty(R_TOP | R_MAIN | R_BOTTOM);
}

void AppState::menuKey(MenuKey k, uint32_t now) {
  menuLastKeyAt_ = now; lastInputMs_ = now; levels_.onInput(now);
  if (menuResultUntil_) { menuResultUntil_ = 0; menuView_.result[0] = 0; }
  switch (k) {
    case MenuKey::Next: menu_.next(); menu_.disarm(); break;
    case MenuKey::Prev: menu_.prev(); menu_.disarm(); break;
    case MenuKey::Up:   if (menu_.isAction()) { menuRunAction(now); if (mode_ != AppMode::Menu) return; } else menuStep(+1, now); break;   // P2 / + runs an action
    case MenuKey::Down: menuStep(-1, now); break;
  }
  refreshMenuItem();
  LOG_I(TAG, "%s -> %u/%u \"%s\"%s%s", k == MenuKey::Next ? "next" : k == MenuKey::Prev ? "back" : k == MenuKey::Up ? "up" : "down",
        (unsigned)(menu_.index() + 1), (unsigned)menu_.count(), menuView_.label, menuView_.value[0] ? " = " : "", menuView_.value);
}

void AppState::menuStep(int dir, uint32_t now) {
  const sb::MenuItem& it = menu_.item();
  if (it.kind == sb::MenuKind::Volume) {
    uint8_t v;
    if (!sb::MenuModel::stepVolume(volume_.master(), volume_.stepPct(), dir, v)) return;
    setVolumeLive(v, now);
  } else if (it.kind == sb::MenuKind::Setting) {
    char t[sb::MENU_TEXT];
    if (!menu_.stepText(cfg_, dir, t, sizeof t)) { LOG_D(TAG, "%s: at the end of its range", it.desc->path); return; }
    menuSetValue(it.desc->path, t, now);
  }
}

void AppState::menuSetValue(const char* path, const char* text, uint32_t now) {   // §14.5: live, through the same paths as the console
  bool on = sb::eqNoCase(text, "true");
  if (!strcmp(path, "bluetoothSpeaker.enabled")) { setBluetooth(on); return; }
  if (!strcmp(path, "audio.outputs.speakers")) { setSpeakers(on); return; }
  char err[100];
  if (!setSetting(path, text, err, sizeof err)) { LOG_W(TAG, "%s = %s rejected: %s", path, text, err); return; }
  LOG_I(TAG, "%s = %s (live; written on exit)", path, text);
  if (!strcmp(path, "vibration.enabled") && on) haptics_.singlePulse(now);   // felt at once
}

void AppState::setVolumeLive(uint8_t pct, uint32_t now) {
  volume_.set(pct);                                            // un-mutes, like every volume change (§6.2)
  volumeChanged(now, false);
  LOG_I(TAG, "volume %u%% (menu)", (unsigned)pct);
  playCue(cfg_.audio.cues.click, sb::ToneKind::Click, volume_.volumeActionClickGain());   // heard at the new level
}

void AppState::menuResult(const char* text, uint32_t ms, uint32_t now) {
  sb::copyStr(menuView_.result, sizeof menuView_.result, text);
  menuResultUntil_ = ms ? now + ms : 0;
  if (ms && !menuResultUntil_) menuResultUntil_ = 1;
  display_.dirty(R_MAIN);
  LOG_I(TAG, "%s: %s", menuView_.label, text);
}

void AppState::menuRunAction(uint32_t now) {                   // the "up" press on an action item
  menuLastKeyAt_ = now; lastInputMs_ = now; levels_.onInput(now);
  if (menu_.confirmNeeded() && !menu_.armed()) { menu_.arm(); menuResult("PRESS AGAIN", 5000, now); return; }   // Wi-Fi setup: twice
  menu_.disarm();
  switch (menu_.item().kind) {
    case sb::MenuKind::PairSpeaker:                            // §7.3; after a failure the next press forgets every speaker first
      if (!cfg_.bluetoothSpeaker.enabled) { menuResult("BT SPEAKER IS OFF", 3000, now); break; }
      if (startPairing(menu_.pairWipeOffered())) { menuResult("SEARCHING", 0, now); menuPairTickAt_ = 0; }
      else menuResult("NOT STARTED", 3000, now);
      break;
    case sb::MenuKind::Recalibrate:                            // §4.1: the fresh calibration with the hint
      recalibrate();
      menuCalibrating_ = touch_.calibrating();
      if (!menuCalibrating_) menuResult("NOT POSSIBLE", 3000, now);
      break;
    case sb::MenuKind::WifiSetup:                              // §15.1: START twice starts it and the menu closes; STOP once stops it
      if (setupOn_) { stopSetup("menu item"); menuResult("STOPPED", 2000, now); break; }
      if (startSetup("menu item", false)) { closeMenu(true, "Wi-Fi setup started"); return; }
      menuResult("NOT STARTED", 3000, now);
      break;
    case sb::MenuKind::Exit:   closeMenu(true, "Exit item"); return;
    case sb::MenuKind::Cancel: closeMenu(false, "Cancel item"); return;
    default: break;
  }
  display_.dirty(R_MAIN);
}

void AppState::menuPairResult(bool connected, uint32_t now) {   // from tickBluetooth while the menu is open
  menu_.setPairWipeOffered(!connected);
  if (menu_.item().kind != sb::MenuKind::PairSpeaker) return;
  menuResult(connected ? "CONNECTED" : "NO SPEAKER FOUND", connected ? 3000 : 6000, now);
  menuLastKeyAt_ = now;
  refreshMenuItem();                                           // the hint after a failure
}

void AppState::tickMenu(uint32_t now) {
  // The exit bar from the command engine: both buttons toward offHoldMs, then "release".
  sb::HoldProgress p = cmds_.progress(now);
  p.pct = (uint8_t)(p.pct / 4 * 4);
  if (p != menuHold_) {
    menuHold_ = p;
    bool hb = p.kind == sb::HoldKind::Both;
    if (hb != menuView_.holdBar || (hb && (p.pct != menuView_.holdPct || p.offReached != menuView_.holdOffReached))) {
      menuView_.holdBar = hb; menuView_.holdPct = p.pct; menuView_.holdOffReached = p.offReached;
      display_.dirty(R_BOTTOM);
    }
  }
  // A recalibration (the item, or console `c`): the hint screen while it runs, then back to the menu.
  if (hintShown_ && !touch_.calibrating()) {
    hintShown_ = false; bootScreen_.calibrationHint = false;
    display_.setScreen(&menuScreen_);
    if (menuCalibrating_) { menuCalibrating_ = false; menuResult("DONE", 2000, now); }
    menuLastKeyAt_ = now;
  }
  // The pairing countdown on its item (§7.3 step 1); no timeout while it searches.
  if (menu_.item().kind == sb::MenuKind::PairSpeaker) {
    KcxLink::Pair ps = kcx_.pair();
    if ((ps == KcxLink::Pair::Wiping || ps == KcxLink::Pair::Searching) && due(now, menuPairTickAt_)) {
      menuPairTickAt_ = now + 1000;
      snprintf(menuView_.result, sizeof menuView_.result, "SEARCHING %lu", (unsigned long)((kcx_.pairLeftMs(now) + 999) / 1000));
      menuResultUntil_ = 0; display_.dirty(R_MAIN);
      menuLastKeyAt_ = now;
    }
  }
  if (menuResultUntil_ && due(now, menuResultUntil_)) { menuResultUntil_ = 0; menuView_.result[0] = 0; display_.dirty(R_MAIN); }
  if (!hintShown_ && due(now, menuLastKeyAt_ + (uint32_t)cfg_.menu.timeoutS * 1000UL)) closeMenu(true, "timeout");   // §14.1
}
