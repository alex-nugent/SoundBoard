#include "audio/cache.h"
#include "audio/wav.h"
#include "audio/resample.h"
#include "hal/storage.h"
#include "diag/log.h"
#include "util/strutil.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "cache";
static constexpr size_t READ_CHUNK = 8192;

#define MTX ((SemaphoreHandle_t)mutex_)
struct Lock { SemaphoreHandle_t m; Lock(void* x) : m((SemaphoreHandle_t)x) { xSemaphoreTake(m, portMAX_DELAY); } ~Lock() { xSemaphoreGive(m); } };

bool SoundCache::begin() {
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  if (!e_) e_ = static_cast<SoundEntry*>(heap_caps_calloc(MAX_FILES, sizeof(SoundEntry), MALLOC_CAP_SPIRAM));
  if (!e_) e_ = static_cast<SoundEntry*>(calloc(MAX_FILES, sizeof(SoundEntry)));
  return e_ != nullptr && mutex_ != nullptr;
}

const char* SoundCache::stateName(uint8_t s) const {
  switch (s) { case S_QUEUED: return "queued"; case S_LOADING: return "loading"; case S_CACHED: return "cached"; case S_STREAM: return "streams"; case S_BAD: return "bad"; case S_MISSING: return "missing"; default: return "unknown"; }
}

int SoundCache::findLocked(const char* name) const {
  for (uint16_t i = 0; i < n_; i++) if (!e_[i].retired && sb::eqNoCase(e_[i].name, name)) return i;
  return -1;
}

int SoundCache::addLocked(const char* name) {
  int i = findLocked(name);
  if (i >= 0) return i;
  if (n_ >= MAX_FILES) return -1;
  SoundEntry& e = e_[n_];
  memset(&e, 0, sizeof e);
  sb::copyStr(e.name, sizeof e.name, name);
  e.state = S_UNKNOWN; e.generation = generation_;
  return n_++;
}

uint32_t SoundCache::referenceHash(const sb::Config& cfg) {
  uint32_t h = 2166136261u;
  auto mix = [&](const char* s) { for (; *s; s++) { h ^= (uint8_t)tolower((unsigned char)*s); h *= 16777619u; } h ^= '|'; h *= 16777619u; };
  mix(cfg.audio.cues.startup); mix(cfg.audio.cues.click); mix(cfg.audio.cues.saved); mix(cfg.audio.cues.lowBattery);
  for (uint8_t l = 0; l < cfg.levels.count; l++) for (uint8_t b = 0; b < sb::MAX_SOUND_PADS; b++) mix(cfg.levels.levels[l].buttons[b].sound);
  h ^= cfg.audio.cacheMaxMB; h *= 16777619u;
  return h;
}

// ---------------------------------------------------------------------------
// Table and order
// ---------------------------------------------------------------------------
void SoundCache::scanCard() {
  Storage::Guard g;
  File dir = SD.open("/sounds");
  if (!dir || !dir.isDirectory()) { LOG_W(TAG, "/sounds is missing on the card"); scanned_ = true; return; }
  uint16_t seen = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      const char* nm = f.name();
      const char* base = strrchr(nm, '/'); base = base ? base + 1 : nm;
      if (sb::validSoundName(base) && base[0]) {
        int i = addLocked(base);
        if (i >= 0) { e_[i].fileBytes = f.size(); seen++; }
      }
    }
    f.close();
  }
  dir.close();
  scanned_ = true;
  LOG_I(TAG, "/sounds: %u file(s)", (unsigned)seen);
}

void SoundCache::retireAll() {
  for (uint16_t i = 0; i < n_; i++) {
    SoundEntry& e = e_[i];
    if (e.pcm) {
      if (e.refs == 0) { heap_caps_free(e.pcm); e.pcm = nullptr; }
      else e.retired = true;                                   // the audio task frees it on release()
    }
  }
  // Compact: keep only retired-in-use entries (they must stay addressable until released).
  uint16_t k = 0;
  for (uint16_t i = 0; i < n_; i++) if (e_[i].retired && e_[i].refs) e_[k++] = e_[i];
  n_ = k; used_ = 0;
}

void SoundCache::joinLoader() {
  if (!running_) return;
  cancel_ = true;
  uint32_t t0 = millis();
  while (running_ && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(2));
  if (running_) LOG_E(TAG, "loader did not stop");
  cancel_ = false;
}

