#include "app/menu_model.h"
#include "config/loader.h"
#include "util/strutil.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

namespace sb {

void MenuModel::add(MenuKind k, const SettingDesc* d, uint8_t order, const char* label) {
  if (n_ >= MENU_MAX_ITEMS || !order) return;
  uint8_t pos = n_;
  while (pos > 0 && items_[pos - 1].order > order) { items_[pos] = items_[pos - 1]; pos--; }   // stable by order
  items_[pos] = { k, d, order, label };
  n_++;
}

void MenuModel::build() {                                    // §14.3: the table rows with MENU, plus the fixed items
  n_ = 0; i_ = 0;
  for (size_t i = 0; i < N_SETTINGS; i++)
    if ((SETTINGS[i].flags & MENU) && SETTINGS[i].menuOrder) add(MenuKind::Setting, &SETTINGS[i], SETTINGS[i].menuOrder, SETTINGS[i].label);
  // Volume (§14.3 item 2) was dropped at CP-9: the − / + buttons set it from the normal view, with the popup.
  add(MenuKind::PairSpeaker, nullptr, 4,  "Pair BT speaker");
  add(MenuKind::PMode,       nullptr, 8,  "P mode");           // §4.5: on / off for this power-up only
  add(MenuKind::Recalibrate, nullptr, 9,  "Recalibrate buttons");
  add(MenuKind::WifiSetup,   nullptr, 10, "Wi-Fi setup");
  add(MenuKind::Exit,        nullptr, 11, "Save and exit");
  add(MenuKind::Cancel,      nullptr, 12, "Cancel changes");
}

const char* MenuModel::actionVerb() const {
  switch (item().kind) {
    case MenuKind::Exit:        return "SAVE";
    case MenuKind::Cancel:      return "CANCEL";
    case MenuKind::PairSpeaker: return "PAIR";
    case MenuKind::Recalibrate: return "START";
    case MenuKind::WifiSetup:   return setupOn_ ? "STOP" : "START";
    default: return "";
  }
}

void MenuModel::open(const Config& c, uint8_t volumePct) {
  for (uint8_t i = 0; i < n_; i++) {
    snap_[i][0] = 0;
    if (items_[i].kind == MenuKind::Setting) config::getScalarText(c, *items_[i].desc, snap_[i], MENU_TEXT);
  }
  snapVol_ = volumePct;
  pairWipe_ = false; armed_ = false;
}

bool MenuModel::changed(const Config& c) const {
  char cur[MENU_TEXT];
  for (uint8_t i = 0; i < n_; i++) {
    if (items_[i].kind != MenuKind::Setting) continue;
    if (!config::getScalarText(c, *items_[i].desc, cur, sizeof cur)) continue;
    if (strcmp(cur, snap_[i])) return true;
  }
  return false;
}

void MenuModel::formatNum(const SettingDesc& d, double x, char* out, size_t n) {
  if (d.type == SType::F32) {
    snprintf(out, n, "%.2f", x);
    size_t len = strlen(out);                                  // trim trailing zeros: 0.50 -> 0.5, 3.00 -> 3
    while (len > 0 && out[len - 1] == '0') out[--len] = 0;
    if (len > 0 && out[len - 1] == '.') out[--len] = 0;
  } else snprintf(out, n, "%ld", (long)llround(x));
}

bool MenuModel::stepSetting(const Config& c, const SettingDesc& d, int dir, char* out, size_t n) {
  char cur[MENU_TEXT];
  if (!config::getScalarText(c, d, cur, sizeof cur)) return false;
  switch (d.type) {
    case SType::Bool: {                                        // up = ON, down = OFF (§14.4)
      bool on = eqNoCase(cur, "true");
      if (dir > 0 && !on) { copyStr(out, n, "true"); return true; }
      if (dir < 0 && on)  { copyStr(out, n, "false"); return true; }
      return false;
    }
    case SType::Enum: {
      int idx = enumIndex(d.enums, cur), cnt = enumCount(d.enums);
      int nxt = idx + (dir > 0 ? 1 : -1);
      if (idx < 0 || nxt < 0 || nxt >= cnt) return false;
      return enumName(d.enums, nxt, out, n);
    }
    case SType::U8: case SType::U16: case SType::U32: case SType::F32: {
      double x = strtod(cur, nullptr), y;
      if (d.choices && *d.choices) {                           // the list: the nearest choice beyond the current value
        bool found = false; double best = 0;
        const char* p = d.choices;
        while (*p) {
          double v = strtod(p, nullptr);
          if (dir > 0 ? (v > x + 1e-6 && (!found || v < best)) : (v < x - 1e-6 && (!found || v > best))) { best = v; found = true; }
          const char* bar = strchr(p, '|');
          if (!bar) break;
          p = bar + 1;
        }
        if (!found) return false;
        y = best;
      } else if (dir > 0) {                                    // the range by step, clamped; ZERO_OFF: 0 sits below min
        if (x < d.min) y = d.min;
        else if (x + d.step <= d.max + 1e-6) y = x + d.step;
        else if (x < d.max - 1e-6) y = d.max;
        else return false;
      } else {
        if (x > d.max) y = d.max;
        else if (x - d.step >= d.min - 1e-6) y = x - d.step;
        else if (x > d.min + 1e-6) y = d.min;
        else if ((d.flags & ZERO_OFF) && x > 0) y = 0;
        else return false;
      }
      formatNum(d, y, out, n);
      return true;
    }
    default: return false;
  }
}

bool MenuModel::stepText(const Config& c, int dir, char* out, size_t n) const {
  if (item().kind != MenuKind::Setting || !item().desc) return false;
  return stepSetting(c, *item().desc, dir, out, n);
}

bool MenuModel::stepVolume(uint8_t cur, uint8_t stepPct, int dir, uint8_t& out) {
  if (!stepPct) stepPct = 10;
  int v = (int)cur + (dir > 0 ? (int)stepPct : -(int)stepPct);
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  if (v == (int)cur) return false;
  out = (uint8_t)v;
  return true;
}

void MenuModel::unitOf(const char* label, char* unit, size_t n) {
  unit[0] = 0;
  size_t len = label ? strlen(label) : 0;
  if (len < 4 || label[len - 1] != ')') return;
  const char* open = strrchr(label, '(');
  if (!open || open == label || open[-1] != ' ') return;
  size_t ulen = (size_t)(label + len - 1 - (open + 1));
  if (ulen == 0 || ulen > 3 || ulen >= n) return;
  for (size_t i = 0; i < ulen; i++) if (open[1 + i] == ' ' || open[1 + i] == ',' || open[1 + i] == '=') return;
  memcpy(unit, open + 1, ulen); unit[ulen] = 0;
}

void MenuModel::label(char* out, size_t n) const {
  if (item().kind == MenuKind::WifiSetup && setupOn_) { copyStr(out, n, "Stop Wi-Fi setup"); return; }
  const MenuItem& it = item();
  copyStr(out, n, it.label ? it.label : "");
  if (it.kind != MenuKind::Setting) return;
  char unit[8];
  unitOf(out, unit, sizeof unit);
  if (unit[0]) {                                               // "Screen brightness (%)" -> "Screen brightness"; the unit joins the value
    char* open = strrchr(out, '(');
    if (open && open > out) open[-1] = 0;
  }
}

void MenuModel::valueText(const Config& c, uint8_t volumePct, char* out, size_t n) const {
  const MenuItem& it = item();
  out[0] = 0;
  if (it.kind == MenuKind::Volume) { snprintf(out, n, "%u", (unsigned)volumePct); return; }
  if (it.kind == MenuKind::PMode)  { copyStr(out, n, pmodeOn_ ? "ON" : "OFF"); return; }
  if (it.kind != MenuKind::Setting) {                          // an action: the pad to press, by the word shown above it
    if (it.kind == MenuKind::PairSpeaker && pairWipe_) copyStr(out, n, "forget all and pair");
    else snprintf(out, n, "press %s%s", actionVerb(), confirmNeeded() ? " twice" : "");
    return;
  }
  if (!it.desc) return;
  char cur[MENU_TEXT];
  if (!config::getScalarText(c, *it.desc, cur, sizeof cur)) return;
  switch (it.desc->type) {
    case SType::Bool: copyStr(out, n, eqNoCase(cur, "true") ? "ON" : "OFF"); return;
    case SType::Enum: {
      copyStr(out, n, cur);
      for (char* p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
      return;
    }
    default: {
      char unit[8];
      unitOf(it.desc->label, unit, sizeof unit);
      if (unit[0]) snprintf(out, n, "%s %s", cur, unit); else copyStr(out, n, cur);
      return;
    }
  }
}

}  // namespace sb
