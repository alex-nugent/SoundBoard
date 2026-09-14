#include "diag/console.h"
#include "diag/log.h"
#include "app/state.h"
#include "config/store.h"
#include "config/loader.h"
#include "config/settings_table.h"
#include "util/strutil.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include <string.h>

namespace Console {

static AppState* s_app = nullptr;
static char*     s_line = nullptr;
static size_t    s_len = 0;
static constexpr size_t CON_LINE_MAX = 4096;   // `merge <json>` takes a whole document on one line
static bool      s_lastWasCR = false;
static size_t    s_cap = 0;

static void printHelp() {
  Serial.println();
  Serial.println("SoundBoard V4 console (Enter ends a line)");
  Serial.println("  ?                 help        s                 status");
  Serial.println("  get <path>        show a setting (or a section, or . for all)");
  Serial.println("  set <path> <val>  change a setting in RAM (JSON or text; e.g. set display.theme green,");
  Serial.println("                    set touch.padPressPct [1,3,3,3], set power {\"offHoldMs\":1200})");
  Serial.println("  merge <json>      merge a partial document (validated as a whole)");
  Serial.println("  save              write config.json + flash mirror    dump   print config.json");
  Serial.println("  factory           defaults on card + mirror; runtime state cleared");
  Serial.println("  log [n]           last n log lines (100)    loglevel error|warn|info|debug");
  Serial.println("  reboot | crash | wdt   restart / abort() / spin without feeding the watchdog");
  Serial.println("  1-4 [ms]          press a pad (released after 100 ms or the given ms): level pad, sounds, actions");
  Serial.println("  + | -             attendant click (volume)    ++   both-button tap (level 1)    h+ | h-   long hold (next / prev level)");
  Serial.println("  l                 live pad deltas at 1 Hz    c   recalibrate pads");
  Serial.println("  b | bt            toggle on-board speakers / Bluetooth speaker    sounds   cache listing    AT+...  to the KCX");
  Serial.println("  p | pairwipe      pair a Bluetooth speaker (AT+PAIR, 60 s) / forget all saved speakers, then pair (AT+DELVMLINK first)");
  Serial.println("  play <file.wav>   bench: play a card file directly");
  Serial.println("  sleep | off       SLEEP now (any pad or button wakes) / OFF now (a button held ~0.5 s wakes)");
  Serial.println("  batt [V|clear]    battery status; `batt 4.12` calibrates K against a meter on VBAT; `batt clear` = design divider");
  Serial.println("  batt fake <V> [nousb]|off  bench: every sample reads <V> (RAM only), `nousb` also fakes USB absent: low-battery row, BATTERY EMPTY -> OFF");
  Serial.println("  kbd | kbdforget   toggle the BLE keyboard (RAM) / forget every bonded host (ble_store_clear)");
  Serial.println("  kbdtype <text>    bench: type text to the host    kbdkey <NAME> [ms]   bench: tap a key, or hold it for ms (Appendix B names)");
  Serial.println("  m                 open the Quick Menu (m again: exit and save); in it: 1 back, 2 or - down, 3 or + up / OK, 4 next");
  Serial.println("  w                 Wi-Fi setup (Phase 10)");
  Serial.println();
}

static void notYet(const char* what, int phase) {
  Serial.printf("%s: arrives with Phase %d\n", what, phase);
}

static void handleLine(char* line, uint32_t now) {
  // Trim.
  while (*line == ' ' || *line == '\t') line++;
  size_t n = strlen(line);
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
  if (!n) return;
  s_app->registerInput(now);

  if (strncmp(line, "AT+", 3) == 0) { s_app->kcx().send(line); return; }

  // Split the first word from the rest.
  char* rest = line;
  while (*rest && *rest != ' ') rest++;
  if (*rest) { *rest++ = 0; while (*rest == ' ') rest++; }
  const char* cmd = line;

  if (n == 1) {
    switch (cmd[0]) {
      case '?': printHelp(); s_app->printStatus(Serial); return;
      case 's': s_app->printStatus(Serial); return;
      case '1': case '2': case '3': case '4': s_app->simulatePress((uint8_t)(cmd[0] - '1'), 100); return;
      case '+': s_app->simulateClick(+1); return;
      case '-': s_app->simulateClick(-1); return;
      case 'l': s_app->setLiveDeltas(!s_app->liveDeltas()); Serial.printf("live pad deltas %s\n", s_app->liveDeltas() ? "ON (1 Hz)" : "off"); return;
      case 'c': s_app->recalibrate(); Serial.println("recalibrating: hands off the pads"); return;
      case 'b': Serial.printf("on-board speakers %s\n", s_app->toggleSpeakers() ? "ON" : "off"); return;
      case 'v': s_app->buzzTest(); return;
      case 'p': Serial.println(s_app->startPairing(false) ? "pairing: put the speaker in pairing mode; searching for 60 s" : "pairing not started (see the log)"); return;
      case 'm':
        if (s_app->mode() == AppMode::Menu) { s_app->closeMenu(true, "console"); Serial.println("menu closed (saved if anything changed)"); }
        else { s_app->openMenu("console"); Serial.println(s_app->mode() == AppMode::Menu ? "menu open: 1 back, 2 or - down, 3 or + up (OK on an action), 4 next, m exits and saves" : "menu not opened (see the log)"); }
        return;
      case 'w': notYet("Wi-Fi setup", 10); return;
      default: Serial.printf("unknown command '%c' -- ? for help\n", cmd[0]); return;
    }
  }

  if (!strcmp(cmd, "help")) { printHelp(); return; }
  if (!strcmp(cmd, "kcxcrlf")) { s_app->kcx().setCrlf(!s_app->kcx().crlf()); Serial.printf("KCX commands end with %s\n", s_app->kcx().crlf() ? "CR LF" : "nothing"); return; }
  if (!strcmp(cmd, "pin")) { int n = atoi(rest); gpio_dump_io_configuration(stdout, 1ULL << n); fflush(stdout); return; }
  if (!strcmp(cmd, "pairwipe")) { Serial.println(s_app->startPairing(true) ? "forgetting all saved speakers, then pairing: put the speaker in pairing mode; searching for 60 s" : "pairing not started (see the log)"); return; }
  if (!strcmp(cmd, "reboot")) { Serial.println("rebooting"); Serial.flush(); delay(50); ESP.restart(); return; }
  if (!strcmp(cmd, "crash")) { Serial.println("abort() now: expect a panic, a core dump and a reboot"); Serial.flush(); delay(50); abort(); }
  if (!strcmp(cmd, "wdt")) {
    Serial.println("spinning without feeding the task watchdog: expect a reset in 5 s"); Serial.flush();
    for (;;) { __asm__ __volatile__("nop"); }
  }
  if (!strcmp(cmd, "get")) {
    if (!s_app->store().getText(s_app->config(), rest, Serial)) Serial.printf("no such setting \"%s\"\n", rest);
    return;
  }
  if (!strcmp(cmd, "set")) {
    char* value = rest;
    while (*value && *value != ' ') value++;
    if (*value) { *value++ = 0; while (*value == ' ') value++; }
    if (!*rest || !*value) { Serial.println("usage: set <path> <value>"); return; }
    char err[100];
    if (s_app->setSetting(rest, value, err, sizeof err)) {
      Serial.printf("ok: %s = ", rest);
      s_app->store().getText(s_app->config(), rest, Serial);
      const sb::ConfigReport& r = s_app->report();
      for (uint8_t i = 0; i < r.count; i++) Serial.printf("  warning: %s\n", r.items[i].text);
      Serial.println("(in RAM; `save` writes it)");
    } else Serial.printf("rejected: %s\n", err);
    return;
  }
  if (!strcmp(cmd, "merge")) {
    if (!*rest) { Serial.println("usage: merge <json object>"); return; }
    char err[100];
    if (s_app->mergeSettings(rest, err, sizeof err)) {
      const sb::ConfigReport& r = s_app->report();
      Serial.printf("ok: merged (%u warning(s)); `save` writes it\n", (unsigned)r.count);
      for (uint8_t i = 0; i < r.count; i++) Serial.printf("  warning: %s\n", r.items[i].text);
    } else Serial.printf("rejected: %s\n", err);
    return;
  }
  if (!strcmp(cmd, "save")) {
    char err[100];
    if (s_app->requestSave(true, err, sizeof err)) Serial.printf("saved revision %lu%s\n", (unsigned long)s_app->config().revision, s_app->store().lastSaveCardLess() ? " (card-less: mirror only)" : "");
    else Serial.printf("save failed: %s\n", err);
    return;
  }
  if (!strcmp(cmd, "dump")) { s_app->store().dump(Serial); return; }
  if (!strcmp(cmd, "factory")) {
    char err[100];
    if (s_app->factory(err, sizeof err)) Serial.println("factory defaults written to card and mirror");
    else Serial.printf("factory reset failed: %s\n", err);
    return;
  }
  if (!strcmp(cmd, "sdclk")) {              // bench only: remount the card at another clock (MHz)
    int mhz = atoi(rest);
    if (mhz < 1 || mhz > 40) { Serial.printf("card clock %lu Hz; usage: sdclk <MHz>\n", (unsigned long)s_app->store().sdHz()); return; }
    bool ok = s_app->store().remountCard((uint32_t)mhz * 1000000UL);
    Serial.printf("card remount at %d MHz: %s\n", mhz, ok ? "mounted" : "FAILED");
    return;
  }
  if (!strcmp(cmd, "sdcycle")) {            // card recovery: gated rail off (default 2000 ms), screen re-init, remount
    int ms = atoi(rest); if (ms < 200) ms = 2000; if (ms > 10000) ms = 10000;
    Serial.printf("card recovery: %s\n", s_app->cardRecover((uint32_t)ms) ? "mounted" : "FAILED");
    return;
  }
  if (!strcmp(cmd, "sdtest")) {             // bench: `sdtest [psram] [chunk]` 16 KB write/verify; chunk 512 = single-block writes
    bool ps = strncmp(rest, "psram", 5) == 0;
    const char* c = rest; if (ps) { c += 5; while (*c == ' ') c++; }
    s_app->cardWriteTest(ps, (uint32_t)atoi(c), Serial);
    return;
  }
  if (!strcmp(cmd, "sdchunk")) {            // bench: bytes per card write (512 = single-block writes)
    int n = atoi(rest);
    if (n >= 64 && n <= 4096) ConfigStore::writeChunk = (uint32_t)n;
    Serial.printf("card write chunk %lu bytes\n", (unsigned long)ConfigStore::writeChunk);
    return;
  }
  if (!strcmp(cmd, "sdraw")) {              // bench: sdraw <hz> [crc] [lib] [low] [n]  raw CMD24/CMD17 over n sectors, restored afterwards
    uint32_t hz = 0; bool crc = false, lib = false, low = false, dark = false; int n = 0;
    char* tok = strtok(rest, " ");
    while (tok) {
      if (!strcmp(tok, "crc")) crc = true; else if (!strcmp(tok, "lib")) lib = true; else if (!strcmp(tok, "low")) low = true; else if (!strcmp(tok, "dark")) dark = true;
      else if (atoi(tok) >= 100000) hz = (uint32_t)atoi(tok); else if (atoi(tok) > 0) n = atoi(tok);
      tok = strtok(nullptr, " ");
    }
    s_app->cardRawTest(hz, crc, lib, low, dark, n, Serial);
    return;
  }
  if (!strcmp(cmd, "log")) { int lines = atoi(rest); if (lines <= 0) lines = 100; Log::tail(lines, Serial); return; }
  if (!strcmp(cmd, "loglevel")) {
    static const char* NAMES[] = { "error", "warn", "info", "debug" };
    for (int i = 0; i < 4; i++) if (sb::eqNoCase(rest, NAMES[i])) { Log::setLevel((LogLevel)i); Serial.printf("log level %s\n", NAMES[i]); return; }
    Serial.printf("log level is %s (error|warn|info|debug)\n", NAMES[(int)Log::level()]);
    return;
  }
  if (!strcmp(cmd, "sleep")) { Serial.println("SLEEP now: any pad or button wakes it (the USB port disappears)"); Serial.flush(); s_app->enterSleep("console"); return; }
  if (!strcmp(cmd, "off"))   { Serial.println("OFF now: hold a button ~0.5 s to wake"); Serial.flush(); s_app->enterOff("console"); return; }
  if (!strcmp(cmd, "batt") || !strcmp(cmd, "vbat")) {
    if (!*rest) { s_app->battery().printStatus(Serial); return; }
    if (!strcmp(rest, "clear")) { Serial.println(s_app->battery().clearCalibration() ? "battery method: design divider (mV x 3.7625)" : "NVS write failed"); return; }
    if (!strncmp(rest, "fake", 4)) {                           // bench: fake the battery voltage (RAM only) to exercise the low/empty paths
      const char* a = rest + 4; while (*a == ' ') a++;
      uint16_t mv = (!*a || !strcmp(a, "off")) ? 0 : (uint16_t)(atof(a) * 1000.0f + 0.5f);
      bool noUsb = strstr(a, "nousb") != nullptr;
      if (mv && (mv < 2500 || mv > 4500)) { Serial.println("usage: batt fake <2.5..4.5 V> [nousb] | batt fake off"); return; }
      s_app->battery().setFake(mv, noUsb);
      if (mv) Serial.printf("battery FAKE %u mV -> %u %% until `batt fake off` or a reset\n", (unsigned)mv, (unsigned)s_app->battery().percent());
      else Serial.println("battery fake off: real readings");
      return;
    }
    char msg[120];
    if (s_app->battery().calibrate((float)atof(rest), msg, sizeof msg)) Serial.println(msg); else Serial.printf("rejected: %s\n", msg);
    return;
  }
  if (!strcmp(cmd, "++") || !strcmp(cmd, "--")) { s_app->simulateCommand(sb::ButtonCmd::Level1); return; }
  if (!strcmp(cmd, "h+")) { s_app->simulateCommand(sb::ButtonCmd::NextLevel); return; }
  if (!strcmp(cmd, "h-")) { s_app->simulateCommand(sb::ButtonCmd::PrevLevel); return; }
  if (!strcmp(cmd, "bt")) { Serial.printf("bluetooth speaker %s\n", s_app->toggleBluetooth() ? "enabled" : "disabled"); return; }
  if (!strcmp(cmd, "sounds")) { s_app->cache().list(Serial); return; }
  if (!strcmp(cmd, "play")) { if (*rest) s_app->playFile(rest); else Serial.println("usage: play <file.wav>"); return; }
  if (!strcmp(cmd, "kbd")) { Serial.printf("keyboard %s (RAM; `save` writes it)\n", s_app->toggleKeyboard() ? "enabled: advertising" : "disabled: host dropped, advertising stopped"); return; }
  if (!strcmp(cmd, "kbdforget")) { s_app->keyboard().forget(); Serial.println("every keyboard host forgotten; forget the board on the host too, then pair again"); return; }
  if (!strcmp(cmd, "kbdtype")) {
    if (!*rest) { Serial.println("usage: kbdtype <text>"); return; }
    Serial.println(s_app->keyboard().typeText(rest, millis()) ? "typing" : "not sent: no host ready, or the queue is full");
    return;
  }
  if (!strcmp(cmd, "kbdkey")) {
    char* ms = rest; while (*ms && *ms != ' ') ms++; if (*ms) *ms++ = 0;
    if (!*rest) { Serial.println("usage: kbdkey <NAME> [hold ms]   (SPACE ENTER TAB ESC BKSP UP DOWN LEFT RIGHT 0-9 A-Z F1-F12 NEXT PREV STOP PLAY MUTE VOL+ VOL- HOME)"); return; }
    uint16_t hold = (uint16_t)atoi(ms);
    Serial.println(s_app->keyboard().keyByName(rest, hold, millis()) ? (hold ? "held" : "tapped") : "not sent: unknown key name, no host ready, or the queue is full");
    return;
  }
  if (cmd[0] == 'j' && cmd[1] >= '1' && cmd[1] <= '4' && !cmd[2]) { s_app->jackTest((uint8_t)(cmd[1] - '1')); Serial.printf("J%c closed for 1 s\n", cmd[1]); return; }
  if ((cmd[0] >= '1' && cmd[0] <= '4') && !cmd[1]) { s_app->simulatePress((uint8_t)(cmd[0] - '1'), (uint32_t)atoi(rest)); return; }
  Serial.printf("unknown command \"%s\" -- ? for help\n", cmd);
}

void begin(AppState* app) {
  s_app = app;
  if (!s_line) {
    s_line = static_cast<char*>(heap_caps_malloc(CON_LINE_MAX, MALLOC_CAP_SPIRAM));
    s_cap = CON_LINE_MAX;
    if (!s_line) { s_line = static_cast<char*>(malloc(512)); s_cap = s_line ? 512 : 0; }
  }
  s_len = 0;
}

void tick(uint32_t now) {
  if (!s_line) return;
  size_t cap = s_cap;
  if (cap < 2) return;
  int budget = 512;                              // bytes per tick
  while (budget-- > 0 && Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    Log::hostActivity();
    if (c == '\n' && s_lastWasCR) { s_lastWasCR = false; continue; }
    s_lastWasCR = (c == '\r');
    if (c == '\r' || c == '\n') {
      s_line[s_len] = 0;
      s_len = 0;
      handleLine(s_line, now);
      continue;
    }
    if (c == 0x08 || c == 0x7F) { if (s_len) s_len--; continue; }
    if (c < 0x20) continue;
    if (s_len < cap - 1) s_line[s_len++] = (char)c;
  }
}

}  // namespace Console
