#include "bt/kcx.h"
#include "hal/pins.h"
#include "diag/log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

static const char* TAG = "kcx";
static SemaphoreHandle_t s_mutex = nullptr;
#include "util/timer.h"

void KcxLink::begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  Serial1.begin(115200, SERIAL_8N1, pins::KCX_RX, pins::KCX_TX);    // §2.3 rule 5: UART open before the 5 V rail
  open_ = true; len_ = 0; powerOn_ = false; okSeen_ = false; softOff_ = false; link_ = Link::Unknown; pair_ = Pair::None; connects_ = 0;
}

void KcxLink::end() {
  if (open_) Serial1.end();
  open_ = false;
  pinMode(pins::KCX_TX, OUTPUT); digitalWrite(pins::KCX_TX, LOW);   // §6.7: TX LOW while the rail is off (no back-powering through the UART)
  railDown();
}

const char* KcxLink::linkName() const {
  switch (link_) { case Link::Linked: return "linked"; case Link::NotLinked: return "not linked"; case Link::Scanning: return "scanning"; default: return "unknown"; }
}

const char* KcxLink::pairName() const {
  switch (pair_) { case Pair::Wiping: return "wiping"; case Pair::Searching: return "searching"; case Pair::Connected: return "connected"; case Pair::Timeout: return "timed out"; default: return "idle"; }
}

void KcxLink::onLine(const char* line) {
  strlcpy(last_, line, sizeof last_);
  LOG_I(TAG, "<- %s", line);
  if (strstr(line, "POWER ON")) { powerOn_ = true; powerOnAt_ = millis(); link_ = Link::Unknown; softOff_ = false; }
  if (!strncmp(line, "OK+", 3)) okSeen_ = true;
  if (!strcmp(line, "OK+POWEROFF_MODE")) { softOff_ = true; link_ = Link::NotLinked; }
  if (const char* n = strstr(line, "Name:")) strlcpy(peer_, n + 5, sizeof peer_);   // "MacAdd:60e010bb6b68,Name:ECOXGEAR" precedes the link report
  if (strstr(line, "DISCONNECT") || strstr(line, "STATUS:0")) link_ = Link::NotLinked;
  else if (strstr(line, "CONNECT") || !strcmp(line, "CON LAST")) { link_ = Link::Linked; connects_++; }   // "CON LAST": linked to the saved speaker (bench, 2026-09-14)
  else if (strstr(line, "STATUS:1")) link_ = Link::Linked;
  else if (strstr(line, "SCAN")) link_ = Link::Scanning;
}

void KcxLink::feed() {
  if (!open_ || !s_mutex) return;
  if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;              // the other task is reading: it will see the bytes
  while (Serial1.available()) {
    int c = Serial1.read();
    if (c < 0) break;
    lastByteAt_ = millis();
    if (c == '\r') continue;
    if (c == '\n') { if (len_) { buf_[len_] = 0; onLine(buf_); len_ = 0; } continue; }
    if (len_ < sizeof buf_ - 1 && c >= 0x20) buf_[len_++] = (char)c;
  }
  if (len_ && millis() - lastByteAt_ > LINE_GAP_MS) { buf_[len_] = 0; onLine(buf_); len_ = 0; }   // a line the module ended with CR only, or not at all
  xSemaphoreGive(s_mutex);
}

void KcxLink::send(const char* cmd) {
  if (!open_) { LOG_W(TAG, "-> %s dropped: UART closed (rail down)", cmd); return; }
  Serial1.print(cmd);
  if (crlf_) Serial1.print("\r\n");                             // the module needs the CR LF (bench 2026-09-14)
  LOG_I(TAG, "-> %s", cmd);
}

void KcxLink::powerOff() {
  send("AT+POWER_OFF");                                          // TODO(OPEN-8): current saved and silence confirmed at CP-3/CP-5
  softOff_ = true; link_ = Link::NotLinked; pair_ = Pair::None;
}

bool KcxLink::startPair(bool wipe, uint32_t now) {
  if (!open_ || softOff_ || !alive()) { LOG_W(TAG, "pairing refused: %s", !open_ ? "UART closed (rail down)" : softOff_ ? "module soft-off (speaker disabled)" : "module silent (no banner, no reply)"); return false; }
  pairWipe_ = wipe; pairConnects_ = connects_;
  pairUntil_ = now + PAIR_MS; if (!pairUntil_) pairUntil_ = 1;
  if (wipe) { send("AT+DELVMLINK"); pair_ = Pair::Wiping; pairSendAt_ = now + 300; LOG_I(TAG, "pairing: saved speakers wiped, AT+PAIR follows (§7.3 step 4)"); }
  else      { send("AT+PAIR"); pair_ = Pair::Searching; LOG_I(TAG, "pairing: searching for %lu s%s", (unsigned long)(PAIR_MS / 1000), linked() ? " (a speaker is linked now: a new CONNECT line counts)" : ""); }
  return true;
}

void KcxLink::cancelPair() {
  if (pair_ == Pair::None) return;
  LOG_I(TAG, "pairing cancelled while %s", pairName());
  pair_ = Pair::None;
}

uint32_t KcxLink::pairLeftMs(uint32_t now) const {
  if (pair_ != Pair::Searching && pair_ != Pair::Wiping) return 0;
  return due(now, pairUntil_) ? 0 : pairUntil_ - now;
}

void KcxLink::tick(uint32_t now, bool enabled) {
  feed();
  if (pair_ == Pair::Wiping && due(now, pairSendAt_)) { send("AT+PAIR"); pair_ = Pair::Searching; }
  if (pair_ == Pair::Searching) {
    if (connects_ != pairConnects_) { pair_ = Pair::Connected; LOG_I(TAG, "pairing: connected (%s)", last_); }
    else if (due(now, pairUntil_))  { pair_ = Pair::Timeout;   LOG_W(TAG, "pairing: no speaker found in %lu s", (unsigned long)(PAIR_MS / 1000)); }
  }
  bool pairing = pair_ == Pair::Wiping || pair_ == Pair::Searching;
  if (!enabled || !open_ || !alive() || softOff_ || link_ == Link::Linked || pairing) { nextStatusAt_ = now + 60000; return; }
  if (due(now, nextStatusAt_)) { nextStatusAt_ = now + 60000; send("AT+STATUS?"); }
}
