// The Quick Menu's items and values (FirmwareSpec.md §14): the descriptor rows
// flagged MENU plus the fixed runtime and action items, in menuOrder; the
// cursor, value stepping from `choices` or the range, the label and value
// texts for the screen, and the snapshot that Cancel restores. Pure logic: the
// app applies every change live (§14.5) and saves once on exit.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config/config.h"
#include "config/settings_table.h"

namespace sb {

enum class MenuKind : uint8_t { Setting, Volume, PairSpeaker, Recalibrate, WifiSetup, Exit, Cancel };

struct MenuItem {
  MenuKind           kind;
  const SettingDesc* desc;      // Setting items: the table row
  uint8_t            order;     // §14.3 position, 1-based
  const char*        label;     // fixed items; a Setting item shows its row's label
};

constexpr uint8_t MENU_MAX_ITEMS = 24;
constexpr uint8_t MENU_TEXT      = 40;

class MenuModel {
 public:
  void build();                                              // once: the items in order
  uint8_t count() const { return n_; }
  uint8_t index() const { return i_; }
  const MenuItem& item() const { return items_[i_]; }
  const MenuItem& at(uint8_t i) const { return items_[i < n_ ? i : 0]; }
  void first() { i_ = 0; }
  void next() { if (n_) i_ = (uint8_t)((i_ + 1) % n_); }    // §14.4: wraps
  void prev() { if (n_) i_ = (uint8_t)((i_ + n_ - 1) % n_); }
  bool isAction() const { MenuKind k = item().kind; return k != MenuKind::Setting && k != MenuKind::Volume; }
  const char* actionVerb() const;                            // the word above the OK pad: SAVE, CANCEL, PAIR, START ("" for a value item)
  // An action runs on the "up" press (P2 or +), like raising a value (CP-9: the + hold of the draft was
  // an accident guard nobody could read). Wi-Fi setup alone wants that press twice: the first arms it.
  bool confirmNeeded() const { return item().kind == MenuKind::WifiSetup; }
  bool armed() const { return armed_; }
  void arm() { armed_ = true; }
  void disarm() { armed_ = false; }
  void setPairWipeOffered(bool on) { pairWipe_ = on; }       // after "no speaker found": the next pair press forgets every speaker first (§7.3 step 4)
  bool pairWipeOffered() const { return pairWipe_; }

  // Entry: every Setting item's value and the volume are remembered for Cancel and changed().
  void open(const Config& c, uint8_t volumePct);
  bool changed(const Config& c) const;                       // a Setting differs from its snapshot (the volume persists on its own, §6.2)
  const char* snapshotText(uint8_t i) const { return snap_[i < n_ ? i : 0]; }   // "" for a non-Setting item
  uint8_t snapshotVolume() const { return snapVol_; }

  // The current Setting item's next value as text for setScalarText; false at the end of its range.
  bool stepText(const Config& c, int dir, char* out, size_t n) const;
  // The same for any table row: the next choice, or one step of the range clamped (ZERO_OFF: 0 sits below min).
  static bool stepSetting(const Config& c, const SettingDesc& d, int dir, char* out, size_t n);
  // The Volume item: one step of stepPct, clamped to 0..100; false at the end.
  static bool stepVolume(uint8_t cur, uint8_t stepPct, int dir, uint8_t& out);

  // Screen texts: the label without a trailing unit ("Screen brightness"), the value ("OFF", "80 %", "60"; an action: what OK does next).
  void label(char* out, size_t n) const;
  void valueText(const Config& c, uint8_t volumePct, char* out, size_t n) const;

 private:
  static void unitOf(const char* label, char* unit, size_t n);   // "Screen brightness (%)" -> "%", else ""
  static void formatNum(const SettingDesc& d, double x, char* out, size_t n);
  void add(MenuKind k, const SettingDesc* d, uint8_t order, const char* label);
  MenuItem items_[MENU_MAX_ITEMS];
  char     snap_[MENU_MAX_ITEMS][MENU_TEXT];
  uint8_t  n_ = 0, i_ = 0, snapVol_ = 60;
  bool     pairWipe_ = false, armed_ = false;
};

}  // namespace sb