void SoundCache::reload(const sb::Config& cfg, uint8_t currentLevel) {
  joinLoader();
  {
    Lock l(mutex_);
    generation_++;
    retireAll();
    scanned_ = false;
    nOrder_ = 0;
    // §6.4 order: cues, the current level, the other levels, then the rest of the card.
    auto want = [&](const char* name) {
      if (!name || !*name) return;
      int i = addLocked(name);
      if (i < 0) return;
      for (uint16_t k = 0; k < nOrder_; k++) if (order_[k] == i) return;
      if (nOrder_ < MAX_FILES) order_[nOrder_++] = (uint16_t)i;
    };
    want(cfg.audio.cues.startup); want(cfg.audio.cues.click); want(cfg.audio.cues.saved); want(cfg.audio.cues.lowBattery);
    uint8_t n = cfg.levels.count;
    for (uint8_t step = 0; step < n; step++) {
      uint8_t l = (uint8_t)((currentLevel + step) % n);
      for (uint8_t b = 0; b < sb::MAX_SOUND_PADS; b++) want(cfg.levels.levels[l].buttons[b].sound);
    }
    for (uint16_t i = 0; i < n_; i++) if (!e_[i].retired) e_[i].state = S_QUEUED;
  }
  scanCard();                                                  // takes the storage lock itself
  {
    Lock l(mutex_);
    for (uint16_t i = 0; i < n_; i++) {
      if (e_[i].retired) continue;
      bool listed = false;
      for (uint16_t k = 0; k < nOrder_; k++) if (order_[k] == i) { listed = true; break; }
      if (!listed && nOrder_ < MAX_FILES) order_[nOrder_++] = i;
      e_[i].state = S_QUEUED;
    }
  }
  running_ = true;
  BaseType_t ok = xTaskCreatePinnedToCore(&SoundCache::loaderThunk, "loader", 6 * 1024, this, 1, (TaskHandle_t*)&task_, 1);   // §19.1: core 1, priority 1, 6 KB
  if (ok != pdPASS) { running_ = false; LOG_E(TAG, "loader task not created"); }
}

// ---------------------------------------------------------------------------
// Loader task
// ---------------------------------------------------------------------------
void SoundCache::loaderThunk(void* arg) { static_cast<SoundCache*>(arg)->loaderMain(); vTaskDelete(NULL); }

void SoundCache::loaderMain() {
  uint32_t t0 = millis();
  uint16_t loaded = 0;
  for (uint16_t k = 0; k < nOrder_ && !cancel_; k++) {
    if (loadOne(order_[k])) loaded++;
    vTaskDelay(1);
  }
  if (!cancel_) LOG_I(TAG, "loader done: %u cached, %lu KB of %lu KB, %lu ms", (unsigned)loaded, (unsigned long)(used_ / 1024), (unsigned long)(budget_ / 1024), (unsigned long)(millis() - t0));
  else LOG_I(TAG, "loader cancelled");
  running_ = false;
}

struct FileCtx { File* f; };
static size_t fileRead(void* ctx, uint32_t off, void* dst, size_t n) {
  File* f = static_cast<FileCtx*>(ctx)->f;
  if (!f->seek(off)) return 0;
  return f->read(static_cast<uint8_t*>(dst), n);
}

