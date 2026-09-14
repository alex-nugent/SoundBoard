// Native tests for the keyboard model (FirmwareSpec.md §8.2, §8.3, Appendix B).
#include <unity.h>
#include <string.h>
#include "ble/keyboard_model.h"

using namespace sb;
void setUp() {} void tearDown() {}

// A stub of the core keymap: a-z, A-Z (shift), space; everything else unmapped.
static bool stubKeymap(uint8_t c, uint8_t& usage, uint8_t& mod) {
  if (c >= 'a' && c <= 'z') { usage = 4 + (c - 'a'); mod = 0; return true; }
  if (c >= 'A' && c <= 'Z') { usage = 4 + (c - 'A'); mod = 2; return true; }
  if (c == ' ') { usage = 0x2C; mod = 0; return true; }
  return false;
}
static KeyboardModel readyModel() { KeyboardModel m; m.configure(12, 3000); m.setKeymap(stubKeymap); m.setReady(true); return m; }
static bool isDown(const KbdReport& r, uint8_t usage, uint8_t mod = 0) { return r.id == 1 && r.len == 8 && r.data[0] == mod && r.data[2] == usage; }
static bool isUp(const KbdReport& r) { return r.id == 1 && r.len == 8 && r.data[0] == 0 && r.data[2] == 0; }

void test_key_table() {
  TEST_ASSERT_NOT_NULL(findKey("space")); TEST_ASSERT_EQUAL_HEX8(0x2C, findKey("SPACE")->usage);
  TEST_ASSERT_EQUAL_HEX8(0x20, findKey("vol+")->consumerBit); TEST_ASSERT_TRUE(findKey("Vol+")->consumer());
  TEST_ASSERT_EQUAL_HEX8(0x45, findKey("f12")->usage);
  TEST_ASSERT_NULL(findKey("bogus")); TEST_ASSERT_NULL(findKey("")); TEST_ASSERT_NULL(findKey(nullptr));
  TEST_ASSERT_EQUAL(65, keyCount());                       // 9 + 10 + 26 + 12 + 8
}

void test_not_ready_drops() {
  KeyboardModel m; m.configure(12, 3000); m.setKeymap(stubKeymap);
  KbdReport r;
  TEST_ASSERT_FALSE(m.type("ab")); TEST_ASSERT_FALSE(m.hold(*findKey("SPACE"), 1)); TEST_ASSERT_FALSE(m.tap(*findKey("ENTER")));
  TEST_ASSERT_FALSE(m.next(0, r)); TEST_ASSERT_TRUE(m.idle()); TEST_ASSERT_EQUAL(0, m.dropped());
}

