// The `levels` array (FirmwareSpec.md §5.1, §13.4): a level with the wrong
// number of buttons or an unknown action is dropped with a warning, never the
// whole file; zero valid levels -> one blank level; goToLevel out of range -> 1.
#include "config/levels.h"
#include "ble/keys.h"
#include "config/loader.h"
#include "util/strutil.h"
#include <string.h>

using namespace ArduinoJson;

namespace sb {
namespace config {

uint8_t loadPatternArray(JsonVariantConst v, uint16_t* out, const char* what, ConfigReport& rep);   // loader.cpp

static const char* const ACTION_NAMES[]  = { "none", "volumeUp", "volumeDown", "mute", "nextLevel", "previousLevel", "goToLevel" };
static const char* const KEYMODE_NAMES[] = { "type", "hold", "tap" };
static const char* const ROLE_NAMES[]    = { "sound", "level", "none" };
static const char* const JACK_NAMES[]    = { "follow", "pulse", "off" };   // JackMode::Follow..Off

const char* actionName(Action a)   { return ACTION_NAMES[(uint8_t)a < 7 ? (uint8_t)a : 0]; }
const char* keyModeName(KeyMode m) { return KEYMODE_NAMES[(uint8_t)m < 3 ? (uint8_t)m : 0]; }
const char* roleName(Role r)       { return ROLE_NAMES[(uint8_t)r < 3 ? (uint8_t)r : 0]; }
const char* jackName(JackMode j)   { return j == JackMode::Inherit ? nullptr : JACK_NAMES[(uint8_t)j - 1]; }

bool actionFromName(const char* s, Action& out) {
  for (uint8_t i = 0; i < 7; i++) if (eqNoCase(s, ACTION_NAMES[i])) { out = (Action)i; return true; }
  return false;
}
bool keyModeFromName(const char* s, KeyMode& out) {
  for (uint8_t i = 0; i < 3; i++) if (eqNoCase(s, KEYMODE_NAMES[i])) { out = (KeyMode)i; return true; }
  return false;
}
bool roleFromName(const char* s, Role& out) {
  for (uint8_t i = 0; i < 3; i++) if (eqNoCase(s, ROLE_NAMES[i])) { out = (Role)i; return true; }
  return false;
}
bool jackFromName(const char* s, JackMode& out) {
  for (uint8_t i = 0; i < 3; i++) if (eqNoCase(s, JACK_NAMES[i])) { out = (JackMode)(i + 1); return true; }
  return false;
}

void blankEntry(Entry& e) {
  memset(&e, 0, sizeof e);
  e.keyMode = KeyMode::Type;
  e.action = Action::None;
  e.gotoLevel = 1;
  e.volumePct = 100;
  e.vibrate = Tri::Inherit;
  e.jack = JackMode::Inherit;
}

static void blankLevel(Level& L) {
  memset(&L, 0, sizeof L);
  for (uint8_t i = 0; i < MAX_SOUND_PADS; i++) blankEntry(L.buttons[i]);
  L.jacks = true;
}

void defaultLevels(Config& c) {
  memset(&c.levels, 0, sizeof c.levels);
  blankLevel(c.levels.levels[0]);
  c.levels.count = 1;
}

// Copies a string field with a length warning. Returns false if it is not text.
static bool takeStr(JsonVariantConst v, char* dst, size_t cap, const char* what, ConfigReport& rep) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) { rep.warn("%s: expected text, ignored", what); return false; }
  const char* s = v.as<const char*>();
  if (strlen(s) > cap - 1) rep.warn("%s: longer than %d chars, truncated", what, (int)(cap - 1));
  copyStr(dst, cap, s);
  return true;
}

