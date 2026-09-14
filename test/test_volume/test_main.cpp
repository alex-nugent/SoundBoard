// Native tests for the volume model and the tone generator (FirmwareSpec.md §6.2, §6.6).
#include <unity.h>
#include "audio/volume.h"
#include "audio/tones.h"

using namespace sb;
void setUp() {} void tearDown() {}

void test_steps_and_clamps() {
  Volume v; v.configure(0.5f, 10, 2); v.init(60, false);
  TEST_ASSERT_TRUE(v.step(+1)); TEST_ASSERT_EQUAL(70, v.master());
  for (int i = 0; i < 5; i++) v.step(+1);
  TEST_ASSERT_EQUAL(100, v.master());
  TEST_ASSERT_FALSE(v.step(+1));                 // clamped: nothing changed
  for (int i = 0; i < 12; i++) v.step(-1);
  TEST_ASSERT_EQUAL(0, v.master());
  TEST_ASSERT_TRUE(v.dirty());
}

void test_gains() {
  Volume v; v.configure(0.5f, 10, 2); v.init(60, false);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f, v.gainFor(100));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.15f, v.gainFor(50));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.20f, v.clickGain());          // maxGain x clickVolume / 5, independent of master
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.30f, v.volumeActionClickGain());
  v.mute(true);
  TEST_ASSERT_EQUAL(0, (int)(v.gainFor(100) * 1000));
  TEST_ASSERT_EQUAL(0, (int)(v.clickGain() * 1000));
  TEST_ASSERT_TRUE(v.step(+1));                  // any volume change un-mutes
  TEST_ASSERT_FALSE(v.muted()); TEST_ASSERT_EQUAL(70, v.master());
}

void test_tone_lengths() {
  ToneGen t; int16_t buf[512]; uint32_t total;
  t.begin(ToneKind::MissingSound, 0.5f); total = 0; for (uint32_t n; (n = t.fill(buf, 512)) > 0;) total += n;
  TEST_ASSERT_EQUAL(44100 * 120 / 1000, total);
  t.begin(ToneKind::Click, 0.5f); total = 0; for (uint32_t n; (n = t.fill(buf, 512)) > 0;) total += n;
  TEST_ASSERT_EQUAL(44100 * 15 / 1000, total);
  t.begin(ToneKind::Startup, 0.5f); total = 0; for (uint32_t n; (n = t.fill(buf, 512)) > 0;) total += n;
  TEST_ASSERT_EQUAL(44100 * 600 / 1000, total);
  t.begin(ToneKind::Fault, 1.0f); total = 0; int16_t peak = 0;
  for (uint32_t n; (n = t.fill(buf, 512)) > 0;) { total += n; for (uint32_t i = 0; i < n; i++) if (buf[i] > peak) peak = buf[i]; }
  TEST_ASSERT_EQUAL(44100 * 500 / 1000, total);
  TEST_ASSERT_INT_WITHIN(600, 18000, peak);       // 9000 x gain x 2
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_steps_and_clamps);
  RUN_TEST(test_gains);
  RUN_TEST(test_tone_lengths);
  return UNITY_END();
}
