// Logging (FirmwareSpec.md §19.8): LOG_E/W/I/D with a tag, to the USB console
// and a 64 KB RAM ring. The USB-JTAG CDC cannot tell a terminal from the host
// kernel polling the endpoint, and bytes handed to the host before a terminal
// opens are discarded by it; so nothing is streamed until the terminal has
// sent a byte (press Enter), then the whole ring since boot is replayed and
// lines stream live. The flag survives resets in RtcState while USB stays
// plugged, so a reset with the monitor open streams at once. The LittleFS
// mirror and the card log come with Phase 12.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Print.h>

enum class LogLevel : uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3 };

namespace Log {

void begin();                                   // allocates the ring (PSRAM)
void setLevel(LogLevel level);
LogLevel level();
void printf(LogLevel level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void write(const char* text, size_t len);       // raw text into the ring and the console
void tick();                                    // flushes unsent ring content when a host is connected
void tail(int lines, Print& out);               // the last N lines of the ring
bool hostConnected();                           // USB plugged, CDC connected, and a terminal has sent a byte
void hostActivity();                            // the console saw a byte from the host
uint32_t bytesLogged();
void setCardSink(bool on);                      // §18 diag.logToCard: append new lines to /log.txt on the card (cardTick() does the writes)
void cardTick();                                // app task, once a second: at most 4 KB per call, only when the audio side is idle

}  // namespace Log

#define LOG_E(tag, ...) Log::printf(LogLevel::Error, tag, __VA_ARGS__)
#define LOG_W(tag, ...) Log::printf(LogLevel::Warn,  tag, __VA_ARGS__)
#define LOG_I(tag, ...) Log::printf(LogLevel::Info,  tag, __VA_ARGS__)
#define LOG_D(tag, ...) Log::printf(LogLevel::Debug, tag, __VA_ARGS__)
