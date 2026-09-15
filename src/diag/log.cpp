#include "diag/log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <esp_heap_caps.h>
#include "power/rtc_state.h"
#include <stdarg.h>
#include <string.h>
#include <SD.h>
#include "hal/storage.h"

namespace Log {

static constexpr uint32_t RING_SIZE = 64 * 1024;
static uint8_t* s_ring = nullptr;
static uint32_t s_size = 0;
static uint32_t s_head = 0;     // total bytes ever written (byte i lives at s_ring[i % RING_SIZE])
static uint32_t s_sent = 0;     // bytes already written to the console
static LogLevel s_level = LogLevel::Info;
static bool     s_wasConnected = false;

void begin() {
  if (s_ring) return;
  s_ring = static_cast<uint8_t*>(heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM));
  s_size = RING_SIZE;
  if (!s_ring) { s_ring = static_cast<uint8_t*>(malloc(8 * 1024)); s_size = s_ring ? 8 * 1024 : 0; }   // no PSRAM: a small internal ring
}

void setLevel(LogLevel level) { s_level = level; }
LogLevel level() { return s_level; }
static void setGate(bool on) {
  RtcState& r = rtc::get();
  if (r.consoleSeen != on) { r.consoleSeen = on; rtc::commit(); }
}

static void ringPut(const char* p, size_t n);
static void note(const char* text) {                                   // diagnostics into the ring only (never streamed while the gate is off)
  char line[96]; int n = snprintf(line, sizeof line, "[%9lu] %s", (unsigned long)millis(), text);
  ringPut(line, n < (int)sizeof line ? n : sizeof line - 1);
}

bool hostConnected() {
  static bool wasPlugged = true, wasCdc = true;
  bool plugged = HWCDC::isPlugged();
  bool cdc = (bool)Serial;                 // polling this is what lets the driver notice a host
  if (plugged != wasPlugged) { wasPlugged = plugged; note(plugged ? "[log] USB SOF back\n" : "[log] USB SOF lost (gate clears after 1 s)\n"); }
  if (cdc != wasCdc) { wasCdc = cdc; note(cdc ? "[log] CDC connected\n" : "[log] CDC disconnected (host stopped taking data or bus reset)\n"); }
  static uint32_t unpluggedSince = 0;
  if (!plugged) {
    if (!unpluggedSince) unpluggedSince = millis() ? millis() : 1;
    if (millis() - unpluggedSince > 1000) {                     // a flash write stalls the SOF monitor briefly: only a real unplug clears the gate
      if (rtc::get().consoleSeen) note("[log] USB unplugged: gate off\n");
      setGate(false);
    }
    return false;
  }
  unpluggedSince = 0;
  return cdc && rtc::get().consoleSeen;
}

void hostActivity() { setGate(true); }
uint32_t bytesLogged() { return s_head; }

static uint32_t ringSize() { return s_size; }

static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;   // the audio task logs too (Phase 3)
static void ringPut(const char* p, size_t n) {
  uint32_t sz = ringSize();
  if (!sz) return;
  portENTER_CRITICAL(&s_ringMux);
  for (size_t i = 0; i < n; i++) s_ring[(s_head + i) % sz] = (uint8_t)p[i];
  s_head += n;
  if (s_head - s_sent > sz) s_sent = s_head - sz;   // the console fell behind by more than the ring: skip
  portEXIT_CRITICAL(&s_ringMux);
}

// Sends pending ring bytes to the console without blocking: at most what the
// CDC buffer can take right now.
static void flushPending() {
  uint32_t sz = ringSize();
  if (!sz || !hostConnected()) return;
  int room = Serial.availableForWrite();
  while (s_sent < s_head && room > 0) {
    uint32_t off = s_sent % sz;
    uint32_t chunk = s_head - s_sent;
    if (chunk > sz - off) chunk = sz - off;
    if (chunk > (uint32_t)room) chunk = (uint32_t)room;
    size_t w = Serial.write(s_ring + off, chunk);
    if (w == 0) break;
    s_sent += w;
    room -= (int)w;
  }
}

void write(const char* text, size_t len) {
  ringPut(text, len);
  if (hostConnected()) flushPending();
}

void printf(LogLevel level, const char* tag, const char* fmt, ...) {
  if ((uint8_t)level > (uint8_t)s_level) return;
  static const char LETTER[] = { 'E', 'W', 'I', 'D' };
  char line[256];
  int n = snprintf(line, sizeof line, "[%7lu] %c %-7s ", (unsigned long)millis(), LETTER[(uint8_t)level & 3], tag ? tag : "");
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = sizeof line - 2;
  va_list ap; va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - 1 - n, fmt, ap);
  va_end(ap);
  if (m < 0) m = 0;
  size_t len = (size_t)n + (size_t)m;
  if (len > sizeof line - 2) len = sizeof line - 2;
  line[len++] = '\n';
  line[len] = 0;
  write(line, len);
}

// --- the card log (§18 diag.logToCard) ---------------------------------------------------------
// Ring bytes not yet on the card go to /log.txt in one append per second, under the storage lock and only
// when the lock is free within 50 ms (a card write during playback is what the lock protects against).
// /log.txt rolls to /log.old at 512 KB. Nothing here blocks logging itself: the ring is the source, this drains it.
static bool     s_card = false;
static uint32_t s_cardSent = 0;
static bool     s_cardFailed = false;
void setCardSink(bool on) {
  if (on && !s_card) { s_cardSent = s_head > ringSize() ? s_head - ringSize() : 0; s_cardFailed = false; }   // whatever the ring still holds goes first
  s_card = on;
}
void cardTick() {
  if (!s_card || s_cardFailed || !s_ring || s_cardSent >= s_head) return;
  if (!Storage::tryLock(50)) return;                           // busy: next second
  uint32_t sz = ringSize();
  if (s_head - s_cardSent > sz) s_cardSent = s_head - sz;       // fell behind by more than the ring: skip
  uint32_t n = s_head - s_cardSent; if (n > 4096) n = 4096;
  File f = SD.open("/log.txt", FILE_APPEND);
  if (!f) { s_cardFailed = true; Storage::unlock(); note("card log: /log.txt would not open, card logging off"); return; }
  if (f.size() > 512 * 1024) { f.close(); SD.remove("/log.old"); SD.rename("/log.txt", "/log.old"); f = SD.open("/log.txt", FILE_APPEND); if (!f) { s_cardFailed = true; Storage::unlock(); return; } }
  uint32_t off = s_cardSent % sz, first = sz - off < n ? sz - off : n;
  f.write(s_ring + off, first);
  if (n > first) f.write(s_ring, n - first);
  f.close();
  s_cardSent += n;
  Storage::unlock();
}

void tick() {
  bool c = hostConnected();
  if (c && s_sent < s_head) flushPending();      // covers a host that connected after the lines were logged
  s_wasConnected = c;
}

void tail(int lines, Print& out) {
  uint32_t sz = ringSize();
  if (!sz || s_head == 0) return;
  uint32_t start = s_head > sz ? s_head - sz : 0;
  // Walk back `lines` newlines from the head.
  uint32_t pos = s_head;
  int seen = 0;
  while (pos > start) {
    if (s_ring[(pos - 1) % sz] == '\n') { if (++seen > lines) break; }
    pos--;
  }
  while (pos < s_head) {
    uint32_t off = pos % sz;
    uint32_t chunk = s_head - pos;
    if (chunk > sz - off) chunk = sz - off;
    if (chunk > 512) chunk = 512;
    out.write(s_ring + off, chunk);
    pos += chunk;
  }
}

}  // namespace Log
