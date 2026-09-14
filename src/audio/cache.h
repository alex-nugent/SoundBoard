// The sound cache (FirmwareSpec.md §6.4): every sound in PSRAM as canonical
// mono 44.1 kHz PCM, filled by the low-priority `loader` task in 8 KB reads
// under the storage mutex (cues first, then the current level, the other
// levels, then everything else until the budget is reached). Entries are
// generation-tracked: a reload retires the old ones and frees each once the
// audio task has released it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Print.h>
#include "config/config.h"

enum SoundState : uint8_t { S_UNKNOWN = 0, S_QUEUED, S_LOADING, S_CACHED, S_STREAM, S_BAD, S_MISSING };

struct SoundEntry {
  char     name[41];          // file name in /sounds (case preserved; compared case-insensitively)
  int16_t* pcm;               // PSRAM, canonical; nullptr unless S_CACHED
  uint32_t frames;            // canonical frames (cached or, for S_STREAM, computed from the header)
  uint32_t fileBytes;
  uint16_t rate;
  uint8_t  channels;
  uint8_t  state;             // SoundState
  uint8_t  refs;              // audio-task holds on pcm
  bool     retired;           // freed when refs reach 0
  uint16_t generation;
  char     reason[28];        // why it is not cached
};

class SoundCache {
 public:
  static constexpr uint16_t MAX_FILES = 200;

  bool begin();                                              // allocates the table in PSRAM
  void setBudget(uint32_t bytes) { budget_ = bytes; }
  // Rebuilds the table from /sounds and (re)starts the loader in the §6.4 order. Joins a running loader first.
  void reload(const sb::Config& cfg, uint8_t currentLevel);
  // A fingerprint of the names the configuration references; reload when it changes.
  static uint32_t referenceHash(const sb::Config& cfg);

  // Audio task: hold a cached buffer for playback. nullptr = not cached (stream it instead).
  const SoundEntry* acquire(const char* name);
  void release(const SoundEntry* e);
  uint8_t stateOf(const char* name) const;

  uint16_t count() const { return n_; }
  uint16_t cachedCount() const;
  uint32_t bytesUsed() const { return used_; }
  uint32_t budget() const { return budget_; }
  bool     loaderRunning() const { return running_; }
  uint16_t generation() const { return generation_; }
  bool     allDone() const { return !running_ && scanned_; }
  void     list(Print& out) const;
  const char* stateName(uint8_t s) const;
  void     joinLoader();                                     // cancel the loader and wait for it: sleep/off entry, reload

 private:
  static void loaderThunk(void* arg);
  void loaderMain();
  bool loadOne(uint16_t idx);
  int  findLocked(const char* name) const;
  int  addLocked(const char* name);
  void scanCard();
  void retireAll();

  SoundEntry* e_ = nullptr;
  uint16_t n_ = 0;
  uint16_t order_[MAX_FILES];
  uint16_t nOrder_ = 0;
  uint32_t budget_ = 6u * 1024u * 1024u, used_ = 0;
  uint16_t generation_ = 0;
  volatile bool cancel_ = false, running_ = false;
  bool scanned_ = false;
  void* mutex_ = nullptr;
  void* task_ = nullptr;
};
