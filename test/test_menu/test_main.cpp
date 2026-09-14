// Native tests for the Quick Menu model (FirmwareSpec.md §14.3, §14.4, §14.5).
// Run with: pio test -d firmware -e native
#include <unity.h>
#include <string.h>
#include "app/menu_model.h"
#include "config/loader.h"

using namespace sb;

static MenuModel m;
static Config    cfg;

void setUp() { config::defaults(cfg); m.build(); m.first(); m.open(cfg, 60); }
void tearDown() {}

static void goTo(MenuKind k) { for (uint8_t i = 0; i < m.count(); i++) { if (m.item().kind == k) return; m.next(); } TEST_FAIL_MESSAGE("item not found"); }
static void goToPath(const char* path) { for (uint8_t i = 0; i < m.count(); i++) { if (m.item().kind == MenuKind::Setting && !strcmp(m.item().desc->path, path)) return; m.next(); } TEST_FAIL_MESSAGE("setting not in the menu"); }
static void apply(const char* text) { TEST_ASSERT_TRUE(config::setScalarText(cfg, *m.item().desc, text, nullptr)); }

void test_items_in_the_order_of_14_3() {                    // minus Volume, dropped at CP-9 (the buttons set it)
  TEST_ASSERT_EQUAL(10, m.count());
  const char* paths[10] = { "audio.outputs.speakers", "bluetoothSpeaker.enabled", nullptr, "keyboard.enabled", "vibration.enabled", "display.brightnessPct", nullptr, nullptr, nullptr, nullptr };
  MenuKind kinds[10] = { MenuKind::Setting, MenuKind::Setting, MenuKind::PairSpeaker, MenuKind::Setting, MenuKind::Setting, MenuKind::Setting, MenuKind::Recalibrate, MenuKind::WifiSetup, MenuKind::Exit, MenuKind::Cancel };
  uint8_t orders[10] = { 1, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
  for (uint8_t i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL((int)kinds[i], (int)m.at(i).kind);
    TEST_ASSERT_EQUAL(orders[i], m.at(i).order);
    if (paths[i]) TEST_ASSERT_EQUAL_STRING(paths[i], m.at(i).desc->path);
  }
}

void test_next_and_prev_wrap() {
  TEST_ASSERT_EQUAL(0, m.index());
  m.prev();
  TEST_ASSERT_EQUAL(9, m.index());
  m.next();
  TEST_ASSERT_EQUAL(0, m.index());
  for (int i = 0; i < 10; i++) m.next();
  TEST_ASSERT_EQUAL(0, m.index());
}

void test_labels_and_values() {
  char s[40];
  m.label(s, sizeof s);  TEST_ASSERT_EQUAL_STRING("On-board speakers", s);
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("OFF", s);
  goToPath("display.brightnessPct");
  m.label(s, sizeof s);  TEST_ASSERT_EQUAL_STRING("Screen brightness", s);   // the unit leaves the label ...
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("80 %", s);    // ... and joins the value
  goToPath("keyboard.enabled");
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("ON", s);
  goTo(MenuKind::Exit);
  m.label(s, sizeof s);  TEST_ASSERT_EQUAL_STRING("Save and exit", s);
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("press SAVE", s);   // an action names the pad to press ...
  TEST_ASSERT_EQUAL_STRING("SAVE", m.actionVerb());                                  // ... by the word shown above it
  goTo(MenuKind::Cancel);
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("press CANCEL", s);
  goTo(MenuKind::WifiSetup);
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("press START twice", s);
  goTo(MenuKind::PairSpeaker);
  TEST_ASSERT_EQUAL_STRING("PAIR", m.actionVerb());
  m.setPairWipeOffered(true);
  m.valueText(cfg, 60, s, sizeof s); TEST_ASSERT_EQUAL_STRING("forget all and pair", s);   // after NO SPEAKER FOUND
  TEST_ASSERT_EQUAL_STRING("", MenuModel().actionVerb() ? "" : "");                 // a value item has no verb (checked below)
  goToPath("keyboard.enabled");
  TEST_ASSERT_EQUAL_STRING("", m.actionVerb());
}

void test_bool_steps_up_to_on_and_down_to_off() {
  char t[40];
  TEST_ASSERT_FALSE(m.stepText(cfg, -1, t, sizeof t));       // OFF already
  TEST_ASSERT_TRUE(m.stepText(cfg, +1, t, sizeof t));
  TEST_ASSERT_EQUAL_STRING("true", t);
  apply(t);
  TEST_ASSERT_FALSE(m.stepText(cfg, +1, t, sizeof t));       // ON already
  TEST_ASSERT_TRUE(m.stepText(cfg, -1, t, sizeof t));
  TEST_ASSERT_EQUAL_STRING("false", t);
}

void test_choices_step_and_clamp() {
  char t[40];
  goToPath("display.brightnessPct");                          // 80, choices 10..100
  TEST_ASSERT_TRUE(m.stepText(cfg, +1, t, sizeof t)); TEST_ASSERT_EQUAL_STRING("90", t); apply(t);
  TEST_ASSERT_TRUE(m.stepText(cfg, +1, t, sizeof t)); TEST_ASSERT_EQUAL_STRING("100", t); apply(t);
  TEST_ASSERT_FALSE(m.stepText(cfg, +1, t, sizeof t));
  for (int i = 0; i < 9; i++) { TEST_ASSERT_TRUE(m.stepText(cfg, -1, t, sizeof t)); apply(t); }
  TEST_ASSERT_EQUAL(10, cfg.display.brightnessPct);
  TEST_ASSERT_FALSE(m.stepText(cfg, -1, t, sizeof t));
}

void test_range_step_with_zero_off() {                        // not a menu row today, but the rule for one: min..max by step, then 0
  const SettingDesc* d = findSetting("power.sleepAfterMin");   // 1..120 step 1, choices 0|1|2|3|5|10|15|30|60|120
  TEST_ASSERT_NOT_NULL(d);
  SettingDesc r = *d; r.choices = nullptr;                     // exercise the range path
  Config c; config::defaults(c);                               // 5
  char t[40];
  TEST_ASSERT_TRUE(MenuModel::stepSetting(c, r, -1, t, sizeof t)); TEST_ASSERT_EQUAL_STRING("4", t);
  c.power.sleepAfterMin = 1;
  TEST_ASSERT_TRUE(MenuModel::stepSetting(c, r, -1, t, sizeof t)); TEST_ASSERT_EQUAL_STRING("0", t);   // ZERO_OFF: 0 below min
  c.power.sleepAfterMin = 0;
  TEST_ASSERT_FALSE(MenuModel::stepSetting(c, r, -1, t, sizeof t));
  TEST_ASSERT_TRUE(MenuModel::stepSetting(c, r, +1, t, sizeof t)); TEST_ASSERT_EQUAL_STRING("1", t);   // back to min
  c.power.sleepAfterMin = 120;
  TEST_ASSERT_FALSE(MenuModel::stepSetting(c, r, +1, t, sizeof t));
}

void test_volume_steps_by_step_pct_and_clamps() {
  uint8_t v;
  TEST_ASSERT_TRUE(MenuModel::stepVolume(60, 10, +1, v)); TEST_ASSERT_EQUAL(70, v);
  TEST_ASSERT_TRUE(MenuModel::stepVolume(95, 10, +1, v)); TEST_ASSERT_EQUAL(100, v);
  TEST_ASSERT_FALSE(MenuModel::stepVolume(100, 10, +1, v));
  TEST_ASSERT_TRUE(MenuModel::stepVolume(5, 10, -1, v));  TEST_ASSERT_EQUAL(0, v);
  TEST_ASSERT_FALSE(MenuModel::stepVolume(0, 10, -1, v));
  TEST_ASSERT_TRUE(MenuModel::stepVolume(60, 5, -1, v));  TEST_ASSERT_EQUAL(55, v);
}

void test_changed_and_snapshot() {
  TEST_ASSERT_FALSE(m.changed(cfg));
  char t[40];
  goToPath("keyboard.enabled");
  TEST_ASSERT_EQUAL_STRING("true", m.snapshotText(m.index()));
  TEST_ASSERT_TRUE(m.stepText(cfg, -1, t, sizeof t)); apply(t);
  TEST_ASSERT_TRUE(m.changed(cfg));
  apply(m.snapshotText(m.index()));                            // Cancel: the snapshot text goes back through the same path
  TEST_ASSERT_FALSE(m.changed(cfg));                           // changed and changed back: nothing to save
  TEST_ASSERT_EQUAL(60, m.snapshotVolume());
  TEST_ASSERT_EQUAL_STRING("", m.snapshotText(2));             // the Pair item has no text snapshot
}

void test_action_items_and_confirmation() {
  TEST_ASSERT_FALSE(m.isAction());
  TEST_ASSERT_FALSE(m.confirmNeeded());
  goTo(MenuKind::PairSpeaker);
  TEST_ASSERT_TRUE(m.isAction());
  TEST_ASSERT_FALSE(m.confirmNeeded());                       // one press pairs
  m.setPairWipeOffered(true);                                  // after "no speaker found": the next press forgets every speaker first
  TEST_ASSERT_TRUE(m.pairWipeOffered());
  goTo(MenuKind::WifiSetup);
  TEST_ASSERT_TRUE(m.confirmNeeded());                        // the one item that wants the press twice
  TEST_ASSERT_FALSE(m.armed());
  m.arm(); TEST_ASSERT_TRUE(m.armed());
  m.disarm(); TEST_ASSERT_FALSE(m.armed());
  goTo(MenuKind::Cancel);
  TEST_ASSERT_TRUE(m.isAction());
  char t[40];
  TEST_ASSERT_FALSE(m.stepText(cfg, +1, t, sizeof t));        // an action has no value to step
  m.arm();
  m.open(cfg, 50);                                             // a new entry forgets the wipe offer and the arming
  goTo(MenuKind::PairSpeaker);
  TEST_ASSERT_FALSE(m.pairWipeOffered());
  TEST_ASSERT_FALSE(m.armed());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_items_in_the_order_of_14_3);
  RUN_TEST(test_next_and_prev_wrap);
  RUN_TEST(test_labels_and_values);
  RUN_TEST(test_bool_steps_up_to_on_and_down_to_off);
  RUN_TEST(test_choices_step_and_clamp);
  RUN_TEST(test_range_step_with_zero_off);
  RUN_TEST(test_volume_steps_by_step_pct_and_clamps);
  RUN_TEST(test_changed_and_snapshot);
  RUN_TEST(test_action_items_and_confirmation);
  return UNITY_END();
}
