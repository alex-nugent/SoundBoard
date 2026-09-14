// The volume model (FirmwareSpec.md §6.2). Pure: master volume in steps,
// mute, and the gains the audio engine multiplies samples by. Persistence
// (NVS after 2 s of silence) is the app's job; `dirty()` says when.
#pragma once
#include <stdint.h>

namespace sb {

class Volume {
 public:
  void configure(float maxGain, uint8_t stepPct, uint8_t clickVolume) { maxGain_ = maxGain; step_ = stepPct ? stepPct : 10; click_ = clickVolume > 5 ? 5 : clickVolume; }
  void init(uint8_t masterPct, bool muted) { master_ = clamp(masterPct); muted_ = muted; dirty_ = false; }

  uint8_t master() const { return master_; }
  bool    muted() const { return muted_; }
  uint8_t stepPct() const { return step_; }

  // Attendant click / VolumeUp / VolumeDown: one step, clamped; un-mutes. Returns true if anything changed.
  bool step(int dir);
  bool set(uint8_t pct);
  bool mute(bool on);

  // Gains (§6.2): entry sounds scale with the master volume and the entry's own volume; cues use the click volume.
  float gainFor(uint8_t entryVolPct) const { return muted_ ? 0.0f : maxGain_ * master_ / 100.0f * (entryVolPct > 100 ? 100 : entryVolPct) / 100.0f; }
  float clickGain() const { return muted_ ? 0.0f : maxGain_ * click_ / 5.0f; }
  float volumeActionClickGain() const { return maxGain_ * master_ / 100.0f; }   // the confirmation click of a volume action follows the master volume, even when un-muting
  float unmuteClickGain() const { return maxGain_ * click_ / 5.0f; }

  bool dirty() const { return dirty_; }       // master changed since the last clearDirty()
  void clearDirty() { dirty_ = false; }

 private:
  static uint8_t clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); }
  float   maxGain_ = 0.5f;
  uint8_t step_ = 10, click_ = 2;
  uint8_t master_ = 60;
  bool    muted_ = false, dirty_ = false;
};

}  // namespace sb
