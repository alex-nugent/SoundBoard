// Native tests for the vibration scheduler (FirmwareSpec.md §9).
#include <unity.h>
#include "app/haptics_model.h"
#include "config/loader.h"

using namespace sb;
void setUp() {} void tearDown() {}

static Config cfg4() { Config c; config::defaults(c); c.levels.count = 4; return c; }

void test_level_pattern_count() {
  Config c = cfg4();
  uint16_t out[MAX_PATTERN];
  TEST_ASSERT_EQUAL(1, levelPattern(c, 0, out, MAX_PATTERN)); TEST_ASSERT_EQUAL(150, out[0]);
  TEST_ASSERT_EQUAL(5, levelPattern(c, 2, out, MAX_PATTERN));
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL(150, out[i]);
  TEST_ASSERT_EQUAL(0, levelPattern(c, 4, out, MAX_PATTERN));   // beyond the count
}

void test_level_pattern_override_and_modes() {
  Config c = cfg4();
  uint16_t out[MAX_PATTERN];
  c.levels.levels[1].hasVibration = true; c.levels.levels[1].vibrationLen = 3;
  c.levels.levels[1].vibration[0] = 300; c.levels.levels[1].vibration[1] = 100; c.levels.levels[1].vibration[2] = 300;
  TEST_ASSERT_EQUAL(3, levelPattern(c, 1, out, MAX_PATTERN)); TEST_ASSERT_EQUAL(300, out[0]); TEST_ASSERT_EQUAL(100, out[1]);
  TEST_ASSERT_EQUAL(1, levelPattern(c, 0, out, MAX_PATTERN));   // other levels keep the mode
  c.levelChange.vibration.mode = VIB_PATTERN; c.levelChange.vibration.patternLen = 3;
  c.levelChange.vibration.pattern[0] = 500; c.levelChange.vibration.pattern[1] = 200; c.levelChange.vibration.pattern[2] = 500;
  TEST_ASSERT_EQUAL(3, levelPattern(c, 3, out, MAX_PATTERN)); TEST_ASSERT_EQUAL(500, out[2]);
  c.levelChange.vibration.mode = VIB_NONE;
  TEST_ASSERT_EQUAL(0, levelPattern(c, 3, out, MAX_PATTERN));
  TEST_ASSERT_EQUAL(3, levelPattern(c, 1, out, MAX_PATTERN));   // the override still plays
  c.vibration.enabled = false;
  TEST_ASSERT_EQUAL(0, levelPattern(c, 1, out, MAX_PATTERN));
}

void test_resolve_vibrate() {
  Config c = cfg4();
  Entry e = c.levels.levels[0].buttons[0];
  e.vibrate = Tri::Inherit; c.vibration.confirmPulse = false; TEST_ASSERT_FALSE(resolveVibrate(c, e));
  c.vibration.confirmPulse = true;  TEST_ASSERT_TRUE(resolveVibrate(c, e));
  e.vibrate = Tri::Off;             TEST_ASSERT_FALSE(resolveVibrate(c, e));
  e.vibrate = Tri::On; c.vibration.confirmPulse = false; TEST_ASSERT_TRUE(resolveVibrate(c, e));
  c.vibration.enabled = false;      TEST_ASSERT_FALSE(resolveVibrate(c, e));
}

void test_pulse_timing_and_strength() {
  HapticsModel h; h.configure(100, 4000);
  TEST_ASSERT_EQUAL(0, h.tick(0));
  TEST_ASSERT_TRUE(h.pulse(200, 1000));
  TEST_ASSERT_EQUAL(255, h.tick(1000)); TEST_ASSERT_EQUAL(255, h.tick(1199));
  TEST_ASSERT_EQUAL(0, h.tick(1200)); TEST_ASSERT_FALSE(h.active());
  h.configure(60, 4000); h.pulse(100, 2000);
  TEST_ASSERT_EQUAL(153, h.tick(2000));
  h.configure(20, 4000); h.pulse(100, 3000);
  TEST_ASSERT_EQUAL(51, h.tick(3000));
}

void test_pattern_replaces_pulse_and_pulse_is_skipped() {
  HapticsModel h; h.configure(100, 4000);
  h.pulse(1000, 0);
  uint16_t p[3] = { 150, 150, 150 };
  h.pattern(p, 3, 100);
  TEST_ASSERT_TRUE(h.patternRunning());
  TEST_ASSERT_EQUAL(255, h.tick(100));
  TEST_ASSERT_FALSE(h.pulse(200, 200));                       // §5.5: skipped while the pattern runs
  TEST_ASSERT_EQUAL(0, h.tick(250)); TEST_ASSERT_EQUAL(0, h.tick(399));
  TEST_ASSERT_EQUAL(255, h.tick(400)); TEST_ASSERT_EQUAL(255, h.tick(549));
  TEST_ASSERT_EQUAL(0, h.tick(550)); TEST_ASSERT_FALSE(h.active());
  TEST_ASSERT_TRUE(h.pulse(200, 600));                        // allowed again afterwards
}

