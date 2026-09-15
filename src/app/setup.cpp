// SETUP, the Wi-Fi settings portal as the app sees it (FirmwareSpec.md §15):
// entry from the menu, the console or a recovery boot; exit from the page, the
// menu, the idle timeout or any power-down; the setup card and the SETUP ON /
// OFF words (§15.2); and the portal's work that touches module state, run on
// the app task for the handlers of net/api.cpp (§19.2). The radio and the
// servers are net/portal.cpp.
#include "app/state.h"
#include "app/faults.h"
#include "diag/log.h"
#include "util/timer.h"
#include "util/strutil.h"
#include "config/loader.h"
#include "config/settings_table.h"
#include "config/levels.h"
#include "hal/board.h"
#include "hal/storage.h"
#include "power/rtc_state.h"
#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char* TAG = "setup";
static constexpr uint32_t CARD_AGAIN_MS = 10000;   // §15.2: the card returns after 10 s without a pad or button

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
    case ESP_RST_USB:       return "USB";
    default:                return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Entry, exit, the card
// ---------------------------------------------------------------------------
bool AppState::startSetup(const char* why, bool recovery) {
  if (setupOn_) return true;
  if (mode_ != AppMode::Active && mode_ != AppMode::Dimmed && mode_ != AppMode::Menu) { LOG_W(TAG, "not from %s", modeName()); return false; }
  uint32_t now = millis();
  if (!portal_.start(*this, cfg_.setup.password, recovery)) {
    setFault(F_WIFI, true);
    message("Wi-Fi did not start", 5000);
    return false;
  }
  setFault(F_WIFI, false);
  setupOn_ = true; setupRecovery_ = recovery;
  menu_.setSetupOn(true);
  if (cfg_.setup.pauseKeyboard) kbd_.setPaused(true);           // §8.5
  linkMessage("SETUP ON", 4000, now);
  setupCardShown_ = false; setupCardAt_ = now; setupTickAt_ = now;
  if (mode_ != AppMode::Menu) showSetupCard(now);
  LOG_I(TAG, "SETUP on (%s)%s: idle timeout %u min", why, recovery ? " in recovery" : "", (unsigned)cfg_.setup.idleOffMin);
  return true;
}

void AppState::stopSetup(const char* why) {
  if (!setupOn_) return;
  uint32_t now = millis();
  portal_.stop();
  setupOn_ = false; setupRecovery_ = false;
  menu_.setSetupOn(false);
  kbd_.setPaused(false);
  setupCardShown_ = false;
  if (mode_ == AppMode::Active || mode_ == AppMode::Dimmed) { if (!hintShown_) display_.setScreen(&normalScreen_); linkMessage("SETUP OFF", 4000, now); }
  else if (mode_ == AppMode::Menu) refreshMenuItem();
  LOG_I(TAG, "SETUP off (%s)", why);
}

void AppState::showSetupCard(uint32_t now) {
  sb::copyStr(setupView_.ssid, sizeof setupView_.ssid, portal_.ssid());
  sb::copyStr(setupView_.password, sizeof setupView_.password, cfg_.setup.password);
  snprintf(setupView_.url, sizeof setupView_.url, "http://%s", portal_.ip());
  setupView_.clients = portal_.clients();
  setupView_.recovery = setupRecovery_;
  setupScreen_.view = &setupView_;
  setupCardShown_ = true;
  display_.setScreen(&setupScreen_);
  (void)now;
}

void AppState::hideSetupCard(uint32_t now) {
  if (!setupCardShown_) return;
  setupCardShown_ = false;
  setupCardAt_ = now + CARD_AGAIN_MS; if (!setupCardAt_) setupCardAt_ = 1;
  if ((mode_ == AppMode::Active || mode_ == AppMode::Dimmed) && !hintShown_) display_.setScreen(&normalScreen_);
}

void AppState::showHome() {
  if (setupOn_ && setupCardShown_) { setupScreen_.view = &setupView_; display_.setScreen(&setupScreen_); }
  else display_.setScreen(&normalScreen_);
}

