// Sample documents for the native configuration tests (FirmwareSpec.md §19.10).
#pragma once

// §13.3 example, comments included, plus an unknown key for the round-trip test.
static const char* SAMPLE_EXAMPLE = R"JSON({
  "schema": 1,
  "revision": 0,                        // incremented by every save
  "device": {
    "name": "SoundBoard V4",
    "ownerLabel": ["If found please call", "(phone number)"],
    "ownerLabelMs": 2000
  },
  "hardware": { "revision": "A", "padChannels": [2, 3, 4, 5] },
  "pads": { "roles": ["level", "sound", "sound", "sound"] },
  "touch": { "pressPct": 3.0, "releasePct": 1.5, "separationPct": 1.0, "padPressPct": null,
             "shield": "driven", "shieldDrive": 3, "stuckAfterMs": 30000 },
  "press": { "interrupt": true, "repeatWhileHeld": false, "repeatDelayMs": 500, "levelRepeatWhileHeld": false },
  "levelChange": { "returnToFirstAfterS": 20, "returnCue": true, "click": true, "attendantHoldMs": 1000,
                   "attendantCues": true, "bothTapMs": 400,
                   "vibration": { "mode": "count", "pulseMs": 150, "gapMs": 150, "pattern": [200, 100, 200] } },
  "audio": { "volumePct": 60, "stepPct": 10, "maxGain": 0.5, "clickVolume": 3, "outputs": { "speakers": false },
             "startupCue": false, "cues": { "startup": "_startup.wav", "click": "_click.wav", "saved": "_saved.wav", "lowBattery": "" },
             "cacheMaxMB": 6 },
  "bluetoothSpeaker": { "enabled": false, "waitOnWakeMs": 0 },
  "keyboard": { "enabled": true, "typeDelayMs": 12, "holdKeysMaxMs": 3000 },
  "vibration": { "enabled": true, "strengthPct": 100, "confirmPulse": false, "confirmPulseMs": 200, "maxPatternMs": 4000 },
  "jacks": { "mode": "follow", "pulseMs": 500, "maxFollowMs": 10000, "levelPad": false },
  "display": { "theme": "amber", "flip": false, "brightnessPct": 80, "dimPct": 10, "dimAfterS": 30,
               "showLabels": true, "pressLabelMs": 0, "volumePopupMs": 1500 },
  "power": { "sleepAfterMin": 5, "sleepMode": "deep", "offHoldMs": 1000, "wakeHoldMs": 400, "offReturnS": 60,
             "lowBatteryWarnPct": 15, "shutdownPct": 5, "showChargingWhenOff": true, "chargeCheckMin": 10 },
  "menu": { "enabled": true, "holdMs": 3000, "timeoutS": 30 },
  "setup": { "password": "soundboard", "idleOffMin": 10, "pauseKeyboard": false },
  "wifi": { "ssid": "", "password": "" },
  "update": { "repo": "alexnugent/SoundBoardV4", "channel": "latest" },
  "diag": { "logToCard": false, "logLevel": "info" },
  "custom": { "keep": "me" },
  "levels": [
    {
      "name": "ONE",
      "vibration": [150, 150, 150],
      "jacks": true,
      "buttons": [
        { "sound": "yes.wav", "label": "Yes", "type": "yes", "key": "", "keyMode": "type", "action": "none",
          "goToLevel": 1, "volumePct": 100, "vibrate": null, "jack": null },
        { "sound": "maybe.wav", "label": "Maybe", "type": "maybe" },
        { "sound": "no.wav",    "label": "No",    "type": "no" }
      ]
    }
  ]
})JSON";