void test_min_gap_and_max_on() {
  HapticsModel h; h.configure(100, 10000);
  uint16_t p[3] = { 3000, 10, 3000 };
  h.pattern(p, 3, 0);
  TEST_ASSERT_EQUAL(4050, h.remainingMs(0));                  // 2000 + 50 + 2000
  TEST_ASSERT_EQUAL(255, h.tick(1999)); TEST_ASSERT_EQUAL(0, h.tick(2000));
  TEST_ASSERT_EQUAL(0, h.tick(2049));   TEST_ASSERT_EQUAL(255, h.tick(2050));
  TEST_ASSERT_EQUAL(255, h.tick(4049)); TEST_ASSERT_EQUAL(0, h.tick(4050));
  TEST_ASSERT_FALSE(h.active());
}

void test_truncation_at_max_pattern() {
  HapticsModel h; h.configure(100, 1000);
  uint16_t p[5] = { 400, 400, 400, 400, 400 };
  h.pattern(p, 5, 0);
  TEST_ASSERT_EQUAL(1000, h.remainingMs(0));
  TEST_ASSERT_EQUAL(255, h.tick(0)); TEST_ASSERT_EQUAL(0, h.tick(400));
  TEST_ASSERT_EQUAL(255, h.tick(800)); TEST_ASSERT_EQUAL(255, h.tick(999));
  TEST_ASSERT_EQUAL(0, h.tick(1000)); TEST_ASSERT_FALSE(h.active());
}

void test_window_cap_clips_and_recovers() {
  HapticsModel h; h.configure(100, 10000);
  uint16_t p[5] = { 2000, 50, 2000, 50, 2000 };                // 6 s of on-time asked, 5 s allowed
  h.pattern(p, 5, 0);
  TEST_ASSERT_EQUAL(255, h.tick(0)); TEST_ASSERT_EQUAL(0, h.tick(2000));
  TEST_ASSERT_EQUAL(255, h.tick(2050)); TEST_ASSERT_EQUAL(0, h.tick(4050));
  TEST_ASSERT_EQUAL(255, h.tick(4100)); TEST_ASSERT_EQUAL(255, h.tick(5099));
  TEST_ASSERT_EQUAL(0, h.tick(5100)); TEST_ASSERT_FALSE(h.active()); TEST_ASSERT_TRUE(h.clipped());
  h.clearClipped();
  uint16_t one[1] = { 2000 };
  h.pattern(one, 1, 6000);                                      // the window is full: dropped
  TEST_ASSERT_FALSE(h.active()); TEST_ASSERT_TRUE(h.clipped()); TEST_ASSERT_EQUAL(0, h.tick(6000));
  h.clearClipped();
  TEST_ASSERT_TRUE(h.pulse(2000, 12100));                       // window 2100..12100 holds 1950 + 1000 = 2950 ms: room for 2000
  TEST_ASSERT_EQUAL(255, h.tick(12100)); TEST_ASSERT_EQUAL(255, h.tick(14099)); TEST_ASSERT_EQUAL(0, h.tick(14100));
  TEST_ASSERT_FALSE(h.clipped());
}

void test_remaining_and_stop() {
  HapticsModel h; h.configure(100, 4000);
  uint16_t p[3] = { 500, 500, 500 };
  h.pattern(p, 3, 0);
  TEST_ASSERT_EQUAL(1500, h.remainingMs(0));
  h.tick(600);
  TEST_ASSERT_EQUAL(900, h.remainingMs(600));
  h.stop();
  TEST_ASSERT_FALSE(h.active()); TEST_ASSERT_EQUAL(0, h.remainingMs(600)); TEST_ASSERT_EQUAL(0, h.tick(700));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_level_pattern_count);
  RUN_TEST(test_level_pattern_override_and_modes);
  RUN_TEST(test_resolve_vibrate);
  RUN_TEST(test_pulse_timing_and_strength);
  RUN_TEST(test_pattern_replaces_pulse_and_pulse_is_skipped);
  RUN_TEST(test_min_gap_and_max_on);
  RUN_TEST(test_truncation_at_max_pattern);
  RUN_TEST(test_window_cap_clips_and_recovers);
  RUN_TEST(test_remaining_and_stop);
  return UNITY_END();
}
