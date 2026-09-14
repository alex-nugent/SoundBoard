#include "audio/engine.h"
#include "audio/wav.h"
#include "hal/pins.h"
#include "hal/storage.h"
#include "util/event.h"
#include "diag/log.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <string.h>

static const char* TAG = "audio";
static constexpr uint32_t BLOCK = 256;                     // frames per block (§6.4)
static constexpr uint32_t FADE_OUT = 128, FADE_IN = 64;
static constexpr uint32_t RING_BYTES = 32 * 1024;
static constexpr uint32_t STREAM_READ = 2048;
static constexpr uint32_t BOOST_MIN_MS = 450, BOOST_MAX_MS = 1000;

#define Q ((QueueHandle_t)queue_)
#define TX ((i2s_chan_handle_t)tx_)
static portMUX_TYPE s_slot = portMUX_INITIALIZER_UNLOCKED;   // the play-request slot is written by app, read by audio

// ---------------------------------------------------------------------------
// App-side API
// ---------------------------------------------------------------------------
bool AudioEngine::begin(SoundCache* cache, KcxLink* kcx) {
  cache_ = cache; kcx_ = kcx;
  if (!queue_) queue_ = xQueueCreate(8, sizeof(Cmd));
  if (!ring_) ring_ = static_cast<uint8_t*>(heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM));
  ringSize_ = ring_ ? RING_BYTES : 0;
  idleSince_ = millis();
  TaskHandle_t h = nullptr;
  BaseType_t ok = xTaskCreatePinnedToCore(&AudioEngine::taskThunk, "audio", 8 * 1024, this, 5, &h, 1);   // §19.1
  return ok == pdPASS && queue_ != nullptr;
}

void AudioEngine::railUp()   { Cmd c = Cmd::RailUp;   xQueueSend(Q, &c, portMAX_DELAY); }
void AudioEngine::railDown() { Cmd c = Cmd::RailDown; xQueueSend(Q, &c, portMAX_DELAY); }
void AudioEngine::stop(uint16_t)  { Cmd c = Cmd::Stop; xQueueSend(Q, &c, portMAX_DELAY); }

void AudioEngine::play(const char* name, float gain, uint16_t requestId) {
  Request r = {}; r.isTone = false; strlcpy(r.name, name ? name : "", sizeof r.name); r.gain = gain; r.requestId = requestId;
  portENTER_CRITICAL(&s_slot); pending_ = r; pendingValid_ = true; portEXIT_CRITICAL(&s_slot);   // a newer request replaces one not yet taken
}

void AudioEngine::tone(sb::ToneKind kind, float gain, uint16_t requestId) {
  Request r = {}; r.isTone = true; r.kind = kind; r.gain = gain; r.requestId = requestId;
  portENTER_CRITICAL(&s_slot); pending_ = r; pendingValid_ = true; portEXIT_CRITICAL(&s_slot);
}

void AudioEngine::setGain(float gain) { gainReq_ = gain; gainReqValid_ = true; }

bool AudioEngine::idleFor(uint32_t ms) const {
  if (src_ != Src::None || pendingValid_) return false;
  return (int32_t)(millis() - (idleSince_ + ms)) >= 0;
}

const char* AudioEngine::railName() const {
  switch (rail_) { case Rail::Down: return "down"; case Rail::Rising: return "rising"; case Rail::Up: return "up"; default: return "falling"; }
}

void AudioEngine::printStatus(Print& out) const {
  out.printf("audio: rail %s | I2S %s | playing %s (request %u%s, frame %lu of %lu) | underruns %lu (silent stalls %lu) | stream starved %lu | KCX %s%s%s\n",
             railName(), i2sFailed_ ? "FAILED" : i2sOn_ ? "on" : "off",
             src_ == Src::None ? "nothing" : src_ == Src::Cached ? "cached" : src_ == Src::Stream ? "streamed" : "tone", (unsigned)reqId_,
             pendingValid_ ? ", one pending" : "", (unsigned long)pos_, (unsigned long)frames_,
             (unsigned long)underruns_, (unsigned long)stalls_, (unsigned long)starved_,
             kcx_ ? (kcx_->powerOnSeen() ? "banner seen" : "no banner") : "-", kcx_ && kcx_->softOff() ? ", soft off" : "", kcx_ ? (kcx_->linked() ? ", linked" : "") : "");
}

