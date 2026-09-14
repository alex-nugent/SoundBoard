// The settings descriptor table (FirmwareSpec.md §13.5). Every scalar of the
// §13.3 schema is one row here; structured data (levels, padChannels, roles,
// padPressPct, ownerLabel, cues, vibration.pattern) has its own loader/saver
// in loader.cpp / levels.cpp.
//
// Adding a setting (§19.9): add the field to Config, add one row here.
#include "config/settings_table.h"
#include "config/config.h"
#include "util/strutil.h"
#include <string.h>

namespace sb {

#define FIELD(f) offsetof(Config, f), (uint16_t)sizeof(((Config*)0)->f)

// Flag shorthands used below.
static constexpr uint8_t P   = PORTAL;
static constexpr uint8_t PL  = PORTAL | LIVE;
static constexpr uint8_t PLA = PORTAL | LIVE | ADVANCED;
static constexpr uint8_t PA  = PORTAL | ADVANCED;
static constexpr uint8_t PLM = PORTAL | LIVE | MENU;

const SettingDesc SETTINGS[] = {
  // path                              type            field                           min    max     step  default          enums                                   choices                      label                              group          flags            menu
  { "device.name",                     SType::String,  FIELD(device.name),             1,     24,     1,    "SoundBoard V4", nullptr,                                nullptr,                     "Board name",                      "Device",      P | REBOOT,      0 },
  { "device.ownerLabelMs",             SType::U16,     FIELD(device.ownerLabelMs),     0,     10000,  500,  "2000",          nullptr,                                nullptr,                     "Owner label time (ms)",           "Power",       PL,              0 },

  { "hardware.revision",               SType::Enum,    FIELD(hardware.revision),       0,     1,      1,    "A",             "A|B",                                  nullptr,                     "Board revision",                  "Device",      PA,              0 },

  { "touch.pressPct",                  SType::F32,     FIELD(touch.pressPct),          1.0f,  10.0f,  0.25f,"3.0",           nullptr,                                nullptr,                     "Press threshold (%)",             "Touch",       PLA,             0 },
  { "touch.releasePct",                SType::F32,     FIELD(touch.releasePct),        0.5f,  10.0f,  0.25f,"1.5",           nullptr,                                nullptr,                     "Release threshold (%)",           "Touch",       PLA,             0 },
  { "touch.separationPct",             SType::F32,     FIELD(touch.separationPct),     0.0f,  5.0f,   0.25f,"1.0",           nullptr,                                nullptr,                     "Separation margin (%)",           "Touch",       PLA,             0 },
  { "touch.shield",                    SType::Enum,    FIELD(touch.shield),            0,     1,      1,    "driven",        "driven|floating",                      nullptr,                     "Shield mode",                     "Touch",       PA | REBOOT,     0 },
  { "touch.shieldDrive",               SType::U8,      FIELD(touch.shieldDrive),       0,     7,      1,    "3",             nullptr,                                nullptr,                     "Shield drive strength",           "Touch",       PA | REBOOT,     0 },
  { "touch.stuckAfterMs",              SType::U32,     FIELD(touch.stuckAfterMs),      5000,  120000, 1000, "30000",         nullptr,                                nullptr,                     "Stuck pad after (ms, 0 = never)", "Touch",       PLA | ZERO_OFF,  0 },

  { "press.interrupt",                 SType::Bool,    FIELD(press.interrupt),         0,     1,      1,    "true",          nullptr,                                nullptr,                     "New press stops the sound",       "Buttons",     PL,              0 },
  { "press.repeatWhileHeld",           SType::Bool,    FIELD(press.repeatWhileHeld),   0,     1,      1,    "false",         nullptr,                                nullptr,                     "Repeat sound while held",         "Buttons",     PL,              0 },
  { "press.repeatDelayMs",             SType::U16,     FIELD(press.repeatDelayMs),     100,   5000,   50,   "500",           nullptr,                                nullptr,                     "Repeat delay (ms)",               "Buttons",     PL,              0 },
  { "press.levelRepeatWhileHeld",      SType::Bool,    FIELD(press.levelRepeatWhileHeld), 0,  1,      1,    "false",         nullptr,                                nullptr,                     "Repeat level change while held",  "Buttons",     PL,              0 },

  { "levelChange.returnToFirstAfterS", SType::U16,     FIELD(levelChange.returnToFirstAfterS), 5, 600, 5,   "20",            nullptr,                                nullptr,                     "Return to level 1 after (s, 0 = never)", "Buttons", PL | ZERO_OFF, 0 },
  { "levelChange.returnCue",           SType::Bool,    FIELD(levelChange.returnCue),   0,     1,      1,    "true",          nullptr,                                nullptr,                     "Cue on return to level 1",        "Buttons",     PL,              0 },
  { "levelChange.click",               SType::Bool,    FIELD(levelChange.click),       0,     1,      1,    "true",          nullptr,                                nullptr,                     "Click on level change",           "Buttons",     PL,              0 },
  { "levelChange.attendantHoldMs",     SType::U16,     FIELD(levelChange.attendantHoldMs), 300, 3000, 100,  "1000",          nullptr,                                nullptr,                     "Attendant hold for level (ms)",   "Buttons",     PL,              0 },
  { "levelChange.attendantCues",       SType::Bool,    FIELD(levelChange.attendantCues), 0,   1,      1,    "true",          nullptr,                                nullptr,                     "Cues on attendant level change",  "Buttons",     PL,              0 },
  { "levelChange.bothTapMs",           SType::U16,     FIELD(levelChange.bothTapMs),   200,   800,    50,   "400",           nullptr,                                nullptr,                     "Both-button tap for level 1 (ms)", "Buttons",    PL,              0 },
  { "levelChange.vibration.mode",      SType::Enum,    FIELD(levelChange.vibration.mode), 0,  2,      1,    "count",         "count|pattern|none",                   nullptr,                     "Level cue vibration",             "Vibration",   PL,              0 },
  { "levelChange.vibration.pulseMs",   SType::U16,     FIELD(levelChange.vibration.pulseMs), 50, 1000, 50,  "150",           nullptr,                                nullptr,                     "Level cue pulse (ms)",            "Vibration",   PL,              0 },
  { "levelChange.vibration.gapMs",     SType::U16,     FIELD(levelChange.vibration.gapMs), 50, 1000,  50,   "150",           nullptr,                                nullptr,                     "Level cue gap (ms)",              "Vibration",   PL,              0 },

  { "audio.volumePct",                 SType::U8,      FIELD(audio.volumePct),         0,     100,    10,   "60",            nullptr,                                nullptr,                     "Initial volume (%)",              "Audio",       PL,              0 },
  { "audio.stepPct",                   SType::U8,      FIELD(audio.stepPct),           5,     25,     5,    "10",            nullptr,                                nullptr,                     "Volume step (%)",                 "Audio",       PL,              0 },
  { "audio.maxGain",                   SType::F32,     FIELD(audio.maxGain),           0.10f, 1.00f,  0.05f,"0.5",           nullptr,                                nullptr,                     "Max gain (headroom)",             "Audio",       PLA,             0 },
  { "audio.clickVolume",               SType::U8,      FIELD(audio.clickVolume),       0,     5,      1,    "3",             nullptr,                                nullptr,                     "Click volume (0-5)",              "Audio",       PL,              0 },
  { "audio.outputs.speakers",          SType::Bool,    FIELD(audio.outputs.speakers),  0,     1,      1,    "false",         nullptr,                                nullptr,                     "On-board speakers",               "Audio",       PLM,             1 },
  { "audio.startupCue",                SType::Bool,    FIELD(audio.startupCue),        0,     1,      1,    "false",         nullptr,                                nullptr,                     "Start-up sound",                  "Audio",       PL,              0 },
  { "audio.cacheMaxMB",                SType::U8,      FIELD(audio.cacheMaxMB),        1,     7,      1,    "6",             nullptr,                                nullptr,                     "Sound cache (MB)",                "Audio",       PLA,             0 },

  { "bluetoothSpeaker.enabled",        SType::Bool,    FIELD(bluetoothSpeaker.enabled), 0,    1,      1,    "false",         nullptr,                                nullptr,                     "Bluetooth speaker",               "Bluetooth speaker", PLM,       3 },

  { "keyboard.enabled",                SType::Bool,    FIELD(keyboard.enabled),        0,     1,      1,    "true",          nullptr,                                nullptr,                     "Bluetooth typing",                "Keyboard",    PLM,             5 },
  { "keyboard.typeDelayMs",            SType::U8,      FIELD(keyboard.typeDelayMs),    5,     50,     1,    "12",            nullptr,                                nullptr,                     "Type delay per key (ms)",         "Keyboard",    PL,              0 },
  { "keyboard.holdKeysMaxMs",          SType::U16,     FIELD(keyboard.holdKeysMaxMs),  500,   10000,  500,  "3000",          nullptr,                                nullptr,                     "Held key limit (ms)",             "Keyboard",    PL,              0 },

  { "vibration.enabled",               SType::Bool,    FIELD(vibration.enabled),       0,     1,      1,    "true",          nullptr,                                nullptr,                     "Buzz",                            "Vibration",   PLM,             6 },
  { "vibration.strengthPct",           SType::U8,      FIELD(vibration.strengthPct),   20,    100,    10,   "100",           nullptr,                                nullptr,                     "Strength (%)",                    "Vibration",   PL,              0 },
  { "vibration.confirmPulse",          SType::Bool,    FIELD(vibration.confirmPulse),  0,     1,      1,    "false",         nullptr,                                nullptr,                     "Pulse on every press",            "Vibration",   PL,              0 },
  { "vibration.confirmPulseMs",        SType::U16,     FIELD(vibration.confirmPulseMs), 50,   1000,   50,   "200",           nullptr,                                nullptr,                     "Confirmation pulse (ms)",         "Vibration",   PL,              0 },
  { "vibration.maxPatternMs",          SType::U16,     FIELD(vibration.maxPatternMs),  500,   10000,  500,  "4000",          nullptr,                                nullptr,                     "Max pattern length (ms)",         "Vibration",   PL,              0 },

  { "jacks.mode",                      SType::Enum,    FIELD(jacks.mode),              0,     2,      1,    "follow",        "follow|pulse|off",                     nullptr,                     "Jack mode",                       "Jacks",       PLA,             0 },
  { "jacks.pulseMs",                   SType::U16,     FIELD(jacks.pulseMs),           100,   5000,   100,  "500",           nullptr,                                nullptr,                     "Jack pulse (ms)",                 "Jacks",       PLA,             0 },
  { "jacks.maxFollowMs",               SType::U16,     FIELD(jacks.maxFollowMs),       1000,  60000,  1000, "10000",         nullptr,                                nullptr,                     "Follow limit (ms, 0 = unlimited)", "Jacks",      PLA | ZERO_OFF,  0 },
  { "jacks.levelPad",                  SType::Bool,    FIELD(jacks.levelPad),          0,     1,      1,    "false",         nullptr,                                nullptr,                     "Level pad closes its jack",       "Jacks",       PLA,             0 },

  { "display.theme",                   SType::Enum,    FIELD(display.theme),           0,     5,      1,    "amber",         "amber|yellow|white|green|cyan|red",    nullptr,                     "Screen colour",                   "Display",     PL,              0 },
  { "display.flip",                    SType::Bool,    FIELD(display.flip),            0,     1,      1,    "false",         nullptr,                                nullptr,                     "Flip the screen",                 "Display",     PL,              0 },
  { "display.brightnessPct",           SType::U8,      FIELD(display.brightnessPct),   10,    100,    10,   "80",            nullptr,                                "10|20|30|40|50|60|70|80|90|100", "Screen brightness (%)",      "Display",     PLM,             7 },
  { "display.dimPct",                  SType::U8,      FIELD(display.dimPct),          0,     50,     5,    "10",            nullptr,                                nullptr,                     "Dimmed brightness (%)",           "Display",     PL,              0 },
  { "display.dimAfterS",               SType::U16,     FIELD(display.dimAfterS),       5,     600,    5,    "30",            nullptr,                                nullptr,                     "Dim after (s, 0 = never)",        "Display",     PL | ZERO_OFF,   0 },
  { "display.showLabels",              SType::Bool,    FIELD(display.showLabels),      0,     1,      1,    "true",          nullptr,                                nullptr,                     "Show labels",                     "Display",     PL,              0 },
  { "display.pressLabelMs",            SType::U16,     FIELD(display.pressLabelMs),    200,   5000,   100,  "0",             nullptr,                                nullptr,                     "Press label time (ms, 0 = off)",  "Display",     PL | ZERO_OFF,   0 },
  { "display.volumePopupMs",           SType::U16,     FIELD(display.volumePopupMs),   500,   5000,   100,  "1500",          nullptr,                                nullptr,                     "Volume popup time (ms)",          "Display",     PL,              0 },

  { "power.sleepAfterMin",             SType::U8,      FIELD(power.sleepAfterMin),     1,     120,    1,    "5",             nullptr,                                "0|1|2|3|5|10|15|30|60|120", "Sleep after (minutes, 0 = never)", "Power",      PL | ZERO_OFF,   0 },
  { "power.sleepMode",                 SType::Enum,    FIELD(power.sleepMode),         0,     1,      1,    "deep",          "deep|light",                           nullptr,                     "Sleep mode",                      "Power",       PA,              0 },
  { "power.offHoldMs",                 SType::U16,     FIELD(power.offHoldMs),         500,   3000,   100,  "1000",          nullptr,                                nullptr,                     "Hold both for Off (ms)",          "Power",       PL,              0 },
  { "power.wakeHoldMs",                SType::U16,     FIELD(power.wakeHoldMs),        100,   1000,   50,   "400",           nullptr,                                nullptr,                     "Wake hold (ms)",                  "Power",       PL,              0 },
  { "power.offReturnS",                SType::U16,     FIELD(power.offReturnS),        15,    600,    5,    "60",            nullptr,                                nullptr,                     "Return to Off after (s)",         "Power",       PL,              0 },
  { "power.lowBatteryWarnPct",         SType::U8,      FIELD(power.lowBatteryWarnPct), 5,     50,     5,    "15",            nullptr,                                nullptr,                     "Low battery warning (%)",         "Power",       PL,              0 },
  { "power.shutdownPct",               SType::U8,      FIELD(power.shutdownPct),       0,     20,     1,    "5",             nullptr,                                nullptr,                     "Shutdown at (%, 0 = never)",      "Power",       PL,              0 },
  { "power.showChargingWhenOff",       SType::Bool,    FIELD(power.showChargingWhenOff), 0,   1,      1,    "true",          nullptr,                                nullptr,                     "Show charging when Off",          "Power",       PL,              0 },
  { "power.chargeCheckMin",            SType::U8,      FIELD(power.chargeCheckMin),    1,     60,     1,    "10",            nullptr,                                nullptr,                     "Charge check every (min)",        "Power",       PL,              0 },

  { "menu.enabled",                    SType::Bool,    FIELD(menu.enabled),            0,     1,      1,    "true",          nullptr,                                nullptr,                     "Quick Menu enabled",              "Buttons",     PL,              0 },
  { "menu.holdMs",                     SType::U16,     FIELD(menu.holdMs),             1000,  10000,  500,  "3000",          nullptr,                                nullptr,                     "Hold both for the menu (ms)",     "Buttons",     PL,              0 },
  { "menu.timeoutS",                   SType::U16,     FIELD(menu.timeoutS),           10,    300,    10,   "30",            nullptr,                                nullptr,                     "Menu timeout (s)",                "Buttons",     PL,              0 },

  { "setup.password",                  SType::String,  FIELD(setup.password),          8,     24,     1,    "soundboard",    nullptr,                                nullptr,                     "Setup Wi-Fi password",            "Device",      P,               0 },
  { "setup.idleOffMin",                SType::U8,      FIELD(setup.idleOffMin),        2,     60,     1,    "10",            nullptr,                                nullptr,                     "Setup idle timeout (min)",        "Device",      PL,              0 },
  { "setup.pauseKeyboard",             SType::Bool,    FIELD(setup.pauseKeyboard),     0,     1,      1,    "false",         nullptr,                                nullptr,                     "Pause typing during setup",       "Device",      PLA,             0 },

  { "wifi.ssid",                       SType::String,  FIELD(wifi.ssid),               0,     32,     1,    "",              nullptr,                                nullptr,                     "Home Wi-Fi network",              "Firmware",    P,               0 },
  { "wifi.password",                   SType::String,  FIELD(wifi.password),           0,     64,     1,    "",              nullptr,                                nullptr,                     "Home Wi-Fi password",             "Firmware",    P,               0 },
  { "update.repo",                     SType::String,  FIELD(update.repo),             0,     64,     1,    "alexnugent/SoundBoardV4", nullptr,                      nullptr,                     "Update repository",               "Firmware",    PA,              0 },   // TODO(OPEN-14)
  { "update.channel",                  SType::String,  FIELD(update.channel),          1,     24,     1,    "latest",        nullptr,                                nullptr,                     "Update channel",                  "Firmware",    PA,              0 },

  { "diag.logToCard",                  SType::Bool,    FIELD(diag.logToCard),          0,     1,      1,    "false",         nullptr,                                nullptr,                     "Log to the card",                 "Diagnostics", PLA,             0 },
  { "diag.logLevel",                   SType::Enum,    FIELD(diag.logLevel),           0,     3,      1,    "info",          "error|warn|info|debug",                nullptr,                     "Log level",                       "Diagnostics", PLA,             0 },
};

const size_t N_SETTINGS = sizeof(SETTINGS) / sizeof(SETTINGS[0]);

const SettingDesc* findSetting(const char* path) {
  if (!path) return nullptr;
  for (size_t i = 0; i < N_SETTINGS; i++)
    if (strcmp(SETTINGS[i].path, path) == 0) return &SETTINGS[i];
  return nullptr;
}

int enumIndex(const char* enums, const char* text) {
  if (!enums || !text) return -1;
  int idx = 0;
  const char* p = enums;
  while (*p) {
    const char* end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (strlen(text) == len) {
      bool same = true;
      for (size_t i = 0; i < len; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)text[i])) { same = false; break; }
      if (same) return idx;
    }
    if (!end) break;
    p = end + 1; idx++;
  }
  return -1;
}

bool enumName(const char* enums, int index, char* out, size_t n) {
  if (!enums || index < 0 || n == 0) return false;
  const char* p = enums;
  for (int i = 0; i < index; i++) {
    const char* end = strchr(p, '|');
    if (!end) return false;
    p = end + 1;
  }
  const char* end = strchr(p, '|');
  size_t len = end ? (size_t)(end - p) : strlen(p);
  if (len >= n) len = n - 1;
  memcpy(out, p, len); out[len] = 0;
  return true;
}

int enumCount(const char* enums) {
  if (!enums || !*enums) return 0;
  int c = 1;
  for (const char* p = enums; *p; p++) if (*p == '|') c++;
  return c;
}

}  // namespace sb
