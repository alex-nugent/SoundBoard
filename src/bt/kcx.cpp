#include "bt/kcx.h"
#include "hal/pins.h"
#include "diag/log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

static const char* TAG = "kcx";
static SemaphoreHandle_t s_mutex = nullptr;

void KcxLink::begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  Serial1.begin(115200, SERIAL_8N1, pins::KCX_RX, pins::KCX_TX);    // §2.3 rule 5: UART open before the 5 V rail
  open_ = true; len_ = 0; powerOn_ = false; softOff_ = false; link_ = Link::Unknown;
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

void KcxLink::onLine(const char* line) {
  strlcpy(last_, line, sizeof last_);
  LOG_I(TAG, "<- %s", line);
  if (strstr(line, "POWER ON")) { powerOn_ = true; powerOnAt_ = millis(); link_ = Link::Unknown; softOff_ = false; }
  if (strstr(line, "DISCONNECT") || strstr(line, "STATUS:0")) link_ = Link::NotLinked;
  else if (strstr(line, "CONNECT") || strstr(line, "STATUS:1")) link_ = Link::Linked;
  else if (strstr(line, "SCAN")) link_ = Link::Scanning;
}

void KcxLink::feed() {
  if (!open_ || !s_mutex) return;
  if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;              // the other task is reading: it will see the bytes
  while (Serial1.available()) {
    int c = Serial1.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') { if (len_) { buf_[len_] = 0; onLine(buf_); len_ = 0; } continue; }
    if (len_ < sizeof buf_ - 1 && c >= 0x20) buf_[len_++] = (char)c;
  }
  xSemaphoreGive(s_mutex);
}

void KcxLink::send(const char* cmd) {
  if (!open_) { LOG_W(TAG, "-> %s dropped: UART closed (rail down)", cmd); return; }
  Serial1.print(cmd);                                            // Appendix A: no terminator
  LOG_I(TAG, "-> %s", cmd);
}

void KcxLink::powerOff() {
  send("AT+POWER_OFF");                                          // TODO(OPEN-8): current saved and silence confirmed at CP-3/CP-5
  softOff_ = true; link_ = Link::NotLinked;
}

void KcxLink::tick(uint32_t now, bool enabled) {
  feed();
  if (!enabled || !open_ || !powerOn_ || softOff_ || link_ == Link::Linked) { nextStatusAt_ = now + 60000; return; }
  if ((int32_t)(now - nextStatusAt_) >= 0) { nextStatusAt_ = now + 60000; send("AT+STATUS?"); }
}
