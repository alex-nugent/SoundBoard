// Native tests for the button-command engine (FirmwareSpec.md §4.2 as amended
// for Draft 4: clicks resolve on release, a quick tap of both buttons is level 1).
// Run with: pio test -d firmware -e native
#include <unity.h>
#include <vector>
#include "input/button_commands.h"

using namespace sb;

static ButtonCommands bc;
static uint32_t T;                       // simulated clock (ms)
static std::vector<ButtonCmd> fired;

void setUp() { bc = ButtonCommands(); bc.setDurations(ButtonDurations()); T = 10000; fired.clear(); }
void tearDown() {}

// Advances the clock in 5 ms ticks holding the given states; collects commands.
// A press is timed from the first tick that sees it, so a hold of exactly the
// threshold needs one extra tick: HOLD/BOTH_MENU below include it.
constexpr uint32_t HOLD = 1005, BOTH_MENU = 3005;
static void hold(uint32_t ms, bool minus, bool plus) {
  for (uint32_t t = 0; t < ms; t += 5) { T += 5; ButtonCmd c = bc.feed(T, minus, plus); if (c != ButtonCmd::None) fired.push_back(c); }
}
static void tap(bool minus, bool plus, uint32_t downMs = 100) { hold(downMs, minus, plus); hold(5, false, false); }

void test_click_resolves_on_release() {
  tap(false, true);
  TEST_ASSERT_EQUAL(1, fired.size());                       // the tick that sees the release fires it
  TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeUp, (int)fired[0]);
  hold(2000, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
}

void test_minus_click_is_volume_down() {
  tap(true, false);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeDown, (int)fired[0]);
}

void test_rapid_clicks_each_count() {                       // five presses ~100 ms apart: five steps, never a level change
  for (int i = 0; i < 5; i++) { tap(false, true, 60); hold(40, false, false); }
  TEST_ASSERT_EQUAL(5, fired.size());
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeUp, (int)fired[i]);
}

void test_long_hold_fires_once_and_release_is_silent() {
  hold(HOLD, false, true);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::NextLevel, (int)fired[0]);
  hold(3000, false, true);                                  // no repeat while held
  hold(500, false, false);                                  // the release produces nothing
  TEST_ASSERT_EQUAL(1, fired.size());
  hold(HOLD, true, false);
  TEST_ASSERT_EQUAL(2, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::PrevLevel, (int)fired[1]);
}

void test_click_then_hold_are_two_commands() {
  tap(false, true);
  hold(50, false, false);
  hold(HOLD, false, true);
  TEST_ASSERT_EQUAL(2, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeUp, (int)fired[0]);
  TEST_ASSERT_EQUAL((int)ButtonCmd::NextLevel, (int)fired[1]);
  hold(500, false, false);
  TEST_ASSERT_EQUAL(2, fired.size());
}

void test_both_tap_is_level1() {
  hold(200, true, true);
  TEST_ASSERT_EQUAL(0, fired.size());
  hold(5, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Level1, (int)fired[0]);
  hold(1000, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());                       // no clicks follow
}

void test_both_tap_staggered_release() {
  hold(200, true, true);
  hold(5, false, true);                                     // minus released first: the tap resolves now
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Level1, (int)fired[0]);
  hold(300, false, true);                                   // plus still down: no click, no long hold
  hold(5, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
}

void test_both_tap_staggered_press() {
  hold(30, false, true);                                    // plus lands first
  hold(200, true, true);                                    // then both
  hold(5, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Level1, (int)fired[0]);   // and no click for the plus
}

void test_both_released_after_off_hold_is_off() {
  hold(1200, true, true);
  TEST_ASSERT_EQUAL(0, fired.size());
  HoldProgress p = bc.progress(T);
  TEST_ASSERT_EQUAL((int)HoldKind::Both, (int)p.kind);
  TEST_ASSERT_TRUE(p.offReached);
  TEST_ASSERT_EQUAL(2, p.secondsLeft);
  hold(5, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Off, (int)fired[0]);
}

void test_both_released_between_tap_and_off_is_nothing() {
  hold(600, true, true);
  hold(5, false, true);                                     // minus released first, plus still down
  hold(2000, false, true);                                  // the remaining button must not become a long hold
  hold(5, false, false);
  TEST_ASSERT_EQUAL(0, fired.size());
}

void test_both_held_to_menu_needs_no_release() {
  hold(BOTH_MENU, true, true);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Menu, (int)fired[0]);
  hold(2000, true, true);
  hold(5, false, false);                                    // the release after MENU produces nothing
  TEST_ASSERT_EQUAL(1, fired.size());
}