// ---------------------------------------------------------------------------
// I2S (IDF driver, §6.4)
// ---------------------------------------------------------------------------
struct OvfCtx { volatile uint32_t* underruns; volatile uint32_t* stalls; volatile bool* sounding; };
static OvfCtx s_ovf;
static bool IRAM_ATTR onOvf(i2s_chan_handle_t, i2s_event_data_t*, void* ctx) {
  OvfCtx* c = static_cast<OvfCtx*>(ctx);
  if (*c->sounding) (*c->underruns)++; else (*c->stalls)++;   // §6.4: zero audible underruns is the criterion; silent stalls are flash writes
  return false;
}

bool AudioEngine::i2sStart() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 3; cc.dma_frame_num = BLOCK; cc.auto_clear = true;     // 17 ms of DMA (§6.4)
  i2s_chan_handle_t tx = nullptr;
  esp_err_t e = i2s_new_channel(&cc, &tx, NULL);
  if (e != ESP_OK) { LOG_E(TAG, "i2s_new_channel: %d", (int)e); i2sFailed_ = true; return false; }
  i2s_std_config_t sc = {};
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(44100);
  i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.clk_cfg = clk; sc.slot_cfg = slot;
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED; sc.gpio_cfg.bclk = (gpio_num_t)pins::I2S_BCLK; sc.gpio_cfg.ws = (gpio_num_t)pins::I2S_LRCLK;
  sc.gpio_cfg.dout = (gpio_num_t)pins::I2S_DIN; sc.gpio_cfg.din = I2S_GPIO_UNUSED;
  sc.gpio_cfg.invert_flags.mclk_inv = false; sc.gpio_cfg.invert_flags.bclk_inv = false; sc.gpio_cfg.invert_flags.ws_inv = false;
  e = i2s_channel_init_std_mode(tx, &sc);
  if (e != ESP_OK) { LOG_E(TAG, "i2s init std: %d", (int)e); i2s_del_channel(tx); i2sFailed_ = true; return false; }
  i2s_event_callbacks_t cbs = {}; cbs.on_send_q_ovf = &onOvf;
  s_ovf.underruns = &underruns_; s_ovf.stalls = &stalls_; s_ovf.sounding = &sounding_;
  i2s_channel_register_event_callback(tx, &cbs, &s_ovf);
  e = i2s_channel_enable(tx);
  if (e != ESP_OK) { LOG_E(TAG, "i2s enable: %d", (int)e); i2s_del_channel(tx); i2sFailed_ = true; return false; }
  // Bug 3a (brief §4.4): weakest drive on the three I2S lines after every enable.
  for (int8_t p : { pins::I2S_BCLK, pins::I2S_LRCLK, pins::I2S_DIN }) gpio_set_drive_capability((gpio_num_t)p, GPIO_DRIVE_CAP_0);
  tx_ = tx; i2sOn_ = true; i2sFailed_ = false;
  return true;
}

void AudioEngine::i2sStop() {
  if (tx_) { i2s_channel_disable(TX); i2s_del_channel(TX); tx_ = nullptr; }
  i2sOn_ = false;
  for (int8_t p : { pins::I2S_BCLK, pins::I2S_LRCLK, pins::I2S_DIN }) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }   // §2.3 rule 5: LOW while the rail is off
}

