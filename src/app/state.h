// The app state machine (FirmwareSpec.md §3, §19.2) and the boot sequence
// (§17). Phase 1: BOOT -> ACTIVE with the normal view; FAULT when the app
// itself cannot run. Phase 2: inputs (pads, buttons, button commands), the
// DIMMED state with its dim timer, the overlays of §11.5. Phase 3: the audio
// engine. Phase 4: levels, actions, mute and the complete press pipeline
// (PressHost is implemented here). Power and the rest arrive by phase.
#pragma once
#include <stdint.h>
#include <Print.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include "config/config.h"
#include "config/report.h"
#include "config/store.h"
#include "display/display.h"
#include "display/screens/screens.h"
#include "input/touch.h"
#include "input/buttons.h"
#include "input/button_commands.h"
#include "audio/engine.h"
#include "audio/cache.h"
#include "audio/volume.h"
#include "bt/kcx.h"
#include "util/event.h"
#include "app/press.h"
#include "power/battery.h"
#include "app/haptics.h"
#include "app/jacks.h"
#include "ble/keyboard.h"
#include "app/levels.h"

enum class BootKind : uint8_t { Cold, SleepWake, OffWake };

struct BootInfo {
  esp_reset_reason_t       reset = ESP_RST_UNKNOWN;
  esp_sleep_wakeup_cause_t wake = ESP_SLEEP_WAKEUP_UNDEFINED;
  BootKind kind = BootKind::Cold;
  bool     rtcValid = false;
  bool     bothButtonsAtReset = false;   // recovery check (§17.1 step 4)
  uint32_t appStartMs = 0;               // millis() at the top of setup()
  uint32_t serialWaitMs = 0;
  uint8_t  wakeTouchGpio = 0;            // a touch wake: the waking channel's GPIO (§4.1 latched wake press), 0 = not identified
  uint32_t touchActiveMask = 0;          // the sensor's active-channel mask at app start (bit n = touch channel n)
  uint8_t  wakeButtons = 0;              // an ext1 wake: bit 0 minus, bit 1 plus
};

enum class AppMode : uint8_t { Boot, Active, Dimmed, Fault };

class AppState : private PressHost {
 public:
  void audioEarlyStart();                  // §17.1 step 9: UART + 5 V rail on their way while the rest boots (call before begin())
  void begin(const BootInfo& bi);
  void tick();

  // Console access.
  sb::Config&        config() { return cfg_; }
  sb::ConfigReport&  report() { return rep_; }
  ConfigStore&       store()  { return store_; }
  Display&           display() { return display_; }
  TouchInput&        touch() { return touch_; }
  AudioEngine&       audio() { return audio_; }
  SoundCache&        cache() { return cache_; }
  KcxLink&           kcx() { return kcx_; }
  const BootInfo&    bootInfo() const { return bi_; }
  AppMode            mode() const { return mode_; }
  const char*        modeName() const;
  void               printStatus(Print& out);
  void               registerInput(uint32_t now);      // §3.1 "input": restarts the timeouts, leaves DIMMED

  bool setSetting(const char* path, const char* value, char* err, size_t errLen);
  bool mergeSettings(const char* json, char* err, size_t errLen);
  bool requestSave(bool userAction, char* err, size_t errLen);   // through Storage::deferredWrite (§19.7 rule 11)
  bool factory(char* err, size_t errLen);
  void onConfigChanged();

