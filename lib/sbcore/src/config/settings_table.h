// The settings descriptor table (FirmwareSpec.md §13.5): one row per scalar
// setting, the single source of truth for load, save, portal, menu, console.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace sb {

enum class SType : uint8_t { Bool, U8, U16, U32, F32, Enum, String };
enum SFlags : uint8_t {
  PORTAL   = 1,    // shown on the settings page
  MENU     = 2,    // a Quick Menu item (with menuOrder)
  LIVE     = 4,    // applies without a reboot
  ADVANCED = 8,    // collapsed by default on the page
  REBOOT   = 16,   // needs a restart
  ZERO_OFF = 32,   // 0 is a valid "off / never / unlimited" value outside min..max
};

struct SettingDesc {
  const char* path;        // "power.sleepAfterMin"
  SType       type;
  size_t      offset;      // offsetof(Config, ...): the table works on any Config instance
  uint16_t    size;        // sizeof the field (String: buffer size including the terminator)
  float       min, max, step;   // numbers: range; String: length range
  const char* def;         // default as text, parsed by type
  const char* enums;       // "deep|light" for Enum
  const char* choices;     // Quick Menu value list, e.g. "0|10|20|30|45|60|120"; null = min..max by step
  const char* label;       // "Sleep after (minutes)"
  const char* group;       // "Power"
  uint8_t     flags;
  uint8_t     menuOrder;   // position in the Quick Menu, 0 = not in the menu
};

extern const SettingDesc SETTINGS[];
extern const size_t      N_SETTINGS;

const SettingDesc* findSetting(const char* path);
int  enumIndex(const char* enums, const char* text);                 // -1 if not in the list (case-insensitive)
bool enumName(const char* enums, int index, char* out, size_t n);    // false if index out of range
int  enumCount(const char* enums);

}  // namespace sb