// ---------------------------------------------------------------------------
// Rail (§6.7, rule 5 of §2.3)
// ---------------------------------------------------------------------------
void AudioEngine::doRailUp() {
  if (rail_ == Rail::Up) return;
  rail_ = Rail::Rising;
  while (railDownAt_ && millis() - railDownAt_ < 500) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(10)); }   // §7.1: ~0.5 s off so the module boots afresh
  for (int8_t p : { pins::I2S_BCLK, pins::I2S_LRCLK, pins::I2S_DIN }) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  kcx_->begin();                                                  // UART before the rail
  digitalWrite(pins::BOOST, HIGH);
  uint32_t t0 = millis();
  LOG_I(TAG, "rail up: IO13 HIGH, waiting for the KCX banner");
  for (;;) {
    esp_task_wdt_reset();
    kcx_->feed();
    uint32_t el = millis() - t0;
    if (el >= BOOST_MIN_MS && (kcx_->powerOnSeen() || el >= BOOST_MAX_MS)) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  bool ok = i2sStart();
  rail_ = Rail::Up;
  idleSince_ = millis();
  LOG_I(TAG, "rail ready after %lu ms: I2S %s, KCX banner %s", (unsigned long)(millis() - t0), ok ? "on" : "FAILED", kcx_->powerOnSeen() ? "seen" : "not seen");
  Event e = { Ev::RailReady, millis(), {} }; e.u32 = (kcx_->powerOnSeen() ? 1u : 0u) | (ok ? 2u : 0u);
  EventBus::post(e);
}