static bool parseEntry(JsonObjectConst o, Entry& e, int lvl, int pad, ConfigReport& rep, bool strict) {
  char what[40];
  blankEntry(e);

  snprintf(what, sizeof what, "level %d pad %d sound", lvl, pad);
  if (takeStr(o["sound"], e.sound, sizeof e.sound, what, rep) && e.sound[0] && !validSoundName(e.sound)) {
    rep.warn("%s: \"%s\" is not a valid .wav name, using none", what, e.sound);
    e.sound[0] = 0;
  }
  snprintf(what, sizeof what, "level %d pad %d label", lvl, pad);
  takeStr(o["label"], e.label, sizeof e.label, what, rep);
  snprintf(what, sizeof what, "level %d pad %d type", lvl, pad);
  takeStr(o["type"], e.type, sizeof e.type, what, rep);
  snprintf(what, sizeof what, "level %d pad %d key", lvl, pad);
  if (takeStr(o["key"], e.key, sizeof e.key, what, rep) && e.key[0] && !findKey(e.key)) {   // Appendix B
    rep.warn("%s: \"%s\" is not a key name (Appendix B), using none", what, e.key);
    e.key[0] = 0;
  }

  JsonVariantConst km = o["keyMode"];
  if (!km.isNull()) {
    if (!km.is<const char*>() || !keyModeFromName(km.as<const char*>(), e.keyMode)) {
      rep.warn("level %d pad %d keyMode: must be type, hold or tap, using type", lvl, pad);
      e.keyMode = KeyMode::Type;
    }
  }

  JsonVariantConst ac = o["action"];
  if (!ac.isNull()) {
    if (!ac.is<const char*>() || !actionFromName(ac.as<const char*>(), e.action)) {
      rep.structural(strict, "level %d pad %d: unknown action, level dropped", lvl, pad);
      return false;
    }
  }

  JsonVariantConst gl = o["goToLevel"];
  if (!gl.isNull()) {
    if (gl.is<int>() && gl.as<int>() >= 0 && gl.as<int>() <= 255) e.gotoLevel = (uint8_t)gl.as<int>();
    else { rep.warn("level %d pad %d goToLevel: expected a level number, using 1", lvl, pad); e.gotoLevel = 1; }
  }

  JsonVariantConst vp = o["volumePct"];
  if (!vp.isNull()) {
    if (!vp.is<double>()) rep.warn("level %d pad %d volumePct: expected a number, using 100", lvl, pad);
    else {
      double x = vp.as<double>(), y = x < 0 ? 0 : (x > 100 ? 100 : x);
      if (y != x) rep.warn("level %d pad %d volumePct: %g out of range, using %g", lvl, pad, x, y);
      e.volumePct = (uint8_t)(y + 0.5);
    }
  }

  JsonVariantConst vb = o["vibrate"];
  if (!vb.isNull()) {
    if (vb.is<bool>()) e.vibrate = vb.as<bool>() ? Tri::On : Tri::Off;
    else rep.warn("level %d pad %d vibrate: expected true, false or null, inheriting", lvl, pad);
  }

  JsonVariantConst jk = o["jack"];
  if (!jk.isNull()) {
    if (!jk.is<const char*>() || !jackFromName(jk.as<const char*>(), e.jack)) {
      rep.warn("level %d pad %d jack: must be follow, pulse, off or null, inheriting", lvl, pad);
      e.jack = JackMode::Inherit;
    }
  }
  return true;
}

static bool parseLevel(JsonVariantConst lv, Level& L, uint8_t nsp, int idx, ConfigReport& rep, bool strict) {
  blankLevel(L);
  if (!lv.is<JsonObjectConst>()) { rep.structural(strict, "level %d: not an object, dropped", idx); return false; }
  JsonObjectConst o = lv.as<JsonObjectConst>();

  char what[32];
  snprintf(what, sizeof what, "level %d name", idx);
  takeStr(o["name"], L.name, sizeof L.name, what, rep);

  JsonVariantConst b = o["buttons"];
  if (!b.is<JsonArrayConst>()) { rep.structural(strict, "level %d (%s): no buttons list, dropped", idx, L.name); return false; }
  if (b.size() != nsp) {
    rep.structural(strict, "level %d (%s): %u buttons, expected %u, dropped", idx, L.name, (unsigned)b.size(), (unsigned)nsp);
    return false;
  }
  uint8_t i = 0;
  for (JsonVariantConst ev : b.as<JsonArrayConst>()) {
    if (!ev.is<JsonObjectConst>()) { rep.structural(strict, "level %d pad %d: not an object, level dropped", idx, i + 1); return false; }
    if (!parseEntry(ev.as<JsonObjectConst>(), L.buttons[i], idx, i + 1, rep, strict)) return false;
    i++;
  }

  JsonVariantConst vib = o["vibration"];
  if (!vib.isNull()) {
    if (!vib.is<JsonArrayConst>()) rep.warn("level %d vibration: expected a list of ms, ignored", idx);
    else {
      snprintf(what, sizeof what, "level %d vibration", idx);
      L.vibrationLen = loadPatternArray(vib, L.vibration, what, rep);
      L.hasVibration = L.vibrationLen > 0;
    }
  }
  JsonVariantConst jacks = o["jacks"];
  if (!jacks.isNull()) {
    if (jacks.is<bool>()) L.jacks = jacks.as<bool>();
    else rep.warn("level %d jacks: expected true or false, ignored", idx);
  }
  return true;
}

