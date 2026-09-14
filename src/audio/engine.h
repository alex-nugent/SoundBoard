// The audio engine (FirmwareSpec.md §6.4, §6.7): the `audio` task (core 1,
// priority 5, 8 KB) owns the I2S port and the 5 V rail. One 256-frame block
// per iteration, 3 x 256 DMA ring, silence when idle (keep-alive), fades on
// interruption, cached sounds from PSRAM, uncached ones streamed through a
// 32 KB ring, built-in tones. Commands come from the app task only.
#pragma once
#include <stdint.h>
#include <Print.h>
#include "audio/cache.h"
#include "audio/tones.h"
#include "audio/resample.h"
#include "bt/kcx.h"

class AudioEngine {
 public:
  enum class Rail : uint8_t { Down, Rising, Up, Falling };

  bool begin(SoundCache* cache, KcxLink* kcx);       // creates the task; the rail stays down until railUp()
  void railUp();                                      // §6.7: UART, IO13, banner wait, I2S, RailReady
  void railDown();                                    // I2S off, pins LOW, amp off, UART parked, IO13 LOW, RailDown
  void play(const char* name, float gain, uint16_t requestId);   // replaces any pending request; interrupts what plays (fade 128)
  void tone(sb::ToneKind kind, float gain, uint16_t requestId);
  void stop(uint16_t fadeFrames = 128);
  void setGain(float gain);                           // live, coalesced

  Rail     rail() const { return rail_; }
  const char* railName() const;
  bool     ready() const { return rail_ == Rail::Up && i2sOn_; }
  bool     i2sFailed() const { return i2sFailed_; }
  bool     playing() const { return src_ != Src::None || pendingValid_; }
  bool     idleFor(uint32_t ms) const;               // for Storage::deferredWrite
  uint32_t underruns() const { return underruns_; }  // DMA refills missed while a sound was playing (audible)
  uint32_t silentStalls() const { return stalls_; }  // the same during silence (a flash write stalling the cores; inaudible)
  uint32_t starved() const { return starved_; }      // streaming ring ran dry
  uint16_t currentRequest() const { return reqId_; }
  bool     streaming() const { return src_ == Src::Stream; }
  void     printStatus(Print& out) const;

 private:
  enum class Cmd : uint8_t { RailUp, RailDown, Stop };
  enum class Src : uint8_t { None, Cached, Stream, Tone };
  struct Request { bool isTone; char name[41]; sb::ToneKind kind; float gain; uint16_t requestId; };

  static void taskThunk(void* arg);
  void taskMain();
  void doRailUp();
  void doRailDown();
  bool i2sStart();
  void i2sStop();
  void startRequest(const Request& r);
  int  openStream(const char* name);                // 1 ok, 0 busy (retry), -1 failed
  void refillStream();
  uint32_t pullStream(int16_t* mono, uint32_t frames);
  void endSource(bool interrupted);
  void produce(int16_t* stereo);

  SoundCache* cache_ = nullptr;
  KcxLink*    kcx_ = nullptr;
  void*       queue_ = nullptr;                       // Cmd queue (depth 8)
  void*       tx_ = nullptr;                          // i2s_chan_handle_t
  volatile Rail rail_ = Rail::Down;
  volatile bool i2sOn_ = false, i2sFailed_ = false;
  volatile uint32_t underruns_ = 0, stalls_ = 0, starved_ = 0;
  volatile bool sounding_ = false;                    // a source is being rendered (read by the DMA callback)
  // play request slot (app -> audio), replaced by a newer request until taken
  Request  pending_ = {};
  volatile bool pendingValid_ = false;
  volatile float gainReq_ = 0; volatile bool gainReqValid_ = false;
  uint8_t  openTries_ = 0;
  uint32_t railDownAt_ = 0;                          // a RailUp waits 500 ms after a RailDown so the KCX really reboots (§7.1)
  bool     donePending_ = false, doneInterrupted_ = false;
  uint16_t doneId_ = 0;
  void*    closePending_ = nullptr;                  // a stream File* to close once the card is free
  // current source
  Src      src_ = Src::None;
  uint16_t reqId_ = 0;
  float    gain_ = 0;
  const SoundEntry* held_ = nullptr;                  // cached buffer in use
  uint32_t pos_ = 0, frames_ = 0;
  sb::ToneGen tone_;
  uint32_t fadeIn_ = 0, fadeOut_ = 0;                 // frames left
  bool     started_ = false;
  volatile uint32_t idleSince_ = 0;
  // streaming
  uint8_t* ring_ = nullptr;                           // 32 KB PSRAM, raw file bytes
  uint32_t ringHead_ = 0, ringTail_ = 0, ringSize_ = 0;
  void*    file_ = nullptr;                           // File*
  uint32_t fileLeft_ = 0;
  uint8_t  sChannels_ = 1; uint32_t sRate_ = 44100;
  bool     sEof_ = false;
  sb::LinearResampler rs_;
  int16_t  monoTmp_[512];
};