  // Console: `1`-`4 [ms]` (full presses, §5.2/§5.3), `+`/`-`, `++`, `h+`/`h-`, `l`, `c`.
  void simulatePress(uint8_t pos, uint32_t ms);
  void simulateClick(int dir);
  void simulateCommand(sb::ButtonCmd c);
  void setLiveDeltas(bool on) { liveDeltas_ = on; }
  bool liveDeltas() const { return liveDeltas_; }
  void recalibrate();
  void playFile(const char* name);         // bench: play a card file (or its missing tone) as if a pad had asked
  bool toggleSpeakers();                   // console `b`: audio.outputs.speakers in RAM
  void buzzTest();                         // console `v`: the current level's pattern at the current strength (§9.5)
  void jackTest(uint8_t jack);             // console `j1`..`j4`: close for 1 s
  bool toggleBluetooth();                  // console `bt`: bluetoothSpeaker.enabled in RAM, rail cycle / AT+POWER_OFF (§7.1)
  bool startPairing(bool wipe);            // console `p` / `pairwipe`, later the menu and portal (§7.3): AT+PAIR, 60 s; wipe = AT+DELVMLINK first
  bool toggleKeyboard();                   // console `kbd`: keyboard.enabled in RAM (§8.4)
  BleKeyboard& keyboard() { return kbd_; }

  uint16_t faults() const { return faults_; }
  void setFault(uint16_t bit, bool on);
  void message(const char* text, uint32_t ms);
  void enterFault(const char* reason);
  bool cardRecover(uint32_t offMs);            // §18: unmount, gated rail off for offMs, screen re-init, remount
  bool cardWriteTest(bool psramBuffer, uint32_t chunk, Print& out);   // bench: write/verify/delete a 16 KB file in `chunk`-byte writes
  bool checkCard();                            // probes the card after an I/O error; clears card_ and raises !CARD when it is gone
  bool cardRawTest(uint32_t hz, bool crcOn, bool libSeq, bool lowRegion, bool dark, int nSectors, Print& out);   // bench: raw CMD24/CMD17 over n sectors, restored afterwards
  // Phase 5 (§12.3): never return. Console `sleep`, `off`; the timers of §3.2; `batt`.
  void enterSleep(const char* why);
  void enterOff(const char* why);
  Battery& battery() { return battery_; }

 private:
  void enterActive(uint32_t now);
  void enterDimmed(uint32_t now);
  void refreshView(uint8_t regions);
  void tick1s(uint32_t now);
  void refreshRtcEarly();
  static void saveThunk(void* arg);
  sb::ButtonDurations durations() const;
  void handleEvent(const Event& e, uint32_t now);
  void onButtonCommand(sb::ButtonCmd c, uint32_t now);
  sb::LevelPolicy levelPolicy() const;
  void applyLevel(const sb::LevelChange& c, const char* why, uint32_t now);   // §5.3: screen, flash, click, RTC copy
  void toggleMute(uint32_t now);                                             // §5.4 Mute
  void stepVolume(int dir, uint32_t now, bool confirmClick);
  void updateHoldBar(uint32_t now);
  void tickOverlays(uint32_t now);
  // PressHost (§5.2 steps 5-7, §5.3 step 1, §5.4)
  const sb::Config& pressConfig() const override { return cfg_; }
  uint8_t pressLevel() const override { return level_; }
  bool    padHeld(uint8_t pos) const override;
  bool    audioPlaying() const override { return audio_.playing(); }
  bool    muted() const override { return volume_.muted(); }
  void    pressLabel(const sb::Entry& e, uint32_t now) override;
  void jackPress(uint8_t pos, const sb::Entry& e, uint16_t pressId, uint32_t now, bool repeat) override;
  void jackRelease(uint8_t pos, uint16_t pressId) override;
  void jackStuck(uint8_t pos) override;
  void confirmPulse(const sb::Entry& e, uint32_t now) override;
  void keyPress(const sb::Entry& e, uint16_t pressId, uint32_t now) override;
  void keyRelease(uint16_t pressId, uint32_t now) override;
  void keyStuck(uint32_t now) override;
  void linkMessage(const char* text, uint32_t ms, uint32_t now);   // §11.4 bottom line, priority 3
  void tickBluetooth(uint32_t now);                                // §7: link edges, pairing overlay, a deferred rail cycle
  void updateStateWord(uint32_t now);                              // §11.4 bottom line, priority 4
  void    playSound(const char* sound, uint8_t volumePct, uint16_t pressId, uint32_t now, bool repeat) override;
  void    runAction(const sb::Entry& e, uint16_t pressId, uint32_t now) override;
  void    levelPad(uint16_t pressId, uint32_t now, bool repeat) override;
  void playCue(const char* file, sb::ToneKind fallback, float gain);
  void ampEnable(bool on);
  void onRailReady(uint32_t flags, uint32_t now);
  void loadVolume();
  static void volumeWriteThunk(void* arg);
  static bool audioIdleThunk();
  // Phase 5
  bool sleepAllowed() const;
  void fillRtcForSleep(uint8_t kind);
  void quiesce();
  void powerDownForSleep();
  void tickPower(uint32_t now);
  void onBatterySample(uint32_t now);

