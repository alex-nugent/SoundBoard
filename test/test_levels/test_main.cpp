// Native tests for the level controller (FirmwareSpec.md §5.3, §4.2, §5.4).
#include <unity.h>
#include "app/levels.h"

using namespace sb;
void setUp() {} void tearDown() {}

static LevelController make(uint8_t count, LevelPolicy p = LevelPolicy()) { LevelController l; l.configure(p, count); l.onInput(0); return l; }

void test_pad_wraps_forward() {
  LevelController l = make(4);
  for (uint8_t i = 1; i <= 8; i++) {
    LevelChange c = l.next(LevelSource::Pad);
    TEST_ASSERT_EQUAL(i % 4, c.level); TEST_ASSERT_EQUAL(i % 4, l.current());
    TEST_ASSERT_TRUE(c.changed); TEST_ASSERT_TRUE(c.cues);
  }
}

void test_prev_wraps_to_last() {                            // [OPEN 18], drafted: wrap
  LevelController l = make(4);
  LevelChange c = l.prev(LevelSource::Attendant);
  TEST_ASSERT_EQUAL(3, c.level); TEST_ASSERT_TRUE(c.changed); TEST_ASSERT_TRUE(c.cues);
}

void test_single_level_pad_still_clicks() {                 // §5.3 step 3 is unconditional for the pad
  LevelController l = make(1);
  LevelChange c = l.next(LevelSource::Pad);
  TEST_ASSERT_FALSE(c.changed); TEST_ASSERT_TRUE(c.cues); TEST_ASSERT_EQUAL(0, c.level);
}

void test_double_click_at_level_1_flashes_only() {          // §4.2: at level 1 already, only the numeral flashes
  LevelController l = make(4);
  LevelChange c = l.set(0, LevelSource::Attendant);
  TEST_ASSERT_FALSE(c.changed); TEST_ASSERT_FALSE(c.cues);
  l.next(LevelSource::Pad); l.next(LevelSource::Pad); l.next(LevelSource::Pad);
  c = l.set(0, LevelSource::Attendant);
  TEST_ASSERT_TRUE(c.changed); TEST_ASSERT_TRUE(c.cues); TEST_ASSERT_EQUAL(3, c.from);
}

void test_cue_policy() {
  LevelPolicy p; p.attendantCues = false;
  LevelController l = make(4, p);
  TEST_ASSERT_FALSE(l.next(LevelSource::Attendant).cues);   // attendantCues off
  TEST_ASSERT_TRUE(l.next(LevelSource::Pad).cues);
  TEST_ASSERT_TRUE(l.next(LevelSource::Action).cues);
  TEST_ASSERT_FALSE(l.set(3, LevelSource::Action).cues);    // GoToLevel to the level already current: flash only
  TEST_ASSERT_FALSE(l.set(1, LevelSource::Portal).cues);    // the portal: screen only
  p = LevelPolicy(); p.click = false;
  l = make(4, p);
  TEST_ASSERT_FALSE(l.next(LevelSource::Pad).cues);
  TEST_ASSERT_FALSE(l.next(LevelSource::Attendant).cues);
  TEST_ASSERT_FALSE(l.next(LevelSource::Action).cues);
}

void test_out_of_range_goes_to_level_1() {
  LevelController l = make(4);
  l.next(LevelSource::Pad);
  LevelChange c = l.set(9, LevelSource::Action);
  TEST_ASSERT_EQUAL(0, c.level); TEST_ASSERT_TRUE(c.changed);
}

void test_shorter_list_clamps() {                           // §5.3: clamped to the new count
  LevelController l = make(4);
  l.set(3, LevelSource::Pad);
  l.configure(LevelPolicy(), 2);
  TEST_ASSERT_EQUAL(1, l.current());
  l.configure(LevelPolicy(), 0);                            // never zero levels
  TEST_ASSERT_EQUAL(1, l.count()); TEST_ASSERT_EQUAL(0, l.current());
}

void test_return_timer() {
  LevelController l = make(4);
  TEST_ASSERT_FALSE(l.tickReturnTimer(60000).changed);      // at level 1: never
  l.onInput(1000); l.next(LevelSource::Pad);                // to level 2 at t = 1 s
  l.onInput(1000);
  TEST_ASSERT_FALSE(l.returnDue(20999));
  TEST_ASSERT_EQUAL(1, l.returnInMs(20999));
  TEST_ASSERT_TRUE(l.returnDue(21000));
  LevelChange c = l.tickReturnTimer(21000);
  TEST_ASSERT_TRUE(c.changed); TEST_ASSERT_TRUE(c.cues); TEST_ASSERT_EQUAL(0, c.level); TEST_ASSERT_EQUAL(1, c.from);
  TEST_ASSERT_TRUE(c.source == LevelSource::Return);
  TEST_ASSERT_FALSE(l.tickReturnTimer(99000).changed);      // fires once
  TEST_ASSERT_EQUAL(0, l.returnInMs(99000));
}

void test_return_timer_restarts_on_input_and_on_a_change() {
  LevelController l = make(4);
  l.onInput(0); l.next(LevelSource::Pad);
  l.onInput(15000);
  TEST_ASSERT_FALSE(l.returnDue(20000));
  TEST_ASSERT_TRUE(l.returnDue(35000));
  l = make(4);
  l.onInput(0);
  l.set(2, LevelSource::Portal);                            // a portal change at t = 0 with the last input long ago...
  l.onInput(0);
  TEST_ASSERT_TRUE(l.returnDue(20000));
  l.tickReturnTimer(20000);
  TEST_ASSERT_EQUAL(0, l.current());
}

void test_return_cue_off_and_never() {
  LevelPolicy p; p.returnCue = false;
  LevelController l = make(4, p);
  l.onInput(0); l.next(LevelSource::Pad);
  LevelChange c = l.tickReturnTimer(20000);
  TEST_ASSERT_TRUE(c.changed); TEST_ASSERT_FALSE(c.cues);
  p = LevelPolicy(); p.returnToFirstAfterS = 0;
  l = make(4, p);
  l.onInput(0); l.next(LevelSource::Pad);
  TEST_ASSERT_FALSE(l.returnDue(3600000));
  TEST_ASSERT_EQUAL(0, l.returnInMs(3600000));
}

void test_restore_clamps() {
  LevelController l = make(3);
  l.restore(2); TEST_ASSERT_EQUAL(2, l.current());
  l.restore(7); TEST_ASSERT_EQUAL(0, l.current());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pad_wraps_forward);
  RUN_TEST(test_prev_wraps_to_last);
  RUN_TEST(test_single_level_pad_still_clicks);
  RUN_TEST(test_double_click_at_level_1_flashes_only);
  RUN_TEST(test_cue_policy);
  RUN_TEST(test_out_of_range_goes_to_level_1);
  RUN_TEST(test_shorter_list_clamps);
  RUN_TEST(test_return_timer);
  RUN_TEST(test_return_timer_restarts_on_input_and_on_a_change);
  RUN_TEST(test_return_cue_off_and_never);
  RUN_TEST(test_restore_clamps);
  return UNITY_END();
}