void loadLevels(JsonVariantConst root, Config& c, ConfigReport& rep, bool strict) {
  uint8_t nsp = nSoundPads(c);
  memset(&c.levels, 0, sizeof c.levels);
  JsonVariantConst v = root["levels"];
  if (v.isNull()) { rep.warn("levels: none in the file, using one blank level"); defaultLevels(c); return; }
  if (!v.is<JsonArrayConst>()) { rep.structural(strict, "levels: expected a list, using one blank level"); defaultLevels(c); return; }

  int idx = 0;
  for (JsonVariantConst lv : v.as<JsonArrayConst>()) {
    idx++;
    if (c.levels.count >= MAX_LEVELS) { rep.structural(strict, "levels: more than %d levels, level %d dropped", (int)MAX_LEVELS, idx); continue; }
    if (parseLevel(lv, c.levels.levels[c.levels.count], nsp, idx, rep, strict)) c.levels.count++;
  }
  if (c.levels.count == 0) { rep.structural(strict, "levels: no valid level, using one blank level"); defaultLevels(c); }

  for (uint8_t l = 0; l < c.levels.count; l++)
    for (uint8_t p = 0; p < nsp; p++) {
      Entry& e = c.levels.levels[l].buttons[p];
      if (e.action == Action::GoToLevel && (e.gotoLevel < 1 || e.gotoLevel > c.levels.count)) {
        rep.warn("level %d pad %d: goToLevel %d is outside 1..%d, using 1", l + 1, p + 1, (int)e.gotoLevel, (int)c.levels.count);
        e.gotoLevel = 1;
      }
    }
}

void saveLevels(const Config& c, JsonObject root) {
  uint8_t nsp = nSoundPads(c);
  JsonArray arr = root["levels"].to<JsonArray>();
  for (uint8_t l = 0; l < c.levels.count; l++) {
    const Level& L = c.levels.levels[l];
    JsonObject lo = arr.add<JsonObject>();
    lo["name"] = copied(L.name);
    if (L.hasVibration) {
      JsonArray vib = lo["vibration"].to<JsonArray>();
      for (uint8_t i = 0; i < L.vibrationLen; i++) vib.add((int)L.vibration[i]);
    }
    if (!L.jacks) lo["jacks"] = false;
    JsonArray bs = lo["buttons"].to<JsonArray>();
    for (uint8_t p = 0; p < nsp; p++) {
      const Entry& e = L.buttons[p];
      JsonObject eo = bs.add<JsonObject>();
      eo["sound"] = copied(e.sound);
      eo["label"] = copied(e.label);
      if (e.type[0]) eo["type"] = copied(e.type);
      if (e.key[0]) eo["key"] = copied(e.key);
      if (e.keyMode != KeyMode::Type) eo["keyMode"] = keyModeName(e.keyMode);
      if (e.action != Action::None) eo["action"] = actionName(e.action);
      if (e.action == Action::GoToLevel) eo["goToLevel"] = (int)e.gotoLevel;
      if (e.volumePct != 100) eo["volumePct"] = (int)e.volumePct;
      if (e.vibrate != Tri::Inherit) eo["vibrate"] = (e.vibrate == Tri::On);
      if (e.jack != JackMode::Inherit) eo["jack"] = jackName(e.jack);
    }
  }
}

}  // namespace config
}  // namespace sb
