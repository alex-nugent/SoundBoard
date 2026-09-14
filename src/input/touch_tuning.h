// Touch detection constants (FirmwareSpec.md §4.1 table). The three
// percentages that are also configuration (press, release, separation) come
// from the Config; everything else lives here, identical to validations 01-12.
#pragma once
#include <stdint.h>

namespace touch_tuning {

constexpr uint16_t POLL_MS           = 15;      // app-task tick for the pads
constexpr uint8_t  CONFIRM_SAMPLES   = 2;       // consecutive samples to confirm an edge
constexpr uint8_t  BASELINE_SAMPLES  = 24;      // calibration averaging
constexpr uint8_t  DRIFT_SHIFT       = 7;       // baseline follows the reading by 1/128 per idle sample
constexpr float    DRIFT_BELOW_PCT   = 0.75f;   // only while |delta| is below this
constexpr uint16_t SETTLE_MS         = 2000;    // hands-off wait before a fresh calibration
constexpr uint8_t  CHECK_SCANS       = 4;       // scans averaged for the boot check of restored baselines
constexpr uint8_t  CHECK_SKIP_MAX    = 40;      // unusable scans (warm-up garbage, or far off the copy) before a fresh calibration (~600 ms)
constexpr float    CHECK_SHIFT_MAX   = 10.0f;   // a common shift up to this % is applied; more forces a calibration
constexpr uint16_t REBASELINE_MS     = 5000;    // a pad parked off its baseline snaps to the reading after this
constexpr uint32_t NVS_MAGIC         = 0x54434131;   // "TCA1": the sb-cal/touch blob layout

}  // namespace touch_tuning
