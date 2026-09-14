// The storage mutex (FirmwareSpec.md §19.3) and deferred flash writes (§19.7
// rule 11): one recursive mutex around every SPI transaction to the card and
// the screen and every LittleFS write; flash writes run only when the audio
// engine has been idle for 200 ms (Phase 3 wires the engine; until then every
// write runs at once).
#pragma once
#include <stdint.h>

namespace Storage {

void begin();
void lock();
bool tryLock(uint32_t timeoutMs);       // the audio task's streaming reads: never wait longer than this on the screen or a save
void unlock();
struct Guard { Guard() { lock(); } ~Guard() { unlock(); } };

typedef void (*WriteFn)(void* arg);
typedef bool (*IdleFn)();

// Runs fn(arg) now if the audio side is idle, else queues it (up to 4). Returns
// false only if the queue is full.
bool deferredWrite(WriteFn fn, void* arg);
void setIdleCheck(IdleFn isIdle);       // Phase 3: the audio engine's "idle for 200 ms"
void tick(uint32_t now);                // drains the queue when idle
bool pending();

}  // namespace Storage
