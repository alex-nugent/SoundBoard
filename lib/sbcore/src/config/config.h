// The typed configuration (FirmwareSpec.md §5.1, §13). Modules read settings
// only through `const Config&` (§19.7 rule 5); only config/ touches JSON.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace sb {

constexpr uint8_t SCHEMA_VERSION = 1;    // §13.1 rule 7
constexpr uint8_t MAX_LEVELS     = 20;   // Appendix E
constexpr uint8_t MAX_SOUND_PADS = 4;
constexpr uint8_t N_PADS         = 4;
constexpr uint8_t MAX_PATTERN    = 16;
constexpr uint8_t OWNER_LINES    = 3;

enum class Role     : uint8_t { Sound, Level, None };
enum class KeyMode  : uint8_t { Type, Hold, Tap };           // Type = type the text; Hold/Tap = the single key
enum class Action   : uint8_t { None, VolumeUp, VolumeDown, Mute, NextLevel, PreviousLevel, GoToLevel };
enum class JackMode : uint8_t { Inherit, Follow, Pulse, Off };
enum class Tri      : uint8_t { Inherit, On, Off };

// Enumerated scalars are stored as the index into their descriptor row's
// `enums` list ("amber|yellow|...") so the table stays the single source of
// truth; the named enums below give the code readable constants.
enum Theme     : uint8_t { THEME_AMBER, THEME_YELLOW, THEME_WHITE, THEME_GREEN, THEME_CYAN, THEME_RED };
enum ShieldMode: uint8_t { SHIELD_DRIVEN, SHIELD_FLOATING };
enum VibMode   : uint8_t { VIB_COUNT, VIB_PATTERN, VIB_NONE };
enum JacksMode : uint8_t { JACKS_FOLLOW, JACKS_PULSE, JACKS_OFF };
enum SleepMode : uint8_t { SLEEP_DEEP, SLEEP_LIGHT };
enum LogLevelCfg : uint8_t { LOGCFG_ERROR, LOGCFG_WARN, LOGCFG_INFO, LOGCFG_DEBUG };
enum HwRevision: uint8_t { REV_A, REV_B };

struct Entry {                       // one sound pad on one level
  char     sound[41];                // file in /sounds, "" = none
  char     label[25];                // shown on the screen; "" = derived from the sound name
  char     type[65];                 // text typed to the host; "" = nothing
  char     key[8];                   // single key name (Appendix B); "" = none
  KeyMode  keyMode;                  // default Type
  Action   action;                   // default None
  uint8_t  gotoLevel;                // 1-based, for GoToLevel
  uint8_t  volumePct;                // 0..100 multiplier on the master volume, default 100
  Tri      vibrate;                  // confirmation pulse for this entry
  JackMode jack;                     // this entry's jack behaviour
};

struct Level {
  char     name[17];                 // shown under the numeral; "" = none
  Entry    buttons[MAX_SOUND_PADS];  // exactly nSoundPads used
  bool     hasVibration;
  uint16_t vibration[MAX_PATTERN];   // optional override pattern (on, off, on ... ms)
  uint8_t  vibrationLen;
  bool     jacks;                    // false = no jack closures on this level (advanced)
};

struct LevelSet { Level levels[MAX_LEVELS]; uint8_t count; };

struct Config {
  uint8_t  schema;
  uint32_t revision;

  struct {
    char     name[25];
    char     ownerLabel[OWNER_LINES][21];
    uint8_t  ownerLines;
    uint16_t ownerLabelMs;
  } device;

  struct {
    uint8_t revision;                // HwRevision
    uint8_t padChannels[N_PADS];     // touch GPIO for positions P1..P4
  } hardware;

  struct { Role roles[N_PADS]; } pads;

  struct {
    float    pressPct, releasePct, separationPct;
    bool     hasPadPressPct;
    float    padPressPct[N_PADS];    // 0 = no override for that position
    uint8_t  shield;                 // ShieldMode
    uint8_t  shieldDrive;
    uint32_t stuckAfterMs;           // 0 = never
  } touch;

  struct {
    bool     interrupt;
    bool     repeatWhileHeld;
    uint16_t repeatDelayMs;
    bool     levelRepeatWhileHeld;
  } press;

  struct {
    uint16_t returnToFirstAfterS;    // 0 = never
    bool     returnCue;
    bool     click;
    uint16_t attendantHoldMs;
    bool     attendantCues;
    uint16_t bothTapMs;              // both buttons pressed and released within this = level 1
    struct {
      uint8_t  mode;                 // VibMode
      uint16_t pulseMs, gapMs;
      uint16_t pattern[MAX_PATTERN];
      uint8_t  patternLen;
    } vibration;
  } levelChange;

  struct {
    uint8_t volumePct, stepPct;
    float   maxGain;
    uint8_t clickVolume;
    struct { bool speakers; } outputs;
    bool    startupCue;
    struct { char startup[41], click[41], saved[41], lowBattery[41]; } cues;
    uint8_t cacheMaxMB;
  } audio;

  struct { bool enabled; uint16_t waitOnWakeMs; } bluetoothSpeaker;
  struct { bool enabled; uint8_t typeDelayMs; uint16_t holdKeysMaxMs; } keyboard;
  struct { bool enabled; uint8_t strengthPct; bool confirmPulse; uint16_t confirmPulseMs, maxPatternMs; } vibration;
  struct { uint8_t mode; uint16_t pulseMs; uint16_t maxFollowMs; bool levelPad; } jacks;   // mode: JacksMode; maxFollowMs 0 = unlimited

  struct {
    uint8_t  theme;                  // Theme
    bool     flip;
    uint8_t  brightnessPct, dimPct;
    uint16_t dimAfterS;              // 0 = never
    bool     showLabels;
    uint16_t pressLabelMs;           // 0 = off
    uint16_t volumePopupMs;
  } display;

  struct {
    uint8_t  sleepAfterMin;          // 0 = never
    uint8_t  sleepMode;              // SleepMode
    uint16_t offHoldMs, wakeHoldMs, offReturnS;
    uint8_t  lowBatteryWarnPct, shutdownPct;
    bool     showChargingWhenOff;
    uint8_t  chargeCheckMin;
  } power;

  struct { bool enabled; uint16_t holdMs, timeoutS; } menu;
  struct { char password[25]; uint8_t idleOffMin; bool pauseKeyboard; } setup;
  struct { char ssid[33]; char password[65]; } wifi;
  struct { char repo[65]; char channel[25]; } update;
  struct { bool logToCard; uint8_t logLevel; } diag;   // logLevel: LogLevelCfg

  LevelSet levels;
};

// Derived facts used everywhere.
uint8_t nSoundPads(const Config& c);            // number of "sound" roles
int     levelPadPos(const Config& c);           // position of the "level" pad, -1 if none
int     soundIndex(const Config& c, uint8_t pos);   // pad position -> entry index, -1 if not a sound pad
void    entryLabel(const Entry& e, char* out, size_t n);   // label, or the sound name without .wav

}  // namespace sb