void AudioEngine::doRailDown() {
  if (rail_ == Rail::Down) return;
  rail_ = Rail::Falling;
  endSource(true);
  pendingValid_ = false;
  i2sStop();
  digitalWrite(pins::AMP_EN, LOW);
  kcx_->end();                                                    // Serial1.end(), TX LOW
  digitalWrite(pins::BOOST, LOW);
  railDownAt_ = millis() ? millis() : 1;
  rail_ = Rail::Down;
  LOG_I(TAG, "rail down");
  Event e = { Ev::RailDown, millis(), {} };
  EventBus::post(e);
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
void AudioEngine::endSource(bool interrupted) {
  if (src_ == Src::None) return;
  if (src_ == Src::Cached && held_) { cache_->release(held_); held_ = nullptr; }
  if (src_ == Src::Stream && file_) {
    File* f = static_cast<File*>(file_); file_ = nullptr;
    if (Storage::tryLock(0)) { f->close(); delete f; Storage::unlock(); }
    else { if (closePending_) { /* an older one waits too: close it now, blocking is the lesser evil */ Storage::Guard g; static_cast<File*>(closePending_)->close(); delete static_cast<File*>(closePending_); } closePending_ = f; }
  }
  donePending_ = true; doneId_ = reqId_; doneInterrupted_ = interrupted;   // posted by the task after AudioStarted, in order
  src_ = Src::None; fadeOut_ = fadeIn_ = 0; started_ = false;
  idleSince_ = millis();
}

struct FileCtx { File* f; };
static size_t fileRead(void* ctx, uint32_t off, void* dst, size_t n) {
  File* f = static_cast<FileCtx*>(ctx)->f;
  if (!f->seek(off)) return 0;
  return f->read(static_cast<uint8_t*>(dst), n);
}

int AudioEngine::openStream(const char* name) {
  if (!ring_) return -1;
  char path[64]; snprintf(path, sizeof path, "/sounds/%s", name);
  if (!Storage::tryLock(2)) return 0;                              // the card is busy: try again next block
  File* f = new File(SD.open(path, FILE_READ));
  bool exists = f && *f, ok = exists;
  sb::wav::Info info; sb::wav::Err err = sb::wav::Err::Io;
  if (ok) { FileCtx ctx = { f }; err = sb::wav::parse(&fileRead, &ctx, f->size(), info); ok = err == sb::wav::Err::None; }
  if (ok) ok = f->seek(info.dataOffset);
  if (!ok) { if (f) { f->close(); delete f; } Storage::unlock(); LOG_W(TAG, "stream %s: %s", name, exists ? sb::wav::errName(err) : "file not found"); return -1; }
  Storage::unlock();
  file_ = f; fileLeft_ = info.frames * info.channels * 2; sChannels_ = (uint8_t)info.channels; sRate_ = info.rate; sEof_ = false;
  ringHead_ = ringTail_ = 0;
  rs_.begin(sRate_);
  frames_ = sb::wav::canonicalFrames(info);
  return 1;
}

void AudioEngine::refillStream() {
  if (!file_ || sEof_) return;
  uint32_t used = ringHead_ - ringTail_;
  while (ringSize_ - used >= STREAM_READ && fileLeft_) {
    uint32_t want = fileLeft_ < STREAM_READ ? fileLeft_ : STREAM_READ;
    want -= want % (sChannels_ * 2);
    if (!want) { sEof_ = true; break; }
    static uint8_t tmp[STREAM_READ];
    if (!Storage::tryLock(2)) return;                           // the screen or a save has the card: the ring has slack
    int n = static_cast<File*>(file_)->read(tmp, want);
    Storage::unlock();
    if (n <= 0) { sEof_ = true; break; }
    for (int i = 0; i < n; i++) ring_[(ringHead_ + i) % ringSize_] = tmp[i];
    ringHead_ += (uint32_t)n; fileLeft_ -= (uint32_t)n;
    used = ringHead_ - ringTail_;
  }
  if (!fileLeft_) sEof_ = true;
}

// Pulls canonical mono frames from the ring (converting on the way). Returns the frames produced; 0 at the end or when starved.
uint32_t AudioEngine::pullStream(int16_t* mono, uint32_t frames) {
  uint32_t avail = ringHead_ - ringTail_;
  uint32_t frameBytes = sChannels_ * 2;
  uint32_t inFrames = avail / frameBytes;
  if (!inFrames) return 0;
  // Convert up to `frames` (or a bit more for the resampler) input frames to mono into monoTmp_.
  uint32_t take = inFrames < 512 ? inFrames : 512;
  for (uint32_t i = 0; i < take; i++) {
    uint32_t b = (ringTail_ + i * frameBytes) % ringSize_;
    int16_t l = (int16_t)(ring_[b] | (ring_[(b + 1) % ringSize_] << 8));
    if (sChannels_ == 2) { int16_t r = (int16_t)(ring_[(b + 2) % ringSize_] | (ring_[(b + 3) % ringSize_] << 8)); monoTmp_[i] = (int16_t)(((int32_t)l + r) / 2); }
    else monoTmp_[i] = l;
  }
  uint32_t consumed = 0, n;
  if (rs_.passthrough()) { n = take < frames ? take : frames; for (uint32_t i = 0; i < n; i++) mono[i] = monoTmp_[i]; consumed = n; }
  else n = rs_.run(monoTmp_, take, consumed, mono, frames);
  ringTail_ += consumed * frameBytes;
  return n;
}

void AudioEngine::startRequest(const Request& r) {
  reqId_ = r.requestId; gain_ = r.gain; pos_ = 0; started_ = false; fadeIn_ = FADE_IN; fadeOut_ = 0;
  if (r.isTone) { tone_.begin(r.kind, 1.0f); src_ = Src::Tone; openTries_ = 0; return; }
  held_ = cache_ ? cache_->acquire(r.name) : nullptr;
  if (held_) { src_ = Src::Cached; frames_ = held_->frames; openTries_ = 0; return; }
  int o = openStream(r.name);
  if (o == 1) { src_ = Src::Stream; openTries_ = 0; refillStream(); return; }
  if (o == 0 && ++openTries_ < 40) {                                 // busy: keep the request for the next block (~230 ms at most)
    portENTER_CRITICAL(&s_slot); if (!pendingValid_) { pending_ = r; pendingValid_ = true; } portEXIT_CRITICAL(&s_slot);
    return;
  }
  openTries_ = 0;
  LOG_W(TAG, "\"%s\": %s, playing the missing-sound tone", r.name, o == 0 ? "card busy for too long" : "not on the card or unreadable");
  tone_.begin(sb::ToneKind::MissingSound, 1.0f); src_ = Src::Tone;
}

// One 256-frame block: source -> gain -> fades -> stereo.
void AudioEngine::produce(int16_t* stereo) {
  static int16_t mono[BLOCK];
  uint32_t n = 0;
  bool exhausted = false;
  if (src_ == Src::Cached && held_) {
    uint32_t left = frames_ - pos_;
    n = left < BLOCK ? left : BLOCK;
    memcpy(mono, held_->pcm + pos_, n * 2);
    pos_ += n; exhausted = pos_ >= frames_;
  } else if (src_ == Src::Stream) {
    n = pullStream(mono, BLOCK);
    if (!n && sEof_ && ringHead_ == ringTail_) exhausted = true;
    else if (!n) starved_++;
    pos_ += n;
  } else if (src_ == Src::Tone) {
    n = tone_.fill(mono, BLOCK);
    exhausted = tone_.done();
  }
  bool cut = false;
  uint32_t i = 0;
  for (; i < BLOCK; i++) {
    int32_t s = i < n ? mono[i] : 0;
    float g = gain_;
    if (fadeOut_) { g *= (float)fadeOut_ / FADE_OUT; fadeOut_--; if (!fadeOut_) cut = true; }
    else if (fadeIn_) { g *= 1.0f - (float)fadeIn_ / FADE_IN; fadeIn_--; }
    int32_t v = (int32_t)(s * g);
    if (v > 32767) v = 32767; if (v < -32768) v = -32768;
    stereo[2 * i] = stereo[2 * i + 1] = (int16_t)v;
    if (cut) { i++; break; }
  }
  for (; i < BLOCK; i++) stereo[2 * i] = stereo[2 * i + 1] = 0;
  if (cut) endSource(true);
  else if (exhausted) endSource(false);
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------
void AudioEngine::taskThunk(void* arg) { static_cast<AudioEngine*>(arg)->taskMain(); }

void AudioEngine::taskMain() {
  esp_task_wdt_add(NULL);                                        // §19.1
  static int16_t block[BLOCK * 2];
  for (;;) {
    esp_task_wdt_reset();
    Cmd c;
    while (xQueueReceive(Q, &c, 0) == pdTRUE) {
      switch (c) {
        case Cmd::RailUp:   doRailUp(); break;
        case Cmd::RailDown: doRailDown(); break;
        case Cmd::Stop:     if (src_ != Src::None) { fadeOut_ = FADE_OUT; fadeIn_ = 0; } pendingValid_ = false; break;
      }
    }
    if (!i2sOn_) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

    // A new request: interrupt what plays (fade), or start at once.
    if (pendingValid_ && src_ != Src::None && !fadeOut_) { fadeOut_ = FADE_OUT; fadeIn_ = 0; }
    if (pendingValid_ && src_ == Src::None) {
      Request r; portENTER_CRITICAL(&s_slot); r = pending_; pendingValid_ = false; portEXIT_CRITICAL(&s_slot);
      startRequest(r);
    }
    if (closePending_ && Storage::tryLock(0)) { static_cast<File*>(closePending_)->close(); delete static_cast<File*>(closePending_); closePending_ = nullptr; Storage::unlock(); }
    if (gainReqValid_) { gain_ = gainReq_; gainReqValid_ = false; }

    bool first = src_ != Src::None && !started_;
    uint16_t startedId = reqId_;
    if (src_ != Src::None) started_ = true;
    sounding_ = src_ != Src::None;
    if (src_ == Src::None) memset(block, 0, sizeof block); else produce(block);
    size_t written = 0;
    i2s_channel_write(TX, block, sizeof block, &written, 1000);    // blocks until a descriptor is free (17 ms max)
    if (first) { Event e = { Ev::AudioStarted, millis(), {} }; e.audio.requestId = startedId; e.audio.interrupted = false; EventBus::post(e); }
    if (donePending_) { donePending_ = false; Event e = { Ev::AudioDone, millis(), {} }; e.audio.requestId = doneId_; e.audio.interrupted = doneInterrupted_; EventBus::post(e); }
    if (src_ == Src::Stream) refillStream();
  }
}