// §13.6: Annalise's initial configuration.
static const char* SAMPLE_ANNALISE = R"JSON({
  "schema": 1,
  "device": { "name": "SoundBoard V4", "ownerLabel": ["If found please call", "(phone number)"], "ownerLabelMs": 2000 },
  "hardware": { "revision": "A", "padChannels": [2, 3, 4, 5] },
  "pads": { "roles": ["level", "sound", "sound", "sound"] },
  "press": { "interrupt": true, "repeatWhileHeld": false, "repeatDelayMs": 250, "levelRepeatWhileHeld": false },
  "levelChange": { "returnToFirstAfterS": 20, "returnCue": true, "click": true,
                   "vibration": { "mode": "count", "pulseMs": 150, "gapMs": 150 } },
  "audio": { "volumePct": 60, "clickVolume": 2,
             "outputs": { "speakers": false },
             "cues": { "startup": "_startup.wav", "click": "_click.wav", "saved": "_saved.wav" } },
  "bluetoothSpeaker": { "enabled": false },
  "keyboard": { "enabled": false },
  "vibration": { "enabled": true, "strengthPct": 100, "confirmPulse": false },
  "display": { "theme": "amber", "dimAfterS": 30, "pressLabelMs": 0, "showLabels": true },
  "power": { "sleepAfterMin": 5, "offHoldMs": 1000 },
  "levels": [
    { "name": "ONE",    "buttons": [ { "sound": "yes.wav",      "label": "Yes" },
                                     { "sound": "maybe.wav",    "label": "Maybe" },
                                     { "sound": "no.wav",       "label": "No" } ] },
    { "name": "TWO",    "buttons": [ { "sound": "choices.wav",  "label": "Choices" },
                                     { "sound": "letters.wav",  "label": "Letters" },
                                     { "sound": "numbers.wav",  "label": "Numbers" } ] },
    { "name": "VOLUME", "buttons": [ { "action": "volumeUp",   "label": "Louder" },
                                     { "action": "mute",       "label": "Mute" },
                                     { "action": "volumeDown", "label": "Quieter" } ] },
    { "name": "FOUR",   "buttons": [ { "sound": "hello.wav",    "label": "Hello" },
                                     { "sound": "thankyou.wav", "label": "Thank you" },
                                     { "sound": "goodbye.wav",  "label": "Goodbye" } ] }
  ]
})JSON";

// Out-of-range and wrong-type values that must clamp or fall back.
static const char* SAMPLE_OUT_OF_RANGE = R"JSON({
  "schema": 1,
  "touch": { "pressPct": 25, "releasePct": 0.1, "stuckAfterMs": 0 },
  "audio": { "volumePct": 150, "stepPct": 1, "maxGain": 2 },
  "display": { "theme": "purple", "brightnessPct": "bright", "dimAfterS": 0 },
  "power": { "sleepAfterMin": 999, "shutdownPct": 20, "lowBatteryWarnPct": 10 },
  "menu": { "holdMs": 1000 },
  "levels": [ { "name": "A", "buttons": [ {}, {}, {} ] } ]
})JSON";

// Three levels: the middle one has the wrong number of buttons, the last an
// unknown action; both drop, the first stays.
static const char* SAMPLE_BAD_LEVELS = R"JSON({
  "schema": 1,
  "levels": [
    { "name": "GOOD", "buttons": [ { "sound": "a.wav" }, { "sound": "b.wav" }, { "sound": "c.wav" } ] },
    { "name": "SHORT", "buttons": [ { "sound": "a.wav" }, { "sound": "b.wav" } ] },
    { "name": "BADACTION", "buttons": [ { "action": "launchRockets" }, { "sound": "b.wav" }, { "sound": "c.wav" } ] }
  ]
})JSON";

// A goToLevel pointing at level 3 of 3; the test removes level 3 and reloads.
static const char* SAMPLE_GOTO = R"JSON({
  "schema": 1,
  "levels": [
    { "name": "ONE",   "buttons": [ { "action": "goToLevel", "goToLevel": 3, "label": "Jump" }, { "sound": "b.wav" }, { "sound": "c.wav" } ] },
    { "name": "TWO",   "buttons": [ { "sound": "a.wav" }, { "sound": "b.wav" }, { "sound": "c.wav" } ] },
    { "name": "THREE", "buttons": [ { "sound": "a.wav" }, { "sound": "b.wav" }, { "sound": "c.wav" } ] }
  ]
})JSON";

static const char* SAMPLE_NEWER = R"JSON({ "schema": 2, "display": { "theme": "green" } })JSON";
