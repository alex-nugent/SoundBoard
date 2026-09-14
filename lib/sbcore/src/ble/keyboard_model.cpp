#include "ble/keyboard_model.h"
#include <string.h>

namespace sb {

static inline bool dueAt(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

void KeyboardModel::configure(uint8_t typeDelayMs, uint16_t holdKeysMaxMs) {
  delay_ = typeDelayMs ? typeDelayMs : 12;
  holdMax_ = holdKeysMaxMs ? holdKeysMaxMs : 3000;
}

void KeyboardModel::setReady(bool ready) {
  if (ready == ready_) return;
  ready_ = ready;
  if (!ready) clear();                   // §8.2/§8.3: nothing waits for a host; a held key is gone with the link
}

void KeyboardModel::clear() { qN_ = 0; qHead_ = 0; cur_ = false; held_ = nullptr; heldOwner_ = 0; upPending_ = false; }

bool KeyboardModel::enqueue(const Item& it) {
  if (qN_ >= KBD_QUEUE_DEPTH) { dropped_++; return false; }
  q_[(qHead_ + qN_) % KBD_QUEUE_DEPTH] = it;
  qN_++;
  return true;
}

bool KeyboardModel::type(const char* text) {
  if (!ready_ || !text || !*text) return false;
  Item it; it.kind = Kind::Type; it.key = nullptr; it.owner = 0;
  strncpy(it.text, text, KBD_TYPE_MAX); it.text[KBD_TYPE_MAX] = 0;
  return enqueue(it);
}

bool KeyboardModel::hold(const KeyDef& k, uint16_t owner) {
  if (!ready_) return false;
  Item it; it.kind = Kind::Hold; it.key = &k; it.owner = owner; it.text[0] = 0;
  return enqueue(it);
}

bool KeyboardModel::tap(const KeyDef& k) {
  if (!ready_) return false;
  Item it; it.kind = Kind::Tap; it.key = &k; it.owner = 0; it.text[0] = 0;
  return enqueue(it);
}

void KeyboardModel::release(uint16_t owner) {
  if (held_ && heldOwner_ == owner) { upPending_ = true; upConsumer_ = held_->consumer(); held_ = nullptr; heldOwner_ = 0; return; }
  // A Hold still waiting behind a string: the pad is up before its key went down, so it never goes down.
  uint8_t n = qN_; uint8_t head = qHead_; qN_ = 0; qHead_ = 0;
  Item keep[KBD_QUEUE_DEPTH]; uint8_t k = 0;
  for (uint8_t i = 0; i < n; i++) { const Item& it = q_[(head + i) % KBD_QUEUE_DEPTH]; if (!(it.kind == Kind::Hold && it.owner == owner)) keep[k++] = it; }
  for (uint8_t i = 0; i < k; i++) q_[i] = keep[i];
  qN_ = k;
}

void KeyboardModel::releaseAll() {
  if (held_) { upPending_ = true; upConsumer_ = held_->consumer(); held_ = nullptr; heldOwner_ = 0; }
  uint8_t n = qN_; uint8_t head = qHead_; qN_ = 0; qHead_ = 0;
  Item keep[KBD_QUEUE_DEPTH]; uint8_t k = 0;
  for (uint8_t i = 0; i < n; i++) { const Item& it = q_[(head + i) % KBD_QUEUE_DEPTH]; if (it.kind != Kind::Hold) keep[k++] = it; }
  for (uint8_t i = 0; i < k; i++) q_[i] = keep[i];
  qN_ = k;
}

void KeyboardModel::fillDown(KbdReport& r, uint8_t usage, uint8_t mod, uint8_t consumerBit) {
  memset(r.data, 0, sizeof r.data);
  if (consumerBit) { r.id = 2; r.len = 1; r.data[0] = consumerBit; }
  else { r.id = 1; r.len = 8; r.data[0] = mod; r.data[2] = usage; }
}

void KeyboardModel::fillUp(KbdReport& r, bool consumer) {
  memset(r.data, 0, sizeof r.data);
  if (consumer) { r.id = 2; r.len = 1; } else { r.id = 1; r.len = 8; }
}

bool KeyboardModel::emitHeldUp(KbdReport& out) {           // a new key wants the report: the held one goes up first
  if (!held_) return false;
  fillUp(out, held_->consumer());
  held_ = nullptr; heldOwner_ = 0;
  return true;
}

bool KeyboardModel::next(uint32_t now, KbdReport& out) {
  if (upPending_) { upPending_ = false; fillUp(out, upConsumer_); return true; }
  if (held_ && dueAt(now, heldUntil_)) {                    // §8.3: forced key up after holdKeysMaxMs
    fillUp(out, held_->consumer());
    held_ = nullptr; heldOwner_ = 0;
    forced_++; forcedFlag_ = true;
    return true;
  }
  for (;;) {
    if (cur_) {
      if (!dueAt(now, dueAt_)) return false;
      if (curKind_ == Kind::Tap) { fillUp(out, curKey_->consumer()); cur_ = false; return true; }
      // Type: alternate down / up per character at delay_ per edge; unmapped characters are skipped.
      if (phase_ == 1) { fillUp(out, false); ti_++; phase_ = 0; dueAt_ = now + delay_; return true; }
      uint8_t usage = 0, mod = 0;
      while (curText_[ti_] && !(keymap_ && keymap_((uint8_t)curText_[ti_], usage, mod) && usage)) ti_++;
      if (!curText_[ti_]) { cur_ = false; continue; }         // string done: the next item may start now
      fillDown(out, usage, mod, 0); phase_ = 1; dueAt_ = now + delay_;
      return true;
    }
    if (!qN_) return false;
    const Item& it = front();
    if (emitHeldUp(out)) return true;                       // any new key releases the held one first
    if (it.kind == Kind::Hold) {
      fillDown(out, it.key->usage, it.key->modifier, it.key->consumerBit);
      held_ = it.key; heldOwner_ = it.owner; heldUntil_ = now + holdMax_;
      pop();
      return true;
    }
    if (it.kind == Kind::Tap) {
      fillDown(out, it.key->usage, it.key->modifier, it.key->consumerBit);
      cur_ = true; curKind_ = Kind::Tap; curKey_ = it.key; dueAt_ = now + KBD_TAP_MS;
      pop();
      return true;
    }
    strncpy(curText_, it.text, KBD_TYPE_MAX); curText_[KBD_TYPE_MAX] = 0;
    cur_ = true; curKind_ = Kind::Type; ti_ = 0; phase_ = 0; dueAt_ = now;
    pop();
  }
}

}  // namespace sb
