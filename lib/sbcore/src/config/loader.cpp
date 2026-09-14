// Document <-> Config through the descriptor table (FirmwareSpec.md §13.4,
// §13.5), plus the structured sections that are not scalars: ownerLabel,
// padChannels, roles, padPressPct, cues, the level-cue pattern. Levels are in
// levels.cpp.
#include "config/loader.h"
#include "config/levels.h"
#include "util/strutil.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

using namespace ArduinoJson;

namespace sb {
namespace config {

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
JsonVariantConst lookup(JsonVariantConst root, const char* path) {
  JsonVariantConst cur = root;
  char seg[48];
  const char* p = path;
  while (p && *p) {
    const char* dot = strchr(p, '.');
    size_t len = dot ? (size_t)(dot - p) : strlen(p);
    if (len >= sizeof seg) len = sizeof seg - 1;
    memcpy(seg, p, len); seg[len] = 0;
    cur = cur[(const char*)seg];
    if (cur.isNull()) return cur;
    if (!dot) break;
    p = dot + 1;
  }
  return cur;
}

JsonVariant ensure(JsonObject root, const char* path) {
  JsonObject cur = root;
  char seg[48];
  const char* p = path;
  while (true) {
    const char* dot = strchr(p, '.');
    size_t len = dot ? (size_t)(dot - p) : strlen(p);
    if (len >= sizeof seg) len = sizeof seg - 1;
    memcpy(seg, p, len); seg[len] = 0;
    if (!dot) {
      // Converting the proxy to JsonVariant does not create the member; to<>() does.
      return cur[seg].to<JsonVariant>();   // `seg` is char*: the key is copied
    }
    JsonVariant next = cur[seg];
    if (!next.is<JsonObject>()) next = cur[seg].to<JsonObject>();
    cur = next.as<JsonObject>();
    p = dot + 1;
  }
}

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------
static uint8_t*       fieldPtr(Config& c, const SettingDesc& d)       { return reinterpret_cast<uint8_t*>(&c) + d.offset; }
static const uint8_t* fieldPtr(const Config& c, const SettingDesc& d) { return reinterpret_cast<const uint8_t*>(&c) + d.offset; }

static double clampNum(const SettingDesc& d, double x, bool& changed) {
  changed = false;
  if ((d.flags & ZERO_OFF) && x == 0) return 0;
  if (x < d.min) { changed = true; return d.min; }
  if (x > d.max) { changed = true; return d.max; }
  return x;
}

static void storeNum(Config& c, const SettingDesc& d, double x) {
  uint8_t* p = fieldPtr(c, d);
  switch (d.type) {
    case SType::Bool: { bool v = x != 0; memcpy(p, &v, sizeof v); break; }
    case SType::U8:   { uint8_t v = (uint8_t)(x + 0.5); memcpy(p, &v, sizeof v); break; }
    case SType::U16:  { uint16_t v = (uint16_t)(x + 0.5); memcpy(p, &v, sizeof v); break; }
    case SType::U32:  { uint32_t v = (uint32_t)(x + 0.5); memcpy(p, &v, sizeof v); break; }
    case SType::F32:  { float v = (float)x; memcpy(p, &v, sizeof v); break; }
    case SType::Enum: { uint8_t v = (uint8_t)x; memcpy(p, &v, sizeof v); break; }
    default: break;
  }
}

static double readNum(const Config& c, const SettingDesc& d) {
  const uint8_t* p = fieldPtr(c, d);
  switch (d.type) {
    case SType::Bool: { bool v; memcpy(&v, p, sizeof v); return v ? 1 : 0; }
    case SType::U8:   { uint8_t v; memcpy(&v, p, sizeof v); return v; }
    case SType::U16:  { uint16_t v; memcpy(&v, p, sizeof v); return v; }
    case SType::U32:  { uint32_t v; memcpy(&v, p, sizeof v); return v; }
    case SType::F32:  { float v; memcpy(&v, p, sizeof v); return v; }
    case SType::Enum: { uint8_t v; memcpy(&v, p, sizeof v); return v; }
    default: return 0;
  }
}

static void storeStr(Config& c, const SettingDesc& d, const char* s) {
  char* dst = reinterpret_cast<char*>(fieldPtr(c, d));
  size_t cap = d.size;
  size_t maxLen = (size_t)d.max;
  if (maxLen + 1 < cap) cap = maxLen + 1;
  copyStr(dst, cap, s);
}

static void fmtNum(const SettingDesc& d, double x, char* out, size_t n) {
  if (d.type == SType::F32) {
    snprintf(out, n, "%g", x);
  } else {
    snprintf(out, n, "%lu", (unsigned long)(x + 0.5));
  }
}

bool setScalarText(Config& c, const SettingDesc& d, const char* text, ConfigReport* rep) {
  if (!text) return false;
  switch (d.type) {
    case SType::Bool: {
      if (eqNoCase(text, "true") || eqNoCase(text, "1") || eqNoCase(text, "on") || eqNoCase(text, "yes")) { storeNum(c, d, 1); return true; }
      if (eqNoCase(text, "false") || eqNoCase(text, "0") || eqNoCase(text, "off") || eqNoCase(text, "no")) { storeNum(c, d, 0); return true; }
      return false;
    }
    case SType::U8: case SType::U16: case SType::U32: case SType::F32: {
      char* end = nullptr;
      double x = strtod(text, &end);
      if (end == text || (end && *end != 0)) return false;
      bool changed;
      double y = clampNum(d, x, changed);
      if (changed && rep) {
        char a[24], b[24]; fmtNum(d, x, a, sizeof a); fmtNum(d, y, b, sizeof b);
        rep->warn("%s: %s out of range, using %s", d.path, a, b);
      }
      storeNum(c, d, y);
      return true;
    }
    case SType::Enum: {
      int idx = enumIndex(d.enums, text);
      if (idx < 0) return false;
      storeNum(c, d, idx);
      return true;
    }
    case SType::String: {
      size_t len = strlen(text);
      if (len < (size_t)d.min) return false;
      if (len > (size_t)d.max && rep) rep->warn("%s: longer than %d chars, truncated", d.path, (int)d.max);
      storeStr(c, d, text);
      return true;
    }
  }
  return false;
}

bool getScalarText(const Config& c, const SettingDesc& d, char* out, size_t n) {
  switch (d.type) {
    case SType::Bool:   copyStr(out, n, readNum(c, d) != 0 ? "true" : "false"); return true;
    case SType::U8: case SType::U16: case SType::U32: case SType::F32:
      fmtNum(d, readNum(c, d), out, n); return true;
    case SType::Enum:   return enumName(d.enums, (int)readNum(c, d), out, n);
    case SType::String: copyStr(out, n, reinterpret_cast<const char*>(fieldPtr(c, d))); return true;
  }
  return false;
}

static void applyDefault(Config& c, const SettingDesc& d) { setScalarText(c, d, d.def, nullptr); }

static void loadScalar(JsonVariantConst root, Config& c, const SettingDesc& d, ConfigReport& rep) {
  JsonVariantConst v = lookup(root, d.path);
  if (v.isNull()) { applyDefault(c, d); return; }
  switch (d.type) {
    case SType::Bool:
      if (v.is<bool>()) { storeNum(c, d, v.as<bool>() ? 1 : 0); return; }
      if (v.is<int>()) { storeNum(c, d, v.as<int>() != 0 ? 1 : 0); return; }
      rep.warn("%s: expected true/false, using %s", d.path, d.def);
      applyDefault(c, d);
      return;
    case SType::U8: case SType::U16: case SType::U32: case SType::F32: {
      if (!v.is<double>() || v.is<bool>()) { rep.warn("%s: expected a number, using %s", d.path, d.def); applyDefault(c, d); return; }
      double x = v.as<double>();
      bool changed;
      double y = clampNum(d, x, changed);
      if (changed) {
        char a[24], b[24]; fmtNum(d, x, a, sizeof a); fmtNum(d, y, b, sizeof b);
        rep.warn("%s: %s out of range, using %s", d.path, a, b);
      }
      storeNum(c, d, y);
      return;
    }
    case SType::Enum: {
      if (!v.is<const char*>()) { rep.warn("%s: expected one of %s, using %s", d.path, d.enums, d.def); applyDefault(c, d); return; }
      int idx = enumIndex(d.enums, v.as<const char*>());
      if (idx < 0) { rep.warn("%s: \"%s\" is not one of %s, using %s", d.path, v.as<const char*>(), d.enums, d.def); applyDefault(c, d); return; }
      storeNum(c, d, idx);
      return;
    }
    case SType::String: {
      if (!v.is<const char*>()) { rep.warn("%s: expected text, using the default", d.path); applyDefault(c, d); return; }
      const char* s = v.as<const char*>();
      size_t len = strlen(s);
      if (len < (size_t)d.min) { rep.warn("%s: shorter than %d chars, using the default", d.path, (int)d.min); applyDefault(c, d); return; }
      if (len > (size_t)d.max) rep.warn("%s: longer than %d chars, truncated", d.path, (int)d.max);
      storeStr(c, d, s);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Structured sections
// ---------------------------------------------------------------------------
static void defaultOwnerLabel(Config& c) {
  memset(c.device.ownerLabel, 0, sizeof c.device.ownerLabel);
  copyStr(c.device.ownerLabel[0], 21, "If found please call");
  copyStr(c.device.ownerLabel[1], 21, "(phone number)");
  c.device.ownerLines = 2;      // TODO(OPEN-3): exact text for Annalise's unit
}

static void loadOwnerLabel(JsonVariantConst root, Config& c, ConfigReport& rep) {
  defaultOwnerLabel(c);
  JsonVariantConst v = lookup(root, "device.ownerLabel");
  if (v.isNull()) return;
  if (!v.is<JsonArrayConst>()) { rep.warn("device.ownerLabel: expected a list of lines, using the default"); return; }
  memset(c.device.ownerLabel, 0, sizeof c.device.ownerLabel);
  uint8_t n = 0;
  for (JsonVariantConst e : v.as<JsonArrayConst>()) {
    if (n >= OWNER_LINES) { rep.warn("device.ownerLabel: more than %d lines, the rest ignored", (int)OWNER_LINES); break; }
    if (!e.is<const char*>()) { rep.warn("device.ownerLabel: line %d is not text, skipped", n + 1); continue; }
    const char* s = e.as<const char*>();
    if (strlen(s) > 20) rep.warn("device.ownerLabel: line %d longer than 20 chars, truncated", n + 1);
    copyStr(c.device.ownerLabel[n], 21, s);
    n++;
  }
  c.device.ownerLines = n;
}

static void defaultPadChannels(Config& c) { for (uint8_t i = 0; i < N_PADS; i++) c.hardware.padChannels[i] = 2 + i; }

static void loadPadChannels(JsonVariantConst root, Config& c, ConfigReport& rep, bool strict) {
  defaultPadChannels(c);
  JsonVariantConst v = lookup(root, "hardware.padChannels");
  if (v.isNull()) return;
  bool ok = v.is<JsonArrayConst>() && v.size() == N_PADS;
  uint8_t tmp[N_PADS] = {0};
  if (ok) {
    uint8_t seen = 0, i = 0;
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
      if (!e.is<int>()) { ok = false; break; }
      int ch = e.as<int>();
      if (ch < 2 || ch > 5 || (seen & (1 << ch))) { ok = false; break; }
      seen |= 1 << ch;
      tmp[i++] = (uint8_t)ch;
    }
  }
  if (!ok) { rep.structural(strict, "hardware.padChannels: must be a permutation of 2,3,4,5, using [2,3,4,5]"); return; }   // TODO(OPEN-5)
  memcpy(c.hardware.padChannels, tmp, sizeof tmp);
}

static void defaultRoles(Config& c) {
  c.pads.roles[0] = Role::Level;
  for (uint8_t i = 1; i < N_PADS; i++) c.pads.roles[i] = Role::Sound;
}

static void loadRoles(JsonVariantConst root, Config& c, ConfigReport& rep, bool strict) {
  defaultRoles(c);
  JsonVariantConst v = lookup(root, "pads.roles");
  if (v.isNull()) return;
  if (!v.is<JsonArrayConst>() || v.size() != N_PADS) { rep.structural(strict, "pads.roles: expected 4 entries, using the default"); return; }
  Role tmp[N_PADS];
  uint8_t i = 0, sounds = 0, levels = 0;
  for (JsonVariantConst e : v.as<JsonArrayConst>()) {
    Role r;
    if (!e.is<const char*>() || !roleFromName(e.as<const char*>(), r)) {
      rep.structural(strict, "pads.roles: entry %d must be level, sound or none, using the default", i + 1);
      return;
    }
    if (r == Role::Level) { if (levels++ > 0) { rep.warn("pads.roles: a second level pad at P%d is treated as a sound pad", i + 1); r = Role::Sound; } }
    if (r == Role::Sound) sounds++;
    tmp[i++] = r;
  }
  if (sounds == 0) { rep.structural(strict, "pads.roles: at least one sound pad is required, using the default"); return; }
  memcpy(c.pads.roles, tmp, sizeof tmp);
}

static void loadPadPressPct(JsonVariantConst root, Config& c, ConfigReport& rep) {
  c.touch.hasPadPressPct = false;
  for (uint8_t i = 0; i < N_PADS; i++) c.touch.padPressPct[i] = 0;
  JsonVariantConst v = lookup(root, "touch.padPressPct");
  if (v.isNull()) return;
  if (!v.is<JsonArrayConst>() || v.size() != N_PADS) { rep.warn("touch.padPressPct: expected null or 4 values, ignored"); return; }
  uint8_t i = 0;
  for (JsonVariantConst e : v.as<JsonArrayConst>()) {
    if (e.isNull()) { i++; continue; }
    if (!e.is<double>()) { rep.warn("touch.padPressPct: P%d is not a number, no override", i + 1); i++; continue; }
    double x = e.as<double>();
    double y = x < 1.0 ? 1.0 : (x > 10.0 ? 10.0 : x);
    if (y != x) rep.warn("touch.padPressPct: P%d %g out of range, using %g", i + 1, x, y);
    c.touch.padPressPct[i] = (float)y;
    c.touch.hasPadPressPct = true;
    i++;
  }
}

static void loadCue(JsonVariantConst root, const char* path, char* dst, const char* def, ConfigReport& rep) {
  copyStr(dst, 41, def);
  JsonVariantConst v = lookup(root, path);
  if (v.isNull()) return;
  if (!v.is<const char*>()) { rep.warn("%s: expected a file name, using %s", path, def[0] ? def : "none"); return; }
  const char* s = v.as<const char*>();
  if (!validSoundName(s)) { rep.warn("%s: \"%s\" is not a valid .wav name, using none", path, s); dst[0] = 0; return; }
  copyStr(dst, 41, s);
}

static void loadCues(JsonVariantConst root, Config& c, ConfigReport& rep) {
  loadCue(root, "audio.cues.startup",    c.audio.cues.startup,    "_startup.wav", rep);
  loadCue(root, "audio.cues.click",      c.audio.cues.click,      "_click.wav",   rep);
  loadCue(root, "audio.cues.saved",      c.audio.cues.saved,      "_saved.wav",   rep);
  loadCue(root, "audio.cues.lowBattery", c.audio.cues.lowBattery, "",             rep);   // TODO(OPEN-16)
}

static void defaultPattern(Config& c) {
  c.levelChange.vibration.pattern[0] = 200;
  c.levelChange.vibration.pattern[1] = 100;
  c.levelChange.vibration.pattern[2] = 200;
  c.levelChange.vibration.patternLen = 3;
}

// Shared by the level-cue pattern and a level's own pattern.
uint8_t loadPatternArray(JsonVariantConst v, uint16_t* out, const char* what, ConfigReport& rep) {
  uint8_t n = 0;
  for (JsonVariantConst e : v.as<JsonArrayConst>()) {
    if (n >= MAX_PATTERN) { rep.warn("%s: more than %d segments, truncated", what, (int)MAX_PATTERN); break; }
    if (!e.is<double>()) { rep.warn("%s: segment %d is not a number, pattern ignored", what, n + 1); return 0; }
    double x = e.as<double>();
    if (x < 0) x = 0;
    if (x > 10000) x = 10000;
    out[n++] = (uint16_t)(x + 0.5);
  }
  return n;
}

static void loadPattern(JsonVariantConst root, Config& c, ConfigReport& rep) {
  defaultPattern(c);
  JsonVariantConst v = lookup(root, "levelChange.vibration.pattern");
  if (v.isNull()) return;
  if (!v.is<JsonArrayConst>()) { rep.warn("levelChange.vibration.pattern: expected a list of ms, using the default"); return; }
  uint16_t tmp[MAX_PATTERN];
  uint8_t n = loadPatternArray(v, tmp, "levelChange.vibration.pattern", rep);
  if (n == 0) { rep.warn("levelChange.vibration.pattern: empty, using the default"); return; }
  memcpy(c.levelChange.vibration.pattern, tmp, n * sizeof(uint16_t));
  c.levelChange.vibration.patternLen = n;
}

// ---------------------------------------------------------------------------
// Cross-field rules (§13.4)
// ---------------------------------------------------------------------------
void crossField(Config& c, ConfigReport& rep, bool strict) {
  if (c.touch.releasePct > c.touch.pressPct) {
    rep.structural(strict, "touch.releasePct %g must not exceed touch.pressPct %g, using %g",
                   (double)c.touch.releasePct, (double)c.touch.pressPct, (double)c.touch.pressPct);
    c.touch.releasePct = c.touch.pressPct;
  }
  uint32_t minHold = (uint32_t)c.power.offHoldMs + 500;
  if (c.menu.holdMs < minHold) {
    rep.structural(strict, "menu.holdMs %u must be at least power.offHoldMs + 500 = %u, using %u",
                   (unsigned)c.menu.holdMs, (unsigned)minHold, (unsigned)minHold);
    c.menu.holdMs = (uint16_t)minHold;
  }
  uint32_t maxTap = c.power.offHoldMs > 200 ? (uint32_t)c.power.offHoldMs - 200 : 0;   // a both-tap must end well before the OFF stage
  if (c.levelChange.bothTapMs > maxTap) {
    rep.structural(strict, "levelChange.bothTapMs %u must be at most power.offHoldMs - 200 = %u, using %u",
                   (unsigned)c.levelChange.bothTapMs, (unsigned)maxTap, (unsigned)maxTap);
    c.levelChange.bothTapMs = (uint16_t)maxTap;
  }
  if (c.power.shutdownPct >= c.power.lowBatteryWarnPct) {
    uint8_t fix = c.power.lowBatteryWarnPct > 0 ? c.power.lowBatteryWarnPct - 1 : 0;
    rep.structural(strict, "power.shutdownPct %u must be below power.lowBatteryWarnPct %u, using %u",
                   (unsigned)c.power.shutdownPct, (unsigned)c.power.lowBatteryWarnPct, (unsigned)fix);
    c.power.shutdownPct = fix;
  }
}

// ---------------------------------------------------------------------------
// Whole document
// ---------------------------------------------------------------------------
void defaults(Config& c) {
  memset(&c, 0, sizeof c);
  c.schema = SCHEMA_VERSION;
  c.revision = 0;
  for (size_t i = 0; i < N_SETTINGS; i++) applyDefault(c, SETTINGS[i]);
  defaultOwnerLabel(c);
  defaultPadChannels(c);
  defaultRoles(c);
  c.touch.hasPadPressPct = false;
  copyStr(c.audio.cues.startup, 41, "_startup.wav");
  copyStr(c.audio.cues.click, 41, "_click.wav");
  copyStr(c.audio.cues.saved, 41, "_saved.wav");
  c.audio.cues.lowBattery[0] = 0;
  defaultPattern(c);
  defaultLevels(c);
}

bool load(JsonVariantConst root, Config& out, ConfigReport& rep, bool strict) {
  defaults(out);
  JsonVariantConst sv = root["schema"];
  out.schema = sv.is<int>() ? (uint8_t)sv.as<int>() : SCHEMA_VERSION;
  JsonVariantConst rv = root["revision"];
  out.revision = rv.is<unsigned long>() ? (uint32_t)rv.as<unsigned long>() : 0;

  for (size_t i = 0; i < N_SETTINGS; i++) loadScalar(root, out, SETTINGS[i], rep);
  loadOwnerLabel(root, out, rep);
  loadPadChannels(root, out, rep, strict);
  loadRoles(root, out, rep, strict);
  loadPadPressPct(root, out, rep);
  loadCues(root, out, rep);
  loadPattern(root, out, rep);
  loadLevels(root, out, rep, strict);
  crossField(out, rep, strict);
  return rep.ok();
}

static void saveScalar(const Config& c, const SettingDesc& d, JsonObject root) {
  JsonVariant leaf = ensure(root, d.path);
  switch (d.type) {
    case SType::Bool:   leaf.set(readNum(c, d) != 0); break;
    case SType::U8: case SType::U16: case SType::U32:
      leaf.set((unsigned long)(readNum(c, d) + 0.5)); break;
    case SType::F32:    leaf.set((float)readNum(c, d)); break;
    case SType::Enum: {
      char name[24];
      if (enumName(d.enums, (int)readNum(c, d), name, sizeof name)) leaf.set(copied(name));
      else leaf.set(copied(d.def));
      break;
    }
    case SType::String: leaf.set(copied(reinterpret_cast<const char*>(fieldPtr(c, d)))); break;
  }
}

void save(const Config& c, JsonObject root) {
  root["schema"] = (int)SCHEMA_VERSION;
  if (root["revision"].isNull()) root["revision"] = (unsigned long)c.revision;

  for (size_t i = 0; i < N_SETTINGS; i++) saveScalar(c, SETTINGS[i], root);

  JsonArray owner = ensure(root, "device.ownerLabel").to<JsonArray>();
  for (uint8_t i = 0; i < c.device.ownerLines && i < OWNER_LINES; i++) owner.add(copied(c.device.ownerLabel[i]));

  JsonArray chans = ensure(root, "hardware.padChannels").to<JsonArray>();
  for (uint8_t i = 0; i < N_PADS; i++) chans.add((int)c.hardware.padChannels[i]);

  JsonArray roles = ensure(root, "pads.roles").to<JsonArray>();
  for (uint8_t i = 0; i < N_PADS; i++) roles.add(roleName(c.pads.roles[i]));

  JsonVariant ppp = ensure(root, "touch.padPressPct");
  if (!c.touch.hasPadPressPct) ppp.clear();
  else {
    JsonArray a = ppp.to<JsonArray>();
    for (uint8_t i = 0; i < N_PADS; i++) {
      if (c.touch.padPressPct[i] > 0) a.add(c.touch.padPressPct[i]);
      else a.add(nullptr);
    }
  }

  ensure(root, "audio.cues.startup").set(copied(c.audio.cues.startup));
  ensure(root, "audio.cues.click").set(copied(c.audio.cues.click));
  ensure(root, "audio.cues.saved").set(copied(c.audio.cues.saved));
  ensure(root, "audio.cues.lowBattery").set(copied(c.audio.cues.lowBattery));

  JsonArray pat = ensure(root, "levelChange.vibration.pattern").to<JsonArray>();
  for (uint8_t i = 0; i < c.levelChange.vibration.patternLen; i++) pat.add((int)c.levelChange.vibration.pattern[i]);

  saveLevels(c, root);
}

uint32_t bumpRevision(JsonObject root) {
  uint32_t r = root["revision"] | 0UL;
  r++;
  root["revision"] = (unsigned long)r;
  return r;
}

}  // namespace config
}  // namespace sb

namespace sb {
namespace config {

void merge(JsonObject target, JsonObjectConst patch) {
  for (JsonPairConst kv : patch) {
    const char* key = kv.key().c_str();
    JsonVariantConst v = kv.value();
    if (v.isNull()) { target.remove(key); continue; }
    if (v.is<JsonObjectConst>()) {
      JsonVariant existing = target[copied(key)];
      if (!existing.is<JsonObject>()) existing = target[copied(key)].to<JsonObject>();
      merge(existing.as<JsonObject>(), v.as<JsonObjectConst>());
    } else {
      target[copied(key)].set(v);
    }
  }
}

}  // namespace config
}  // namespace sb
