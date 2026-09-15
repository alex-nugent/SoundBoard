// Screens (FirmwareSpec.md §11.9): each has draw(display, regions). The
// display module clears the panel on a screen change and then asks the screen
// to draw its dirty regions, one bounded slice per tick.
#pragma once
#include <stdint.h>
#include "display/theme.h"

class Display;

struct Screen {
  virtual ~Screen() {}
  virtual void draw(Display& d, uint8_t regions) = 0;
};

// §11.6: owner label lines, version bottom-right; or the recovery prompt.
struct BootScreen : Screen {
  const char* lines[3] = { nullptr, nullptr, nullptr };
  uint8_t     nLines = 0;
  const char* version = "";
  bool        recoveryPrompt = false;
  bool        calibrationHint = false;    // "keep hands off the buttons" (Phase 2)
  void draw(Display& d, uint8_t regions) override;
};

// §11.4: what the normal view shows; filled by the app state.
struct NormalView {
  uint8_t  level = 0;             // 0-based
  uint8_t  levelCount = 1;
  char     name[17] = "";
  uint8_t  nSoundPads = 3;
  char     labels[4][25] = { "", "", "", "" };
  int8_t   pressedPad = -1;       // sound index drawn inverted
  bool     showLabels = true;
  bool     battValid = false;
  uint8_t  battPct = 0;
  bool     usb = false;
  bool     lowBattery = false;
  uint8_t  volumePct = 60;
  bool     muted = false;
  char     link[24] = "";         // §11.4 bottom line, priority 3: a link change in plain words, for a few seconds
  char     stateWord[24] = "";    // §11.4 bottom line, priority 4: one dim word while something is missing
  uint16_t faults = 0;
  char     message[40] = "";      // transient status message shown on the name row
  // Overlays (§11.5), owned by the app state which clears them on time.
  bool     flashNumeral = false;  // numeral drawn inverted (level change flash; level pad held)
  char     overlay[20] = "";      // text over the numeral at scale 3: volume popup ("VOL 60", "MUTE") or the press label
  bool     overlayBar = false;    // volume bar under the overlay text
  uint8_t  overlayPct = 0;
  uint8_t  holdKind = 0;          // countdown bar along the bottom: 0 none, 1 PREV LEVEL, 2 NEXT LEVEL, 3 both-hold
  uint8_t  holdPct = 0;
  bool     holdOffReached = false;
  uint8_t  holdSeconds = 0;
};

struct NormalScreen : Screen {
  const NormalView* view = nullptr;
  void draw(Display& d, uint8_t regions) override;
};

// §14.2: the Quick Menu; filled by the app from the menu model.
struct MenuView {
  uint8_t  index = 0, count = 1;     // 0-based item and the total, shown as "n / N"
  char     label[40] = "";           // the item, scale 2
  char     value[24] = "";           // the value line, scale 3 ("" for an action item)
  bool     action = false;           // an action item: the value line says how to run it, no arrows
  bool     canUp = true, canDown = true;   // the arrows dim at the ends of a range
  char     result[24] = "";          // an action's result, in place of the value line while set
  char     keys[4][8] = { "BACK", "DOWN", "UP", "NEXT" };   // one word above each pad, scale 2 ("" = that pad does nothing here); an action names itself: SAVE, CANCEL, PAIR, START
  bool     holdBar = false;          // both-hold: the exit bar along the bottom
  uint8_t  holdPct = 0;
  bool     holdOffReached = false;
};

struct MenuScreen : Screen {
  const MenuView* view = nullptr;
  void draw(Display& d, uint8_t regions) override;
};

// §15.2: the setup card (network name, password, address, phones connected; the recovery banner).
struct SetupView {
  char    ssid[33] = "";
  char    password[25] = "";
  char    url[28] = "";
  uint8_t clients = 0;
  bool    recovery = false;
};

struct SetupScreen : Screen {
  const SetupView* view = nullptr;
  void draw(Display& d, uint8_t regions) override;
};

// §11.8 / §12.3: "OFF" or "BATTERY EMPTY" at a big scale, optional second line.
struct MessageScreen : Screen {
  const char* text = "";
  const char* sub = nullptr;
  uint8_t     scale = 4;
  void draw(Display& d, uint8_t regions) override;
};

// §12.3 charging display from OFF: percentage and "charging" / "full" at dimPct.
struct ChargeScreen : Screen {
  bool    valid = false;
  uint8_t pct = 0;
  bool    full = false;
  void draw(Display& d, uint8_t regions) override;
};

// §18: the fault card.
struct FaultScreen : Screen {
  const char* reason = "";
  void draw(Display& d, uint8_t regions) override;
};
