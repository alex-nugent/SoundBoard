#include "app/haptics_model.h"

namespace sb {

static inline bool dueAt(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

uint8_t levelPattern(const Config& c, uint8_t levelIdx, uint16_t* out, uint8_t max) {
  if (!c.vibration.enabled || levelIdx >= c.levels.count || max == 0) return 0;
  const Level& L = c.levels.levels[levelIdx];
  uint8_t n = 0;
  if (L.hasVibration && L.vibrationLen) {                      // the level's own array wins
    for (uint8_t i = 0; i < L.vibrationLen && n < max && i < MAX_PATTERN; i++) out[n++] = L.vibration[i];
    return n;
  }
  switch (c.levelChange.vibration.mode) {
    case VIB_COUNT: {                                          // N × (pulse, gap) without the trailing gap
      uint8_t pulses = levelIdx + 1;
      for (uint8_t p = 0; p < pulses && n < max; p++) {
        out[n++] = c.levelChange.vibration.pulseMs;
        if (p + 1 < pulses && n < max) out[n++] = c.levelChange.vibration.gapMs;
      }
      return n;
    }
    case VIB_PATTERN:
      for (uint8_t i = 0; i < c.levelChange.vibration.patternLen && n < max && i < MAX_PATTERN; i++) out[n++] = c.levelChange.vibration.pattern[i];
      return n;
    default: return 0;
  }
}

bool resolveVibrate(const Config& c, const Entry& e) {
  if (!c.vibration.enabled) return false;
  if (e.vibrate == Tri::On) return true;
  if (e.vibrate == Tri::Off) return false;
  return c.vibration.confirmPulse;
}

// --- HapticsModel ----------------------------------------------------------

uint32_t HapticsModel::onInWindow(uint32_t now) const {
  uint32_t sum = 0;
  for (uint8_t k = 0; k < histN_; k++) {
    const OnRec& r = hist_[k];
    uint32_t end = r.start + r.ms;
    if ((int32_t)(end - (now - HAPTICS_WINDOW_MS)) <= 0) continue;   // ended before the window
    uint32_t from = r.start;
    if ((int32_t)(from - (now - HAPTICS_WINDOW_MS)) < 0) from = now - HAPTICS_WINDOW_MS;
    sum += end - from;
  }
  return sum;
}

void HapticsModel::noteOn(uint32_t start, uint16_t ms) {
  hist_[histHead_] = { start, ms };
  histHead_ = (uint8_t)((histHead_ + 1) % 32);
  if (histN_ < 32) histN_++;
}

void HapticsModel::startSegment(uint32_t start) {
  onPhase_ = (i_ % 2) == 0;
  uint16_t len = seg_[i_];
  if (onPhase_) {
    uint32_t used = onInWindow(start);
    uint32_t allowed = used >= HAPTICS_WINDOW_ON_CAP_MS ? 0 : HAPTICS_WINDOW_ON_CAP_MS - used;
    if (len > allowed) { len = (uint16_t)allowed; clipped_ = true; }
    if (len) noteOn(start, len);
    else { n_ = 0; duty_ = 0; return; }                        // nothing left in the budget: the rest is dropped
  }
  segEnd_ = start + len;
}

void HapticsModel::load(const uint16_t* onOff, uint8_t n, uint32_t now, bool isPattern) {
  if (n > MAX_PATTERN) n = MAX_PATTERN;
  uint32_t total = 0; uint8_t m = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint16_t v = onOff[i];
    bool on = (i % 2) == 0;
    if (on && v > HAPTICS_MAX_ON_MS) v = HAPTICS_MAX_ON_MS;                     // §9.4 single on segment
    if (!on && i + 1 < n && v < HAPTICS_MIN_GAP_MS) v = HAPTICS_MIN_GAP_MS;     // §9.4 gap between on segments
    if (total + v > maxPatternMs_) {                                            // §9.3 truncation
      v = (uint16_t)(maxPatternMs_ - total);
      if (v == 0 || !on) break;                                                 // an off tail is pointless
      seg_[m++] = v; total += v; break;
    }
    seg_[m++] = v; total += v;
  }
  while (m && (m % 2) == 0 && seg_[m - 1] == 0) m--;           // drop a zero off tail
  n_ = m; i_ = 0; isPattern_ = isPattern;
  if (!n_) { duty_ = 0; return; }
  startSegment(now);
}

bool HapticsModel::pulse(uint16_t ms, uint32_t now) {
  if (patternRunning()) return false;                          // §5.5: never interrupts a level pattern
  uint16_t one[1] = { ms };
  load(one, 1, now, false);
  return true;
}

void HapticsModel::pattern(const uint16_t* onOff, uint8_t n, uint32_t now) { load(onOff, n, now, true); }

void HapticsModel::stop() { n_ = 0; i_ = 0; duty_ = 0; onPhase_ = false; }

uint8_t HapticsModel::tick(uint32_t now) {
  while (n_ && dueAt(now, segEnd_)) {                           // one or more segments ended
    uint32_t start = segEnd_;
    if (++i_ >= n_) { n_ = 0; break; }
    startSegment(start);
  }
  duty_ = (n_ && onPhase_) ? (uint8_t)((255u * strength_ + 50) / 100) : 0;
  return duty_;
}

uint32_t HapticsModel::remainingMs(uint32_t now) const {
  if (!n_) return 0;
  uint32_t rem = dueAt(now, segEnd_) ? 0 : segEnd_ - now;
  for (uint8_t k = i_ + 1; k < n_; k++) rem += seg_[k];
  return rem;
}

}  // namespace sb