void test_type_cadence() {
  KeyboardModel m = readyModel(); KbdReport r;
  TEST_ASSERT_TRUE(m.type("ab"));
  TEST_ASSERT_TRUE(m.next(100, r)); TEST_ASSERT_TRUE(isDown(r, 4));      // a down at once
  TEST_ASSERT_FALSE(m.next(100, r)); TEST_ASSERT_FALSE(m.next(111, r));
  TEST_ASSERT_TRUE(m.next(112, r)); TEST_ASSERT_TRUE(isUp(r));           // a up after 12 ms
  TEST_ASSERT_FALSE(m.next(123, r));
  TEST_ASSERT_TRUE(m.next(124, r)); TEST_ASSERT_TRUE(isDown(r, 5));      // b down
  TEST_ASSERT_TRUE(m.next(136, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_FALSE(m.next(200, r)); TEST_ASSERT_TRUE(m.idle());
}

void test_shift_and_unmapped() {
  KeyboardModel m = readyModel(); KbdReport r;
  TEST_ASSERT_TRUE(m.type("A\x01z"));
  TEST_ASSERT_TRUE(m.next(0, r)); TEST_ASSERT_TRUE(isDown(r, 4, 2));     // shift + a
  TEST_ASSERT_TRUE(m.next(12, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_TRUE(m.next(24, r)); TEST_ASSERT_TRUE(isDown(r, 4 + 25));  // \x01 skipped, z next
  TEST_ASSERT_TRUE(m.next(36, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_FALSE(m.next(48, r));
}

void test_queue_depth() {
  KeyboardModel m = readyModel();
  TEST_ASSERT_TRUE(m.type("a")); TEST_ASSERT_TRUE(m.type("b")); TEST_ASSERT_TRUE(m.type("c")); TEST_ASSERT_TRUE(m.type("d"));
  TEST_ASSERT_FALSE(m.type("e"));                          // the fifth is dropped
  TEST_ASSERT_EQUAL(4, m.queued()); TEST_ASSERT_EQUAL(1, m.dropped());
}

void test_hold_forced_release() {
  KeyboardModel m = readyModel(); KbdReport r;
  TEST_ASSERT_TRUE(m.hold(*findKey("SPACE"), 7));
  TEST_ASSERT_TRUE(m.next(1000, r)); TEST_ASSERT_TRUE(isDown(r, 0x2C)); TEST_ASSERT_TRUE(m.holding()); TEST_ASSERT_EQUAL(7, m.heldOwner());
  TEST_ASSERT_FALSE(m.next(3999, r));
  TEST_ASSERT_TRUE(m.next(4000, r)); TEST_ASSERT_TRUE(isUp(r));           // holdKeysMaxMs 3000 after the down
  TEST_ASSERT_FALSE(m.holding()); TEST_ASSERT_EQUAL(1, m.forced()); TEST_ASSERT_TRUE(m.takeForcedFlag()); TEST_ASSERT_FALSE(m.takeForcedFlag());
  m.release(7);                                            // the pad lifts later: nothing more
  TEST_ASSERT_FALSE(m.next(5000, r));
}

void test_release_by_owner() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.hold(*findKey("ENTER"), 7); TEST_ASSERT_TRUE(m.next(0, r));
  m.release(8); TEST_ASSERT_FALSE(m.next(1, r)); TEST_ASSERT_TRUE(m.holding());   // another press's up: ignored
  m.release(7); TEST_ASSERT_TRUE(m.next(2, r)); TEST_ASSERT_TRUE(isUp(r)); TEST_ASSERT_FALSE(m.holding());
  TEST_ASSERT_EQUAL(0, m.forced());
}

void test_new_hold_releases_previous() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.hold(*findKey("SPACE"), 7); TEST_ASSERT_TRUE(m.next(0, r));
  m.hold(*findKey("ENTER"), 8);
  TEST_ASSERT_TRUE(m.next(10, r)); TEST_ASSERT_TRUE(isUp(r));               // SPACE up first
  TEST_ASSERT_TRUE(m.next(10, r)); TEST_ASSERT_TRUE(isDown(r, 0x28));       // then ENTER down
  TEST_ASSERT_EQUAL(8, m.heldOwner());
  m.release(7); TEST_ASSERT_FALSE(m.next(11, r)); TEST_ASSERT_TRUE(m.holding());   // the old press's up changes nothing
}

void test_hold_waits_for_typing() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.type("ab"); m.hold(*findKey("SPACE"), 9);
  TEST_ASSERT_TRUE(m.next(0, r)); TEST_ASSERT_TRUE(isDown(r, 4));
  TEST_ASSERT_FALSE(m.next(5, r));                                          // the hold does not interleave
  TEST_ASSERT_TRUE(m.next(12, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_TRUE(m.next(24, r)); TEST_ASSERT_TRUE(isDown(r, 5));
  TEST_ASSERT_TRUE(m.next(36, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_TRUE(m.next(48, r)); TEST_ASSERT_TRUE(isDown(r, 0x2C));      // then SPACE down
  TEST_ASSERT_TRUE(m.holding());
}

void test_release_cancels_queued_hold() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.type("a"); m.hold(*findKey("SPACE"), 9);
  TEST_ASSERT_TRUE(m.next(0, r));
  m.release(9);                                                             // pad up before its key went down
  TEST_ASSERT_TRUE(m.next(12, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_FALSE(m.next(24, r)); TEST_ASSERT_FALSE(m.holding()); TEST_ASSERT_TRUE(m.idle());
}

void test_tap() {
  KeyboardModel m = readyModel(); KbdReport r;
  TEST_ASSERT_TRUE(m.tap(*findKey("ENTER")));
  TEST_ASSERT_TRUE(m.next(100, r)); TEST_ASSERT_TRUE(isDown(r, 0x28));
  TEST_ASSERT_FALSE(m.next(129, r));
  TEST_ASSERT_TRUE(m.next(130, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_TRUE(m.idle());
}

void test_consumer_key() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.hold(*findKey("VOL+"), 3);
  TEST_ASSERT_TRUE(m.next(0, r)); TEST_ASSERT_EQUAL(2, r.id); TEST_ASSERT_EQUAL(1, r.len); TEST_ASSERT_EQUAL_HEX8(0x20, r.data[0]);
  m.release(3);
  TEST_ASSERT_TRUE(m.next(1, r)); TEST_ASSERT_EQUAL(2, r.id); TEST_ASSERT_EQUAL_HEX8(0, r.data[0]);
}

void test_ready_lost_clears() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.type("abc"); m.hold(*findKey("SPACE"), 4); m.next(0, r);
  m.setReady(false);
  TEST_ASSERT_FALSE(m.next(1, r)); TEST_ASSERT_TRUE(m.idle()); TEST_ASSERT_FALSE(m.holding());
  TEST_ASSERT_FALSE(m.type("x"));
  m.setReady(true); TEST_ASSERT_TRUE(m.type("x"));
}

void test_release_all() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.hold(*findKey("SPACE"), 5); TEST_ASSERT_TRUE(m.next(0, r));
  m.type("a"); m.hold(*findKey("ENTER"), 6);                                // one string and one hold waiting
  m.releaseAll();                                                           // level change
  TEST_ASSERT_TRUE(m.next(1, r)); TEST_ASSERT_TRUE(isUp(r));                // the held key up at once
  TEST_ASSERT_TRUE(m.next(1, r)); TEST_ASSERT_TRUE(isDown(r, 4));           // the string still types
  TEST_ASSERT_TRUE(m.next(13, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_FALSE(m.next(25, r)); TEST_ASSERT_FALSE(m.holding());         // the queued hold is gone
}

void test_type_after_hold_releases_it() {
  KeyboardModel m = readyModel(); KbdReport r;
  m.hold(*findKey("SPACE"), 5); TEST_ASSERT_TRUE(m.next(0, r));
  m.type("a");
  TEST_ASSERT_TRUE(m.next(1, r)); TEST_ASSERT_TRUE(isUp(r));
  TEST_ASSERT_TRUE(m.next(1, r)); TEST_ASSERT_TRUE(isDown(r, 4));
  TEST_ASSERT_FALSE(m.holding());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_key_table);
  RUN_TEST(test_not_ready_drops);
  RUN_TEST(test_type_cadence);
  RUN_TEST(test_shift_and_unmapped);
  RUN_TEST(test_queue_depth);
  RUN_TEST(test_hold_forced_release);
  RUN_TEST(test_release_by_owner);
  RUN_TEST(test_new_hold_releases_previous);
  RUN_TEST(test_hold_waits_for_typing);
  RUN_TEST(test_release_cancels_queued_hold);
  RUN_TEST(test_tap);
  RUN_TEST(test_consumer_key);
  RUN_TEST(test_ready_lost_clears);
  RUN_TEST(test_release_all);
  RUN_TEST(test_type_after_hold_releases_it);
  return UNITY_END();
}
