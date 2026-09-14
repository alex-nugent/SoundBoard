// The press pipeline (FirmwareSpec.md §5.2, §5.3 steps 1 and 7, §5.4): one
// record per press, the steps in order, the repeat timer. The physical outputs
// (screen, audio, the level controller; keys, jacks and the motor by phase)
// belong to the host; the pipeline decides what happens and when, and every
// output stays tied to the pressId that started it.
#pragma once
#include <stdint.h>
#include "config/config.h"

struct PressHost {
  virtual ~PressHost() {}
  virtual const sb::Config& pressConfig() const = 0;
  virtual uint8_t pressLevel() const = 0;                                       // current level index
  virtual bool    padHeld(uint8_t pos) const = 0;                               // still pressed (physical or simulated)
  virtual bool    audioPlaying() const = 0;
  virtual bool    muted() const = 0;
  virtual void    pressLabel(const sb::Entry& e, uint32_t now) = 0;             // step 5
  virtual void    playSound(const char* sound, uint8_t volumePct, uint16_t pressId, uint32_t now, bool repeat) = 0;   // step 6
  virtual void    runAction(const sb::Entry& e, uint16_t pressId, uint32_t now) = 0;   // §5.4, instead of step 6
  virtual void    levelPad(uint16_t pressId, uint32_t now, bool repeat) = 0;    // §5.3 step 1
  virtual void    jackPress(uint8_t pos, const sb::Entry& e, uint16_t pressId, uint32_t now, bool repeat) = 0;   // step 3 (a repeat re-pulses in pulse mode only)
  virtual void    jackRelease(uint8_t pos, uint16_t pressId) = 0;               // PadUp: opens that press's followed jack
  virtual void    jackStuck(uint8_t pos) = 0;                                   // PadStuck: opens a followed jack
  virtual void    confirmPulse(const sb::Entry& e, uint32_t now) = 0;          // step 4
  virtual void    keyPress(const sb::Entry& e, uint16_t pressId, uint32_t now) = 0;   // step 2 (§8.3)
  virtual void    keyRelease(uint16_t pressId, uint32_t now) = 0;              // PadUp: that press's held key
  virtual void    keyStuck(uint32_t now) = 0;                                  // PadStuck: releases a held key
};

class PressPipeline {
 public:
  void begin(PressHost* h) { h_ = h; }
  void onPadDown(uint8_t pos, uint16_t pressId, uint32_t now);
  void onPadUp(uint8_t pos, uint16_t pressId, uint32_t now);
  void onPadStuck(uint8_t pos, uint32_t now);
  void onAudioDone(uint16_t requestId, bool interrupted, uint32_t now);         // step 7: arms the repeat
  void onLevelChanged();          // §5.2: a level change ends a sound pad's repeat (the level pad's own repeat survives)
  void cancel();                  // level list edited, or leaving ACTIVE/DIMMED/SETUP
  void tick(uint32_t now);

  uint16_t current() const { return p_.id; }
  bool     repeatArmed() const { return p_.repeatAt != 0; }
  uint16_t repeats() const { return p_.repeats; }

 private:
  struct Press {
    uint16_t id = 0;
    uint8_t  pos = 0;
    bool     held = false, levelPad = false, hasSound = false;
    char     sound[41] = "";
    uint8_t  volumePct = 100;
    uint32_t repeatAt = 0;
    uint16_t repeats = 0;
  };
  void armRepeat(uint32_t now);
  PressHost* h_ = nullptr;
  Press      p_;
};