bool SoundCache::loadOne(uint16_t idx) {
  char name[41]; uint16_t gen;
  { Lock l(mutex_); if (idx >= n_ || e_[idx].retired) return false; strlcpy(name, e_[idx].name, sizeof name); gen = generation_; e_[idx].state = S_LOADING; }
  char path[64]; snprintf(path, sizeof path, "/sounds/%s", name);
  sb::wav::Info info; sb::wav::Err err;
  uint32_t fileBytes = 0;
  {
    Storage::Guard g;
    File f = SD.open(path, FILE_READ);
    if (!f) { Lock l(mutex_); e_[idx].state = S_MISSING; strlcpy(e_[idx].reason, "file not found", sizeof e_[idx].reason); return false; }
    fileBytes = f.size();
    FileCtx ctx = { &f };
    err = sb::wav::parse(&fileRead, &ctx, fileBytes, info);
    f.close();
  }
  if (err != sb::wav::Err::None) { Lock l(mutex_); e_[idx].state = S_BAD; strlcpy(e_[idx].reason, sb::wav::errName(err), sizeof e_[idx].reason); LOG_W(TAG, "%s: %s", name, sb::wav::errName(err)); return false; }
  uint32_t canon = sb::wav::canonicalFrames(info);
  { Lock l(mutex_); e_[idx].fileBytes = fileBytes; e_[idx].rate = (uint16_t)info.rate; e_[idx].channels = (uint8_t)info.channels; e_[idx].frames = canon; }
  if (!canon) { Lock l(mutex_); e_[idx].state = S_BAD; strlcpy(e_[idx].reason, "size overflow", sizeof e_[idx].reason); return false; }
  uint64_t bytes = (uint64_t)canon * 2;
  if (bytes > 0xFFFFFFFFull || used_ + bytes > budget_) {
    Lock l(mutex_); e_[idx].state = S_STREAM; strlcpy(e_[idx].reason, "over the cache budget", sizeof e_[idx].reason);
    LOG_I(TAG, "%s: %lu KB does not fit the cache, streams", name, (unsigned long)(bytes / 1024));
    return false;
  }
  int16_t* out = static_cast<int16_t*>(heap_caps_malloc((size_t)bytes, MALLOC_CAP_SPIRAM));
  if (!out) { Lock l(mutex_); e_[idx].state = S_STREAM; strlcpy(e_[idx].reason, "PSRAM exhausted", sizeof e_[idx].reason); return false; }
  // Mono at the file rate lands in `mono` (== out when no resampling is needed).
  int16_t* mono = out;
  if (info.rate != sb::CANON_RATE) {
    mono = static_cast<int16_t*>(heap_caps_malloc((size_t)info.frames * 2, MALLOC_CAP_SPIRAM));
    if (!mono) { heap_caps_free(out); Lock l(mutex_); e_[idx].state = S_STREAM; strlcpy(e_[idx].reason, "PSRAM exhausted", sizeof e_[idx].reason); return false; }
  }
  static uint8_t chunk[READ_CHUNK];                            // one loader at a time
  uint32_t off = info.dataOffset, left = info.frames * info.channels * 2, outFrames = 0;
  bool ok = true;
  File f;
  { Storage::Guard g; f = SD.open(path, FILE_READ); if (!f || !f.seek(off)) ok = false; }
  while (ok && left && !cancel_) {
    size_t want = left < READ_CHUNK ? left : READ_CHUNK;
    want -= want % (info.channels * 2);
    int n;
    { Storage::Guard g; n = f.read(chunk, want); }
    if (n <= 0) { ok = false; break; }
    uint32_t frames = (uint32_t)n / (info.channels * 2);
    const int16_t* s = reinterpret_cast<const int16_t*>(chunk);
    if (info.channels == 1) { for (uint32_t i = 0; i < frames; i++) mono[outFrames + i] = s[i]; }
    else { for (uint32_t i = 0; i < frames; i++) mono[outFrames + i] = (int16_t)(((int32_t)s[2 * i] + s[2 * i + 1]) / 2); }
    outFrames += frames; left -= (uint32_t)n;
    vTaskDelay(1);                                             // §19.1: yield between reads
  }
  { Storage::Guard g; f.close(); }
  if (ok && !cancel_ && info.rate != sb::CANON_RATE) { sb::resampleMono(mono, outFrames, info.rate, out, canon); }
  if (mono != out) heap_caps_free(mono);
  Lock l(mutex_);
  if (!ok || cancel_ || gen != generation_) {
    heap_caps_free(out);
    e_[idx].state = ok ? S_QUEUED : S_BAD;
    if (!ok) strlcpy(e_[idx].reason, "read error", sizeof e_[idx].reason);
    return false;
  }
  e_[idx].pcm = out; e_[idx].frames = canon; e_[idx].state = S_CACHED; e_[idx].reason[0] = 0;
  used_ += (uint32_t)bytes;
  return true;
}

// ---------------------------------------------------------------------------
// Playback side
// ---------------------------------------------------------------------------
const SoundEntry* SoundCache::acquire(const char* name) {
  Lock l(mutex_);
  int i = findLocked(name);
  if (i < 0 || e_[i].state != S_CACHED || !e_[i].pcm) return nullptr;
  e_[i].refs++;
  return &e_[i];
}

void SoundCache::release(const SoundEntry* e) {
  if (!e) return;
  Lock l(mutex_);
  SoundEntry* w = const_cast<SoundEntry*>(e);
  if (w->refs) w->refs--;
  if (w->retired && w->refs == 0 && w->pcm) { heap_caps_free(w->pcm); w->pcm = nullptr; }
}

uint8_t SoundCache::stateOf(const char* name) const {
  Lock l(mutex_);
  int i = findLocked(name);
  return i < 0 ? S_UNKNOWN : e_[i].state;
}

uint16_t SoundCache::cachedCount() const {
  Lock l(mutex_);
  uint16_t c = 0;
  for (uint16_t i = 0; i < n_; i++) if (!e_[i].retired && e_[i].state == S_CACHED) c++;
  return c;
}

void SoundCache::list(Print& out) const {
  Lock l(mutex_);
  out.printf("cache: %u file(s), %lu KB of %lu KB, generation %u, loader %s\n", (unsigned)n_, (unsigned long)(used_ / 1024), (unsigned long)(budget_ / 1024), (unsigned)generation_, running_ ? "running" : "idle");
  for (uint16_t i = 0; i < n_; i++) {
    const SoundEntry& e = e_[i];
    if (e.retired) continue;
    out.printf("  %-24s %-8s %6.2f s  %u ch %5u Hz  %6lu KB%s%s\n", e.name, stateName(e.state), e.frames / 44100.0f, e.channels, e.rate,
               (unsigned long)(e.fileBytes / 1024), e.reason[0] ? "  " : "", e.reason);
  }
}
