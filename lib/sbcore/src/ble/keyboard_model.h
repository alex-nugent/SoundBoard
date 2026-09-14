// The keyboard's report scheduler (FirmwareSpec.md §8.3): one ordered queue
// of Type / Hold / Tap requests, the typing cadence, the held key with its
// forced release, and the rules for what is dropped. Host-independent: the
// driver feeds requests and the ready state in, and pumps reports out.
#pragma once
#include <stdint.h>
#include "ble/keys.h"

namespace sb {

constexpr uint8_t  KBD_QUEUE_DEPTH = 4;    // §8.3: a fifth request is dropped
constexpr uint8_t  KBD_TYPE_MAX    = 64;   // characters per Type request
constexpr uint16_t KBD_TAP_MS      = 30;

struct KbdReport {
  uint8_t id;        // 1 keyboard (modifiers, reserved, 6 usages), 2 consumer bitmap
  uint8_t len;       // 8 or 1
  uint8_t data[8];
};

// ASCII -> usage + modifier (the core keymap[] in the driver, a stub in tests); false = skip the character.
typedef bool (*KeymapFn)(uint8_t c, uint8_t& usage, uint8_t& modifier);

class KeyboardModel {
 public:
  void configure(uint8_t typeDelayMs, uint16_t holdKeysMaxMs);
  void setKeymap(KeymapFn fn) { keymap_ = fn; }

  // §8.2: reports are queued only while a host is ready; losing it drops everything, silently.
  void setReady(bool ready);
  bool ready() const { return ready_; }

  // §8.3 requests. False = not ready (nothing queued) or the queue is full.
  bool type(const char* text);
  bool hold(const KeyDef& k, uint16_t owner);
  bool tap(const KeyDef& k);
  void release(uint16_t owner);        // PadUp: the held key of that press, or its Hold still waiting in the queue
  void releaseAll();                   // level change, disable, pause, stuck pad, update: the held key up, queued holds gone

  // The driver pumps: one report per call, in order; false = nothing due at `now`.
  bool next(uint32_t now, KbdReport& out);

  bool          idle() const { return !qN_ && !cur_ && !held_ && !upPending_; }
  bool          holding() const { return held_ != nullptr; }
  uint16_t      heldOwner() const { return heldOwner_; }
  const KeyDef* heldKey() const { return held_; }
  bool          typing() const { return cur_ && curKind_ == Kind::Type; }
  uint8_t       queued() const { return qN_; }
  uint32_t      dropped() const { return dropped_; }        // queue full
  uint32_t      forced() const { return forced_; }          // holdKeysMaxMs releases
  bool          takeForcedFlag() { bool f = forcedFlag_; forcedFlag_ = false; return f; }

 private:
  enum class Kind : uint8_t { Type, Hold, Tap };
  struct Item { Kind kind; const KeyDef* key; uint16_t owner; char text[KBD_TYPE_MAX + 1]; };
  bool enqueue(const Item& it);
  const Item& front() const { return q_[qHead_]; }
  void pop() { qHead_ = (uint8_t)((qHead_ + 1) % KBD_QUEUE_DEPTH); qN_--; }
  void clear();
  static void fillDown(KbdReport& r, uint8_t usage, uint8_t mod, uint8_t consumerBit);
  static void fillUp(KbdReport& r, bool consumer);
  bool emitHeldUp(KbdReport& out);

  KeymapFn keymap_ = nullptr;
  uint8_t  delay_ = 12;
  uint16_t holdMax_ = 3000;
  bool     ready_ = false;
  Item     q_[KBD_QUEUE_DEPTH];
  uint8_t  qHead_ = 0, qN_ = 0;
  // In progress: a Type string or a Tap's pending up.
  bool     cur_ = false;
  Kind     curKind_ = Kind::Type;
  const KeyDef* curKey_ = nullptr;
  char     curText_[KBD_TYPE_MAX + 1] = "";
  uint8_t  ti_ = 0, phase_ = 0;        // Type: index and 0 = down due, 1 = up due
  uint32_t dueAt_ = 0;
  // Held key.
  const KeyDef* held_ = nullptr;
  uint16_t heldOwner_ = 0;
  uint32_t heldUntil_ = 0;
  bool     upPending_ = false;         // a release asked for: the up report leaves first
  bool     upConsumer_ = false;
  uint32_t dropped_ = 0, forced_ = 0;
  bool     forcedFlag_ = false;
};

}  // namespace sb
