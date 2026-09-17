// Native tests for the P mode model (FirmwareSpec.md §4.5).
// Run with: pio test -d firmware -e native -f test_pmode
#include <unity.h>
#include <math.h>
#include "app/pmode_model.h"

using namespace sb;

static PModeModel m;

void setUp() { m.configure(0.01f, 4.5f, true); }
void tearDown() {}

void test_sigma_follows_k() {
  // sigma = 0.5 * sqrt(k / (2 - k)): k = 0.01 -> 0.03544; the threshold is S sigma.
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.035444f, m.sigma());
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 4.5f * 0.035444f, m.threshold());
  m.configure(0.5f, 2.0f, true);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f * sqrtf(0.5f / 1.5f), m.sigma());
}

void test_starts_at_half_and_a_fair_word_moves_by_k() {
  for (uint8_t c = 0; c < 4; c++) TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, m.avg(c));
  TEST_ASSERT_EQUAL(-1, m.feed(0x1));                     // channel 0 sees a 1, the others a 0
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.505f, m.avg(0));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.495f, m.avg(1));
  TEST_ASSERT_EQUAL(1, m.samples());
}

void test_a_run_of_ones_fires_that_channel_after_the_expected_count() {
  // x_n = 1 - 0.5 (1 - k)^n; it crosses 0.5 + th when (1 - k)^n <= 1 - 2 th.
  m.configure(0.01f, 4.5f, false);                         // one-sided: the other channels' zeros do not count
  float th = m.threshold();
  int expected = (int)ceilf(logf(1.0f - 2.0f * th) / logf(1.0f - 0.01f));
  int fired = -1, n = 0;
  while (fired < 0 && n < 10000) { fired = m.feed(0x4); n++; }   // bit 2 = channel 2 (pad 3)
  TEST_ASSERT_EQUAL(2, fired);
  TEST_ASSERT_INT_WITHIN(1, expected, n);
  TEST_ASSERT_EQUAL(1, m.triggers(2));
  TEST_ASSERT_EQUAL(0, m.triggers(0));
}

void test_a_trigger_resets_every_channel() {
  for (int i = 0; i < 2000 && m.feed(0xF) < 0; i++) {}   // all four climb together; channel 0 is reported first
  for (uint8_t c = 0; c < 4; c++) TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, m.avg(c));
}

void test_two_sided_fires_on_a_run_of_zeros_one_sided_does_not() {
  int fired = -1;
  for (int i = 0; i < 2000 && fired < 0; i++) fired = m.feed(0xE);   // channel 0 sees only zeros, the rest only ones
  TEST_ASSERT_EQUAL(0, fired);                             // two-sided: the zeros win the race (same distance, lower channel)
  m.configure(0.01f, 4.5f, false);
  fired = -1;
  for (int i = 0; i < 2000 && fired < 0; i++) fired = m.feed(0xE);
  TEST_ASSERT_EQUAL(1, fired);                             // one-sided: channel 0's zeros never count; channel 1's ones do
  TEST_ASSERT_TRUE(m.avg(0) < 0.5f - m.threshold() || m.avg(0) == 0.5f);   // reset by channel 1's trigger
}

void test_configure_clears_the_counts() {
  for (int i = 0; i < 2000 && m.feed(0x1) < 0; i++) {}
  TEST_ASSERT_EQUAL(1, m.triggers(0));
  m.configure(0.02f, 3.0f, true);
  TEST_ASSERT_EQUAL(0, m.triggers(0));
  TEST_ASSERT_EQUAL(0, m.samples());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sigma_follows_k);
  RUN_TEST(test_starts_at_half_and_a_fair_word_moves_by_k);
  RUN_TEST(test_a_run_of_ones_fires_that_channel_after_the_expected_count);
  RUN_TEST(test_a_trigger_resets_every_channel);
  RUN_TEST(test_two_sided_fires_on_a_run_of_zeros_one_sided_does_not);
  RUN_TEST(test_configure_clears_the_counts);
  return UNITY_END();
}