void test_second_button_cancels_pending_long_hold() {
  hold(800, false, true);
  hold(600, true, true);                                    // both down before the single hold fires (too long for a tap)
  hold(5, false, true);                                     // minus released: the plus hold cannot fire any more
  hold(2000, false, true);
  hold(5, false, false);
  TEST_ASSERT_EQUAL(0, fired.size());
}

void test_disabled_until_enable_and_held_through_boot_ignored() {
  bc.enableAt(T + 500);
  hold(300, false, true);                                   // pressed before the enable time
  hold(1500, false, true);                                  // still held long after it
  TEST_ASSERT_EQUAL(0, fired.size());
  hold(5, false, false);
  hold(HOLD, false, true);                                  // a fresh press works
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::NextLevel, (int)fired[0]);
}

void test_off_suppressed_after_wake_until_released_once() {
  bc.suppressOffUntilRelease();
  hold(1200, true, true);
  hold(5, false, false);
  TEST_ASSERT_EQUAL(0, fired.size());                       // would have been Off
  hold(1200, true, true);
  hold(5, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Off, (int)fired[0]);
}

void test_both_tap_works_while_off_suppressed() {
  bc.suppressOffUntilRelease();
  hold(200, true, true);
  hold(5, false, false);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Level1, (int)fired[0]);
}

void test_menu_still_reachable_while_off_suppressed() {
  bc.suppressOffUntilRelease();
  hold(BOTH_MENU, true, true);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::Menu, (int)fired[0]);
}

void test_menu_mode_click_resolves_on_release() {
  bc.setMenuMode(true);
  tap(false, true);
  TEST_ASSERT_EQUAL(1, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeUp, (int)fired[0]);
  tap(false, true);
  TEST_ASSERT_EQUAL(2, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeUp, (int)fired[1]);
}

void test_progress_bar_for_single_hold() {
  hold(100, false, true);
  TEST_ASSERT_EQUAL((int)HoldKind::None, (int)bc.progress(T).kind);   // too early for a bar
  hold(400, false, true);
  HoldProgress p = bc.progress(T);
  TEST_ASSERT_EQUAL((int)HoldKind::NextLevel, (int)p.kind);
  TEST_ASSERT_INT_WITHIN(2, 50, p.pct);
  hold(510, false, true);
  TEST_ASSERT_EQUAL((int)HoldKind::None, (int)bc.progress(T).kind);   // fired and consumed: bar gone
}

void test_progress_bar_for_both_hold() {
  hold(100, true, true);
  TEST_ASSERT_EQUAL((int)HoldKind::None, (int)bc.progress(T).kind);   // a tap does not flicker the bar
  hold(400, true, true);
  HoldProgress p = bc.progress(T);
  TEST_ASSERT_EQUAL((int)HoldKind::Both, (int)p.kind);
  TEST_ASSERT_INT_WITHIN(2, 50, p.pct);
  TEST_ASSERT_FALSE(p.offReached);
}

void test_click_then_other_button_hold() {
  tap(false, true);
  hold(100, false, false);
  hold(HOLD, true, false);                                  // the other button, held to a long hold
  TEST_ASSERT_EQUAL(2, fired.size());
  TEST_ASSERT_EQUAL((int)ButtonCmd::VolumeUp, (int)fired[0]);
  TEST_ASSERT_EQUAL((int)ButtonCmd::PrevLevel, (int)fired[1]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_click_resolves_on_release);
  RUN_TEST(test_minus_click_is_volume_down);
  RUN_TEST(test_rapid_clicks_each_count);
  RUN_TEST(test_long_hold_fires_once_and_release_is_silent);
  RUN_TEST(test_click_then_hold_are_two_commands);
  RUN_TEST(test_both_tap_is_level1);
  RUN_TEST(test_both_tap_staggered_release);
  RUN_TEST(test_both_tap_staggered_press);
  RUN_TEST(test_both_released_after_off_hold_is_off);
  RUN_TEST(test_both_released_between_tap_and_off_is_nothing);
  RUN_TEST(test_both_held_to_menu_needs_no_release);
  RUN_TEST(test_second_button_cancels_pending_long_hold);
  RUN_TEST(test_disabled_until_enable_and_held_through_boot_ignored);
  RUN_TEST(test_off_suppressed_after_wake_until_released_once);
  RUN_TEST(test_both_tap_works_while_off_suppressed);
  RUN_TEST(test_menu_still_reachable_while_off_suppressed);
  RUN_TEST(test_menu_mode_click_resolves_on_release);
  RUN_TEST(test_progress_bar_for_single_hold);
  RUN_TEST(test_progress_bar_for_both_hold);
  RUN_TEST(test_click_then_other_button_hold);
  return UNITY_END();
}