void AppState::tickSetup(uint32_t now) {
  portal_.tickApp();                                             // a handler waiting on the app task, in every mode
  if (!setupOn_) return;
  if (portal_.stopRequested()) { char why[48]; snprintf(why, sizeof why, "page: %s", portal_.stopReason()); stopSetup(why); return; }
  if (cfg_.setup.idleOffMin && !updater_.busy() && due(now, portal_.lastInputRequestMs() + (uint32_t)cfg_.setup.idleOffMin * 60000UL)) {   // §15.1; not while an update runs
    char why[40]; snprintf(why, sizeof why, "%u min without a change", (unsigned)cfg_.setup.idleOffMin);
    stopSetup(why);
    return;
  }
  if ((mode_ == AppMode::Active || mode_ == AppMode::Dimmed) && !hintShown_) {
    if (!setupCardShown_ && due(now, setupCardAt_)) showSetupCard(now);
    else if (setupCardShown_ && due(now, setupTickAt_)) {         // the phone count, once a second
      setupTickAt_ = now + 1000;
      uint8_t c = portal_.clients();
      if (c != setupView_.clients) { setupView_.clients = c; display_.dirty(R_ALL); }
    }
  }
}

// ---------------------------------------------------------------------------
// The portal's reads (app task)
// ---------------------------------------------------------------------------
static const char* pairName(KcxLink::Pair p) {
  switch (p) { case KcxLink::Pair::Wiping: return "wiping"; case KcxLink::Pair::Searching: return "searching"; case KcxLink::Pair::Connected: return "connected"; case KcxLink::Pair::Timeout: return "timeout"; default: return "none"; }
}

