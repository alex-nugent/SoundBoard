// Native tests for the battery model (FirmwareSpec.md §12.1).
#include <unity.h>
#include "power/battery_model.h"

using namespace sb;
void setUp() {} void tearDown() {}

void test_table_endpoints_and_rows() {
  TEST_ASSERT_EQUAL(100, socFromMv(4250));
  TEST_ASSERT_EQUAL(100, socFromMv(4200));
  TEST_ASSERT_EQUAL(0, socFromMv(3500));
  TEST_ASSERT_EQUAL(0, socFromMv(3000));
  TEST_ASSERT_EQUAL(50, socFromMv(3840));
  TEST_ASSERT_EQUAL(15, socFromMv(3710));
  TEST_ASSERT_EQUAL(5, socFromMv(3610));
}

void test_table_interpolates() {
  TEST_ASSERT_EQUAL(98, socFromMv(4175));        // halfway between 4150 (95) and 4200 (100) -> 97.5 -> 98
  TEST_ASSERT_EQUAL(3, socFromMv(3555));         // halfway between 3500 (0) and 3610 (5)
  uint8_t last = 100;
  for (uint16_t mv = 4200; mv >= 3500; mv -= 5) { uint8_t p = socFromMv(mv); TEST_ASSERT_TRUE(p <= last); last = p; }   // monotonic table
}

void test_filter_median_rejects_a_spike() {
  BatteryFilter f; f.reset();
  f.push(3900); f.push(3900); f.push(3900);
  TEST_ASSERT_EQUAL(3900, f.filtered());
  f.push(4300);                                   // a single spike (a load released, or noise)
  TEST_ASSERT_EQUAL(3900, f.filtered());          // the median ignores it
  f.push(4300); f.push(4300);                     // three in a row: it is real, the EMA follows at 1/4 per step
  TEST_ASSERT_TRUE(f.filtered() > 3900 && f.filtered() < 4300);
}

void test_percent_never_rises_on_battery() {
  BatteryFilter f; f.reset();
  for (int i = 0; i < 6; i++) f.push(3800);
  TEST_ASSERT_EQUAL(40, f.percent(false));
  for (int i = 0; i < 12; i++) f.push(3900);      // the voltage recovers after a load
  TEST_ASSERT_EQUAL(40, f.percent(false));        // the displayed percentage does not
  TEST_ASSERT_TRUE(f.percent(true) > 40);         // on USB it may rise
  for (int i = 0; i < 12; i++) f.push(3700);
  TEST_ASSERT_TRUE(f.percent(false) < 40);        // and it still falls
}

void test_seed_restores_across_sleep() {
  BatteryFilter f; f.reset();
  f.seed(3850, 55);
  TEST_ASSERT_TRUE(f.primed());
  TEST_ASSERT_EQUAL(3850, f.filtered());
  TEST_ASSERT_EQUAL(55, f.percent(false));
  f.push(3950); f.push(3950); f.push(3950);       // a lighter load after the wake reads higher
  TEST_ASSERT_EQUAL(55, f.percent(false));        // no climb across the sleep
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_table_endpoints_and_rows);
  RUN_TEST(test_table_interpolates);
  RUN_TEST(test_filter_median_rejects_a_spike);
  RUN_TEST(test_percent_never_rises_on_battery);
  RUN_TEST(test_seed_restores_across_sleep);
  return UNITY_END();
}