  BootInfo         bi_;
  AppMode          mode_ = AppMode::Boot;
  sb::Config       cfg_;
  sb::ConfigReport rep_;
  ConfigStore      store_;
  Display          display_;
  NormalView       view_;
  NormalScreen     normalScreen_;
  BootScreen       bootScreen_;
  FaultScreen      faultScreen_;
  TouchInput       touch_;
  Buttons          buttons_;
  sb::ButtonCommands cmds_;
  sb::HoldProgress hold_;
  SoundCache       cache_;
  AudioEngine      audio_;
  KcxLink          kcx_;
  sb::Volume       volume_;
  sb::LevelController levels_;
  PressPipeline    press_;
  Battery          battery_;
  Haptics          haptics_;
  Jacks            jacks_;
  BleKeyboard      kbd_;
  bool             kbdInitPending_ = false;
  uint32_t         linkUntil_ = 0;
  bool             kbdWasReady_ = false;
  // §7: the Bluetooth speaker as the app sees it.
  bool             btWasLinked_ = false, btCyclePending_ = false, btRecycled_ = false;   // btRecycled_: the one rail cycle allowed for a silent module at boot
  uint32_t         btNotLinkedSince_ = 0;      // enabled and unlinked since (0 = linked or disabled); NO SPEAKER after 10 s (§7.5)
  uint32_t         pairTickAt_ = 0;            // next pairing-overlay refresh
  uint32_t         btProbeAt_ = 0;             // no banner at rail-up: AT+ sent, verdict due (a reset leaves the rail up, the module silent)
  MessageScreen    msgScreen_;
  bool             wokeFromOff_ = false, padSinceWake_ = false, lowWarned_ = false;
  uint32_t         emptyAt_ = 0;
  uint8_t          level_ = 0;                 // mirrors levels_.current()
  bool             audioStarted_ = false, railReady_ = false, startupCuePending_ = false, ampOn_ = false, saveUserAction_ = false;
  uint16_t         playingPress_ = 0;          // requestId of the entry sound in flight (0 = none)
  uint16_t         padDownReq_ = 0;            // the request whose latency padDownAt_ measures
  uint8_t          playingEntryVol_ = 100;
  uint32_t         padDownAt_ = 0, ampOffAt_ = 0, volPersistAt_ = 0, kcxPowerOffAt_ = 0, refHash_ = 0;
  uint16_t         faults_ = 0;
  bool             configLoaded_ = false;
  bool             recoveryRequested_ = false;
  bool             otaMarked_ = false;
  bool             liveDeltas_ = false;
  bool             hintShown_ = false;         // "keep hands off the buttons" is on the screen
  int8_t           simPos_ = -1;               // console-simulated press in progress
  uint16_t         simPressId_ = 0;
  uint32_t         simUntil_ = 0, simStart_ = 0;
  uint32_t         bootUntil_ = 0, recoveryCheckAt_ = 0, lastInputMs_ = 0, msgUntil_ = 0, next1s_ = 0;
  uint32_t         overlayUntil_ = 0, flashUntil_ = 0, levelChangeAt_ = 0;
  bool             lastSaveOk_ = false;
  char             lastSaveErr_[100] = { 0 };
  char             faultReason_[64] = { 0 };
};