void AppState::portalStatus(Print& out) {
  JsonDocument d(psramAllocator());
  uint32_t now = millis();
  d["mode"] = modeName();
  d["version"] = FW_VERSION;
  d["uptimeS"] = now / 1000;
  d["reset"] = resetName(bi_.reset);
  d["name"] = cfg_.device.name;
  JsonObject b = d["battery"].to<JsonObject>();
  b["pct"] = battery_.percent(); b["mv"] = battery_.millivolts(); b["usb"] = Board::usbPresent(); b["valid"] = battery_.valid(); b["full"] = battery_.full();
  JsonObject l = d["level"].to<JsonObject>();
  l["index"] = level_ + 1; l["count"] = cfg_.levels.count; l["name"] = cfg_.levels.levels[level_ < cfg_.levels.count ? level_ : 0].name;
  JsonObject v = d["volume"].to<JsonObject>();
  v["pct"] = volume_.master(); v["muted"] = volume_.muted();
  JsonObject c = d["card"].to<JsonObject>();
  c["mounted"] = store_.cardMounted(); c["info"] = store_.cardMounted() ? store_.cardInfo() : ""; c["files"] = cache_.count();
  c["source"] = store_.sourceName(); c["revision"] = cfg_.revision; c["lastSaveOk"] = lastSaveOk_; c["cardLess"] = store_.lastSaveCardLess();
  c["schemaTooNew"] = store_.schemaTooNew(); c["parseError"] = store_.lastParseError();
  JsonObject bt = d["bt"].to<JsonObject>();
  bt["enabled"] = cfg_.bluetoothSpeaker.enabled; bt["linked"] = kcx_.linked(); bt["peer"] = kcx_.peerName();
  bt["module"] = kcx_.open() ? (kcx_.softOff() ? "soft off" : kcx_.alive() ? "alive" : "silent") : "unpowered";
  bt["pairing"] = pairName(kcx_.pair()); bt["pairLeftS"] = (kcx_.pairLeftMs(now) + 999) / 1000;
  JsonObject k = d["keyboard"].to<JsonObject>();
  k["enabled"] = cfg_.keyboard.enabled; k["paused"] = setupOn_ && cfg_.setup.pauseKeyboard;
  k["link"] = kbd_.link() == BleKeyboard::Link::Ready ? "ready" : kbd_.link() == BleKeyboard::Link::Connecting ? "connecting" : kbd_.link() == BleKeyboard::Link::Advertising ? "advertising" : "off";
  k["bonds"] = kbd_.bonds(); k["initOk"] = kbd_.initOk();
  d["speakers"] = cfg_.audio.outputs.speakers;
  d["playing"] = audio_.playing();
  JsonArray f = d["faults"].to<JsonArray>();
  for (uint8_t i = 0; i < N_FAULTS; i++) if (faults_ & FAULTS[i].bit) { JsonObject o = f.add<JsonObject>(); o["token"] = FAULTS[i].token; o["text"] = FAULTS[i].message; }
  JsonArray w = d["warnings"].to<JsonArray>();
  for (uint8_t i = 0; i < rep_.count; i++) w.add(rep_.items[i].text);
  JsonObject s = d["setup"].to<JsonObject>();
  s["clients"] = portal_.clients(); s["recovery"] = setupRecovery_; s["ssid"] = portal_.ssid(); s["ip"] = portal_.ip();
  s["idleS"] = (now - portal_.lastInputRequestMs()) / 1000; s["idleOffMin"] = cfg_.setup.idleOffMin; s["requests"] = portal_.requests();
  s["sinceS"] = (now - portal_.startedAt()) / 1000;
  JsonObject p = d["pad"].to<JsonObject>();
  p["seq"] = identSeq_; p["ch"] = identCh_; p["pos"] = identPos_;
  d["heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  d["psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  d["calibrating"] = touch_.calibrating();
  serializeJson(d, out);
}

void AppState::portalDiag(Print& out) {
  JsonDocument d(psramAllocator());
  uint32_t now = millis();
  JsonArray pads = d["pads"].to<JsonArray>();
  for (uint8_t pos = 0; pos < 4; pos++) {
    const TouchInput::Pad& p = touch_.pad(pos);
    JsonObject o = pads.add<JsonObject>();
    o["pos"] = pos + 1; o["ch"] = p.ch >= 0 ? p.ch + 2 : 0; o["delta"] = serialized(String(p.delta, 2)); o["raw"] = p.raw;
    o["baseline"] = p.ch >= 0 ? touch_.baseline((uint8_t)p.ch) : 0; o["pressed"] = p.pressed; o["stuck"] = p.stuck;
    o["role"] = sb::config::roleName(p.role);
  }
  d["touch"] = touch_.failed() ? "failed" : touch_.calibrating() ? "calibrating" : touch_.ready() ? "ready" : "checking";
  JsonObject c = d["cache"].to<JsonObject>();
  c["cached"] = cache_.cachedCount(); c["count"] = cache_.count(); c["usedKB"] = cache_.bytesUsed() / 1024; c["budgetKB"] = cache_.budget() / 1024; c["loader"] = cache_.loaderRunning();
  JsonObject a = d["audio"].to<JsonObject>();
  a["rail"] = audio_.railName(); a["ready"] = audio_.ready(); a["playing"] = audio_.playing(); a["underruns"] = audio_.underruns(); a["stalls"] = audio_.silentStalls(); a["starved"] = audio_.starved();
  JsonObject kx = d["kcx"].to<JsonObject>();
  kx["last"] = kcx_.lastLine(); kx["module"] = kcx_.open() ? (kcx_.softOff() ? "soft off" : kcx_.alive() ? "alive" : "silent") : "unpowered"; kx["link"] = kcx_.linkName(); kx["connects"] = kcx_.connects();
  JsonObject b = d["battery"].to<JsonObject>();
  b["method"] = battery_.methodName(); b["k"] = serialized(String(battery_.k(), 3)); b["tcal"] = serialized(String(battery_.tcal(), 1));
  b["mv"] = battery_.millivolts(); b["pct"] = battery_.percent(); b["valid"] = battery_.valid(); b["usb"] = Board::usbPresent();
  d["reset"] = resetName(bi_.reset); d["bootCount"] = rtc::get().bootCount; d["crashes"] = rtc::get().crashCount;
  d["heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); d["heapMin"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  d["psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  d["appStackMin"] = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t); d["netStackMin"] = portal_.netStackMin();
  d["uptimeS"] = now / 1000;
  JsonObject p = d["pad"].to<JsonObject>();
  p["seq"] = identSeq_; p["ch"] = identCh_; p["pos"] = identPos_;
  JsonArray ch = d["padChannels"].to<JsonArray>();
  for (uint8_t i = 0; i < 4; i++) ch.add(cfg_.hardware.padChannels[i]);
  serializeJson(d, out);
}

void AppState::portalSounds(Print& out) {
  JsonDocument d(psramAllocator());
  d["cardMounted"] = store_.cardMounted();
  d["count"] = cache_.count();
  d["loader"] = cache_.loaderRunning();
  JsonArray arr = d["sounds"].to<JsonArray>();
  SoundEntry e;
  for (uint16_t i = 0; i < cache_.count(); i++) {
    if (!cache_.entry(i, e)) continue;
    JsonObject o = arr.add<JsonObject>();
    o["name"] = e.name; o["bytes"] = e.fileBytes; o["rate"] = e.rate; o["channels"] = e.channels;
    o["seconds"] = serialized(String(e.frames / 44100.0f, 2)); o["state"] = cache_.stateName(e.state); o["reason"] = e.reason;
    JsonArray used = o["usedBy"].to<JsonArray>();
    for (uint8_t l = 0; l < cfg_.levels.count; l++) {
      uint8_t si = 0;
      for (uint8_t pos = 0; pos < 4; pos++) {
        if (cfg_.pads.roles[pos] != sb::Role::Sound) continue;
        const sb::Entry& en = cfg_.levels.levels[l].buttons[si];
        if (en.sound[0] && sb::eqNoCase(en.sound, e.name)) { char t[16]; snprintf(t, sizeof t, "L%u P%u", (unsigned)(l + 1), (unsigned)(pos + 1)); used.add(t); }
        si++;
      }
    }
    if (sb::eqNoCase(cfg_.audio.cues.startup, e.name)) used.add("cue startup");
    if (sb::eqNoCase(cfg_.audio.cues.click, e.name)) used.add("cue click");
    if (sb::eqNoCase(cfg_.audio.cues.saved, e.name)) used.add("cue saved");
    if (sb::eqNoCase(cfg_.audio.cues.lowBattery, e.name)) used.add("cue low battery");
  }
  serializeJson(d, out);
}

void AppState::portalConfig(Print& out, bool support) { store_.exportTo(cfg_, out, true, support); }

void AppState::portalReport(Print& out) {
  out.print('[');
  for (uint8_t i = 0; i < rep_.count; i++) {
    if (i) out.print(',');
    out.print('"');
    for (const char* p = rep_.items[i].text; *p; p++) { if (*p == '"' || *p == '\\') out.print('\\'); if ((uint8_t)*p < 0x20) out.print(' '); else out.print(*p); }
    out.print('"');
  }
  out.print(']');
}

// ---------------------------------------------------------------------------
// The portal's writes and actions (app task)
// ---------------------------------------------------------------------------
bool AppState::portalApply(const char* json, bool replace, bool keepHardware, char* err, size_t errLen) {
  if (!store_.applyText(json, replace, keepHardware, cfg_, rep_, err, errLen)) return false;
  onConfigChanged();
  registerInput(millis());                                       // §3.1: a change from the page is input
  char serr[100];
  bool saved = requestSave(true, serr, sizeof serr);
  if (!saved && strstr(serr, "queued")) saved = true;            // a sound is playing: the write follows once it ends
  if (!saved) { snprintf(err, errLen, "applied, but the save failed: %s", serr); LOG_W(TAG, "%s", err); return false; }
  LOG_I(TAG, "configuration %s from the page: revision %lu, %u warning(s)", replace ? "replaced" : "updated", (unsigned long)cfg_.revision, (unsigned)rep_.count);
  return true;
}

bool AppState::portalPlay(const PortalArgs& a, char* err, size_t errLen) {
  uint32_t now = millis();
  registerInput(now);
  if (mode_ == AppMode::Menu) { snprintf(err, errLen, "the Quick Menu is open on the board"); return false; }
  if (!audio_.ready()) { snprintf(err, errLen, "audio rail not ready"); return false; }
  if (a.value[0]) {                                              // "name": a library file
    if (!sb::validSoundName(a.value)) { snprintf(err, errLen, "bad sound name"); return false; }
    playSound(a.value, 100, touch_.nextPressId(), now, false);
    LOG_I(TAG, "page: play %s", a.value);
    return true;
  }
  if (a.level > 0) {
    if (a.level > cfg_.levels.count || a.button < 0 || a.button >= sb::nSoundPads(cfg_)) { snprintf(err, errLen, "no such level or pad"); return false; }
    const sb::Entry& e = cfg_.levels.levels[a.level - 1].buttons[a.button];
    if (!e.sound[0]) { snprintf(err, errLen, "that pad has no sound"); return false; }
    playSound(e.sound, e.volumePct, touch_.nextPressId(), now, false);
    LOG_I(TAG, "page: play level %d pad %d (%s)", a.level, a.button + 1, e.sound);
    return true;
  }
  if (a.name[0]) {                                               // "cue"
    const char* file = nullptr; sb::ToneKind kind = sb::ToneKind::Click;
    if (!strcmp(a.name, "startup")) { file = cfg_.audio.cues.startup; kind = sb::ToneKind::Startup; }
    else if (!strcmp(a.name, "click")) { file = cfg_.audio.cues.click; kind = sb::ToneKind::Click; }
    else if (!strcmp(a.name, "saved")) { file = cfg_.audio.cues.saved; kind = sb::ToneKind::Saved; }
    else if (!strcmp(a.name, "lowBattery")) { file = cfg_.audio.cues.lowBattery; kind = sb::ToneKind::Fault; }
    else if (!strcmp(a.name, "missing")) { file = ""; kind = sb::ToneKind::MissingSound; }
    else { snprintf(err, errLen, "unknown cue"); return false; }
    if (!strcmp(a.name, "lowBattery") && !file[0]) { snprintf(err, errLen, "no low-battery cue file is set"); return false; }
    playCue(file, kind, volume_.clickGain());
    LOG_I(TAG, "page: cue %s (%s)", a.name, file[0] ? file : "built-in");
    return true;
  }
  snprintf(err, errLen, "nothing to play");
  return false;
}

bool AppState::portalAction(const PortalArgs& a, Print& out, char* err, size_t errLen) {
  uint32_t now = millis();
  const char* n = a.name;
  if (strcmp(n, "wifiOff") && strcmp(n, "preview")) registerInput(now);   // §3.1: an action is input
  if (!strcmp(n, "buzz")) {
    if (a.pattern[0]) {
      uint16_t p[sb::MAX_PATTERN]; uint8_t k = 0;
      const char* s = a.pattern;
      while (*s && k < sb::MAX_PATTERN) { while (*s == ' ' || *s == ',') s++; if (!*s) break; long v = strtol(s, (char**)&s, 10); if (v < 0) v = 0; if (v > 10000) v = 10000; p[k++] = (uint16_t)v; }
      if (!k) { snprintf(err, errLen, "empty pattern"); return false; }
      haptics_.playPattern(p, k, now);
    } else {
      uint8_t l = a.level > 0 && a.level <= cfg_.levels.count ? (uint8_t)(a.level - 1) : level_;
      haptics_.buzzTest(cfg_, l, now);
    }
    return true;
  }
  if (!strcmp(n, "pairSpeaker") || !strcmp(n, "forgetSpeakers")) {
    if (!cfg_.bluetoothSpeaker.enabled) { snprintf(err, errLen, "enable the Bluetooth speaker first"); return false; }
    if (!startPairing(!strcmp(n, "forgetSpeakers"))) {
      KcxLink::Pair ps = kcx_.pair();
      if (ps == KcxLink::Pair::Wiping || ps == KcxLink::Pair::Searching) snprintf(err, errLen, "already searching");
      else if (!railReady_ || !kcx_.alive()) snprintf(err, errLen, "the speaker module is still starting: try again in a few seconds");
      else snprintf(err, errLen, "pairing did not start");
      return false;
    }
    return true;
  }
  if (!strcmp(n, "forgetHosts")) { kbd_.forget(); return true; }
  if (!strcmp(n, "recalibrate")) {
    if (touch_.failed()) { snprintf(err, errLen, "touch driver is down"); return false; }
    recalibrate();
    return true;
  }
  if (!strcmp(n, "setLevel")) {
    if (a.level < 1 || a.level > cfg_.levels.count) { snprintf(err, errLen, "no such level"); return false; }
    if (mode_ == AppMode::Menu) { snprintf(err, errLen, "the Quick Menu is open on the board"); return false; }
    applyLevel(levels_.set((uint8_t)(a.level - 1), sb::LevelSource::Portal), "page", now);
    return true;
  }
  if (!strcmp(n, "closeJack")) {
    if (a.jack < 1 || a.jack > 4) { snprintf(err, errLen, "jack 1-4"); return false; }
    jacks_.closeTest((uint8_t)(a.jack - 1), 1000, now);
    return true;
  }
  if (!strcmp(n, "setBatteryK")) {
    char msg[120];
    bool ok = a.volts > 0 ? battery_.calibrate(a.volts, msg, sizeof msg) : battery_.clearCalibration();
    if (a.volts <= 0) snprintf(msg, sizeof msg, ok ? "battery method: design divider" : "NVS write failed");
    if (!ok) { snprintf(err, errLen, "%s", msg); return false; }
    out.printf(",\"message\":\"%s\"", msg);
    return true;
  }
  if (!strcmp(n, "resetSettings")) {
    if (!store_.resetScalars(cfg_, rep_, err, errLen)) return false;
    onConfigChanged();
    char serr[100];
    bool saved = requestSave(true, serr, sizeof serr);
    if (!saved && strstr(serr, "queued")) saved = true;
    if (!saved) { snprintf(err, errLen, "reset, but the save failed: %s", serr); return false; }
    LOG_W(TAG, "page: every scalar setting back to its default (levels, roles, hardware, owner label, cues and wifi kept)");
    return true;
  }
  if (!strcmp(n, "wifiOff")) { portal_.requestStop("Turn off setup"); return true; }
  if (!strcmp(n, "preview")) {                                   // display.* live without a save; Discard sends the old value back
    if (strncmp(a.path, "display.", 8)) { snprintf(err, errLen, "preview is for display settings"); return false; }
    if (!setSetting(a.path, a.value, err, errLen)) return false;
    return true;
  }
  if (!strcmp(n, "mute")) { toggleMute(now); return true; }
  snprintf(err, errLen, "unknown action \"%s\"", n);
  return false;
}

// A sound file is deleted or renamed: every entry and cue that names it follows, in one save (§15.4).
struct RefEdit { const char* from; const char* to; int changed; };
static bool editRefs(JsonObject root, void* ctx) {
  RefEdit* e = static_cast<RefEdit*>(ctx);
  auto fix = [&](JsonVariant v) {
    const char* s = v.as<const char*>();
    if (s && sb::eqNoCase(s, e->from)) { v.set(sb::config::copied(e->to)); e->changed++; }
  };
  JsonArray levels = root["levels"].as<JsonArray>();
  for (JsonObject l : levels) { JsonArray b = l["buttons"].as<JsonArray>(); for (JsonObject en : b) if (!en["sound"].isNull()) fix(en["sound"]); }
  JsonObject cues = root["audio"]["cues"].as<JsonObject>();
  if (!cues.isNull()) for (JsonPair kv : cues) fix(kv.value());
  return true;
}

bool AppState::portalSoundDelete(const char* name, char* err, size_t errLen) {
  if (!sb::validSoundName(name) || !name[0]) { snprintf(err, errLen, "bad sound name"); return false; }
  registerInput(millis());
  RefEdit e = { name, "", 0 };
  if (!store_.editDocument(&editRefs, &e, cfg_, rep_, err, errLen)) return false;
  if (e.changed) onConfigChanged();
  char path[64]; snprintf(path, sizeof path, "/sounds/%s", name);
  bool removed;
  { Storage::Guard g; removed = SD.remove(path); }
  if (!removed) { snprintf(err, errLen, "could not delete %s", name); LOG_W(TAG, "%s", err); }
  if (e.changed) { char serr[100]; if (!requestSave(true, serr, sizeof serr) && !strstr(serr, "queued")) LOG_W(TAG, "save after delete: %s", serr); }
  rescanSounds();
  LOG_I(TAG, "page: deleted %s (%d reference(s) blanked)", name, e.changed);
  return removed;
}

bool AppState::portalSoundRename(const char* name, const char* to, char* err, size_t errLen) {
  if (!sb::validSoundName(name) || !name[0] || !sb::validSoundName(to) || !to[0]) { snprintf(err, errLen, "bad sound name"); return false; }
  registerInput(millis());
  char from[64], dest[64]; snprintf(from, sizeof from, "/sounds/%s", name); snprintf(dest, sizeof dest, "/sounds/%s", to);
  {
    Storage::Guard g;
    if (!SD.exists(from)) { snprintf(err, errLen, "no such file"); return false; }
    if (!sb::eqNoCase(name, to) && SD.exists(dest)) { snprintf(err, errLen, "a file called %s exists", to); return false; }
    if (!SD.rename(from, dest)) { snprintf(err, errLen, "rename failed"); return false; }
  }
  RefEdit e = { name, to, 0 };
  if (!store_.editDocument(&editRefs, &e, cfg_, rep_, err, errLen)) { LOG_W(TAG, "rename: references not updated: %s", err); }
  else if (e.changed) { onConfigChanged(); char serr[100]; if (!requestSave(true, serr, sizeof serr) && !strstr(serr, "queued")) LOG_W(TAG, "save after rename: %s", serr); }
  rescanSounds();
  LOG_I(TAG, "page: renamed %s -> %s (%d reference(s) updated)", name, to, e.changed);
  return true;
}

void AppState::rescanSounds() {
  refHash_ = SoundCache::referenceHash(cfg_);
  if (audioStarted_) cache_.reload(cfg_, level_);
}
