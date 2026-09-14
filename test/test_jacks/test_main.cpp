// Native tests for the output jacks (FirmwareSpec.md §10, §5.2 ownership).
#include <unity.h>
#include "app/jacks_model.h"
#include "config/loader.h"

using namespace sb;
void setUp() {} void tearDown() {}

static Config cfg2() { Config c; config::defaults(c); c.levels.count = 2; return c; }

void test_resolve() {
  Config c = cfg2();
  Entry e = c.levels.levels[0].buttons[0];
  e.jack = JackMode::Inherit; c.jacks.mode = JACKS_FOLLOW;
  TEST_ASSERT_EQUAL((int)JackResolved::Follow, (int)resolveJack(c, 0, &e));
  e.jack = JackMode::Pulse; TEST_ASSERT_EQUAL((int)JackResolved::Pulse, (int)resolveJack(c, 0, &e));
  e.jack = JackMode::Off;   TEST_ASSERT_EQUAL((int)JackResolved::Off, (int)resolveJack(c, 0, &e));
  e.jack = JackMode::Follow; c.levels.levels[1].jacks = false;
  TEST_ASSERT_EQUAL((int)JackResolved::Off, (int)resolveJack(c, 1, &e));      // the level flag suppresses everything
  c.jacks.mode = JACKS_OFF; e.jack = JackMode::Inherit;
  TEST_ASSERT_EQUAL((int)JackResolved::Off, (int)resolveJack(c, 0, &e));
  c.jacks.mode = JACKS_PULSE;
  TEST_ASSERT_EQUAL((int)JackResolved::Pulse, (int)resolveJack(c, 0, nullptr)); // the level pad: the global mode
}

void test_follow_opens_for_its_owner_only() {
  JacksModel j; j.configure(500, 10000);
  j.press(0, JackResolved::Follow, 7, 0);
  TEST_ASSERT_TRUE(j.closed(0)); TEST_ASSERT_EQUAL(1, j.mask()); TEST_ASSERT_EQUAL(7, j.owner(0));
  j.release(0, 8); TEST_ASSERT_TRUE(j.closed(0));               // an older press's release changes nothing
  j.release(0, 7); TEST_ASSERT_FALSE(j.closed(0)); TEST_ASSERT_EQUAL(0, j.mask());
}

void test_follow_max_and_unlimited() {
  JacksModel j; j.configure(500, 10000);
  j.press(1, JackResolved::Follow, 1, 1000);
  TEST_ASSERT_EQUAL(2, j.tick(10999)); TEST_ASSERT_EQUAL(0, j.tick(11000));
  j.configure(500, 0);
  j.press(0, JackResolved::Follow, 2, 0);
  TEST_ASSERT_EQUAL(1, j.tick(100000));                         // 0 = unlimited
  j.stuck(0); TEST_ASSERT_EQUAL(0, j.mask());                   // PadStuck opens it
}

void test_pulse_runs_to_its_end() {
  JacksModel j; j.configure(500, 10000);
  j.press(2, JackResolved::Pulse, 3, 0);
  j.release(2, 3); TEST_ASSERT_TRUE(j.closed(2));               // release does not cut a pulse
  TEST_ASSERT_EQUAL(4, j.tick(499)); TEST_ASSERT_EQUAL(0, j.tick(500));
  j.press(2, JackResolved::Pulse, 4, 600); j.press(2, JackResolved::Pulse, 4, 800);   // a repeat re-pulses
  TEST_ASSERT_EQUAL(4, j.tick(1299)); TEST_ASSERT_EQUAL(0, j.tick(1300));
}

void test_level_change_opens_follow_not_pulse() {
  JacksModel j; j.configure(500, 10000);
  j.press(0, JackResolved::Follow, 1, 0);
  j.press(1, JackResolved::Pulse, 2, 0);
  j.levelChanged();
  TEST_ASSERT_EQUAL(2, j.mask());
  TEST_ASSERT_EQUAL(0, j.tick(500));
}

void test_off_mode_all_off_and_test_closure() {
  JacksModel j; j.configure(500, 10000);
  j.press(3, JackResolved::Off, 1, 0); TEST_ASSERT_EQUAL(0, j.mask());
  j.press(0, JackResolved::Follow, 1, 0); j.press(1, JackResolved::Follow, 2, 0);
  TEST_ASSERT_EQUAL(3, j.mask());
  j.allOff(); TEST_ASSERT_EQUAL(0, j.mask());
  j.close(2, 1000, 0); TEST_ASSERT_TRUE(j.closed(2)); TEST_ASSERT_EQUAL(0, j.owner(2));
  TEST_ASSERT_EQUAL(4, j.tick(999)); TEST_ASSERT_EQUAL(0, j.tick(1000));
  j.press(9, JackResolved::Follow, 1, 0); TEST_ASSERT_EQUAL(0, j.mask());   // out of range ignored
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_resolve);
  RUN_TEST(test_follow_opens_for_its_owner_only);
  RUN_TEST(test_follow_max_and_unlimited);
  RUN_TEST(test_pulse_runs_to_its_end);
  RUN_TEST(test_level_change_opens_follow_not_pulse);
  RUN_TEST(test_off_mode_all_off_and_test_closure);
  return UNITY_END();
}
