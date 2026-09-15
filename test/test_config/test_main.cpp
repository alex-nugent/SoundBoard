// Native tests for the configuration core (FirmwareSpec.md §19.10, Phase 1).
// Run with: pio test -d firmware -e native
#include <unity.h>
#include <ArduinoJson.h>
#include <string>
#include <string.h>
#include "config/config.h"
#include "config/loader.h"
#include "config/levels.h"
#include "config/migrate.h"
#include "samples.h"

using namespace sb;
using namespace ArduinoJson;

static Config       cfg;
static ConfigReport rep;

void setUp() { rep.clear(); }
void tearDown() {}

static void dumpReport(const ConfigReport& r) {
  for (uint8_t i = 0; i < r.count; i++) printf("    %s %s\n", r.items[i].error ? "ERROR" : "warn ", r.items[i].text);
}

static bool hasWarning(const ConfigReport& r, const char* needle) {
  for (uint8_t i = 0; i < r.count; i++) if (strstr(r.items[i].text, needle)) return true;
  return false;
}

static bool parse(JsonDocument& doc, const char* text) {
  DeserializationError e = deserializeJson(doc, text, strlen(text));
  if (e) { printf("    parse error: %s\n", e.c_str()); return false; }
  return true;
}

// ---- defaults ---------------------------------------------------------------
void test_defaults() {
  config::defaults(cfg);
  TEST_ASSERT_EQUAL(SCHEMA_VERSION, cfg.schema);
  TEST_ASSERT_EQUAL(1, cfg.levels.count);
  TEST_ASSERT_EQUAL(3, nSoundPads(cfg));
  TEST_ASSERT_EQUAL(0, levelPadPos(cfg));
  TEST_ASSERT_EQUAL(5, cfg.power.sleepAfterMin);
  TEST_ASSERT_EQUAL(THEME_AMBER, cfg.display.theme);
  TEST_ASSERT_EQUAL_FLOAT(3.0f, cfg.touch.pressPct);
  TEST_ASSERT_EQUAL_STRING("soundboard", cfg.setup.password);
  TEST_ASSERT_EQUAL_STRING("_click.wav", cfg.audio.cues.click);
  TEST_ASSERT_EQUAL(2, cfg.device.ownerLines);
  TEST_ASSERT_EQUAL(3, cfg.levelChange.vibration.patternLen);
  TEST_ASSERT_EQUAL(2, cfg.hardware.padChannels[0]);
  TEST_ASSERT_EQUAL(5, cfg.hardware.padChannels[3]);
  TEST_ASSERT_EQUAL_STRING("", cfg.levels.levels[0].buttons[0].sound);
  TEST_ASSERT_EQUAL(100, cfg.levels.levels[0].buttons[0].volumePct);
}

// ---- the §13.3 example -------------------------------------------------------
void test_load_example() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_EXAMPLE));
  TEST_ASSERT_TRUE(config::load(doc.as<JsonVariantConst>(), cfg, rep));
  dumpReport(rep);
  TEST_ASSERT_EQUAL(0, rep.count);
  TEST_ASSERT_EQUAL(1, cfg.levels.count);
  TEST_ASSERT_EQUAL_STRING("ONE", cfg.levels.levels[0].name);
  TEST_ASSERT_TRUE(cfg.levels.levels[0].hasVibration);
  TEST_ASSERT_EQUAL(3, cfg.levels.levels[0].vibrationLen);
  TEST_ASSERT_EQUAL_STRING("maybe.wav", cfg.levels.levels[0].buttons[1].sound);
  TEST_ASSERT_EQUAL_STRING("maybe", cfg.levels.levels[0].buttons[1].type);
  TEST_ASSERT_EQUAL((int)Tri::Inherit, (int)cfg.levels.levels[0].buttons[0].vibrate);
  TEST_ASSERT_EQUAL((int)JackMode::Inherit, (int)cfg.levels.levels[0].buttons[0].jack);
  TEST_ASSERT_FALSE(cfg.touch.hasPadPressPct);
  TEST_ASSERT_EQUAL_STRING("alex-nugent/SoundBoard", cfg.update.repo);
}

// ---- The owner's file (§13.6) ---------------------------------------------------
void test_load_owner_file() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_OWNER));
  TEST_ASSERT_TRUE(config::load(doc.as<JsonVariantConst>(), cfg, rep));
  dumpReport(rep);
  TEST_ASSERT_EQUAL_MESSAGE(0, rep.count, "her file must load without warnings");
  TEST_ASSERT_EQUAL(4, cfg.levels.count);
  TEST_ASSERT_EQUAL_STRING("VOLUME", cfg.levels.levels[2].name);
  TEST_ASSERT_EQUAL((int)Action::VolumeUp,   (int)cfg.levels.levels[2].buttons[0].action);
  TEST_ASSERT_EQUAL((int)Action::Mute,       (int)cfg.levels.levels[2].buttons[1].action);
  TEST_ASSERT_EQUAL((int)Action::VolumeDown, (int)cfg.levels.levels[2].buttons[2].action);
  TEST_ASSERT_EQUAL_STRING("Thank you", cfg.levels.levels[3].buttons[1].label);
  TEST_ASSERT_EQUAL(250, cfg.press.repeatDelayMs);
  TEST_ASSERT_FALSE(cfg.keyboard.enabled);
  TEST_ASSERT_EQUAL(2, cfg.audio.clickVolume);
  TEST_ASSERT_EQUAL(3000, cfg.menu.holdMs);        // default filled in
  TEST_ASSERT_EQUAL_STRING("", cfg.levels.levels[0].buttons[0].type);
  char lbl[25];
  entryLabel(cfg.levels.levels[0].buttons[0], lbl, sizeof lbl);
  TEST_ASSERT_EQUAL_STRING("Yes", lbl);
  Entry e; config::blankEntry(e); strcpy(e.sound, "hello.wav");
  entryLabel(e, lbl, sizeof lbl);
  TEST_ASSERT_EQUAL_STRING("hello", lbl);
}

// ---- clamping ----------------------------------------------------------------
void test_clamp_out_of_range() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_OUT_OF_RANGE));
  config::load(doc.as<JsonVariantConst>(), cfg, rep);
  dumpReport(rep);
  TEST_ASSERT_TRUE(rep.ok());                       // clamps are warnings, not errors
  TEST_ASSERT_EQUAL_FLOAT(10.0f, cfg.touch.pressPct);
  TEST_ASSERT_EQUAL_FLOAT(0.5f, cfg.touch.releasePct);
  TEST_ASSERT_EQUAL(0, cfg.touch.stuckAfterMs);     // 0 = never is allowed
  TEST_ASSERT_EQUAL(100, cfg.audio.volumePct);
  TEST_ASSERT_EQUAL(5, cfg.audio.stepPct);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, cfg.audio.maxGain);
  TEST_ASSERT_EQUAL(THEME_AMBER, cfg.display.theme);   // "purple" -> default
  TEST_ASSERT_EQUAL(80, cfg.display.brightnessPct);    // wrong type -> default
  TEST_ASSERT_EQUAL(0, cfg.display.dimAfterS);         // 0 = never
  TEST_ASSERT_EQUAL(120, cfg.power.sleepAfterMin);
  TEST_ASSERT_EQUAL(9, cfg.power.shutdownPct);         // cross-field: below lowBatteryWarnPct 10
  TEST_ASSERT_EQUAL(1500, cfg.menu.holdMs);            // cross-field: offHoldMs 1000 + 500
  TEST_ASSERT_TRUE(hasWarning(rep, "touch.pressPct"));
  TEST_ASSERT_TRUE(hasWarning(rep, "display.theme"));
  TEST_ASSERT_TRUE(hasWarning(rep, "menu.holdMs"));
}

// ---- level structure -----------------------------------------------------------
void test_drop_malformed_level() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_BAD_LEVELS));
  config::load(doc.as<JsonVariantConst>(), cfg, rep);
  dumpReport(rep);
  TEST_ASSERT_TRUE(rep.ok());                       // at boot a bad level is a warning
  TEST_ASSERT_EQUAL(1, cfg.levels.count);
  TEST_ASSERT_EQUAL_STRING("GOOD", cfg.levels.levels[0].name);
  TEST_ASSERT_TRUE(hasWarning(rep, "SHORT"));
  TEST_ASSERT_TRUE(hasWarning(rep, "unknown action"));

  rep.clear();
  config::load(doc.as<JsonVariantConst>(), cfg, rep, true);   // strict: an edit with these levels is refused
  TEST_ASSERT_FALSE(rep.ok());
}

void test_goto_clamped_after_level_removal() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_GOTO));
  TEST_ASSERT_TRUE(config::load(doc.as<JsonVariantConst>(), cfg, rep));
  TEST_ASSERT_EQUAL(3, cfg.levels.levels[0].buttons[0].gotoLevel);
  TEST_ASSERT_EQUAL(0, rep.count);

  doc["levels"].as<JsonArray>().remove(2);          // the portal removes level 3
  rep.clear();
  config::load(doc.as<JsonVariantConst>(), cfg, rep);
  dumpReport(rep);
  TEST_ASSERT_EQUAL(2, cfg.levels.count);
  TEST_ASSERT_EQUAL(1, cfg.levels.levels[0].buttons[0].gotoLevel);
  TEST_ASSERT_TRUE(hasWarning(rep, "goToLevel"));
}

// ---- partial PUT ----------------------------------------------------------------
void test_partial_put_rejected_as_whole() {
  JsonDocument live;
  TEST_ASSERT_TRUE(parse(live, SAMPLE_OWNER));
  TEST_ASSERT_TRUE(config::load(live.as<JsonVariantConst>(), cfg, rep));
  uint16_t before = cfg.power.offHoldMs;

  JsonDocument patch;
  TEST_ASSERT_TRUE(parse(patch, R"({ "power": { "offHoldMs": 2800 }, "display": { "theme": "green" } })"));
  JsonDocument candidate = live;                    // merge into a copy
  config::merge(candidate.as<JsonObject>(), patch.as<JsonObjectConst>());

  Config trial;
  rep.clear();
  bool ok = config::load(candidate.as<JsonVariantConst>(), trial, rep, true);
  dumpReport(rep);
  TEST_ASSERT_FALSE_MESSAGE(ok, "menu.holdMs 3000 < offHoldMs 2800 + 500 must reject the PUT");
  TEST_ASSERT_EQUAL(before, cfg.power.offHoldMs);   // live untouched
  TEST_ASSERT_EQUAL(THEME_AMBER, cfg.display.theme);

  // The same PUT with a consistent menu.holdMs passes and both keys apply.
  JsonDocument patch2;
  TEST_ASSERT_TRUE(parse(patch2, R"({ "power": { "offHoldMs": 2800 }, "menu": { "holdMs": 3500 }, "display": { "theme": "green" } })"));
  JsonDocument candidate2 = live;
  config::merge(candidate2.as<JsonObject>(), patch2.as<JsonObjectConst>());
  rep.clear();
  TEST_ASSERT_TRUE(config::load(candidate2.as<JsonVariantConst>(), trial, rep, true));
  TEST_ASSERT_EQUAL(2800, trial.power.offHoldMs);
  TEST_ASSERT_EQUAL(3500, trial.menu.holdMs);
  TEST_ASSERT_EQUAL(THEME_GREEN, trial.display.theme);
  TEST_ASSERT_EQUAL(4, trial.levels.count);         // the rest of the document survived the merge
}

// ---- schema ---------------------------------------------------------------------
void test_newer_schema_refused() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_NEWER));
  TEST_ASSERT_EQUAL(-1, config::migrateDocument(doc.as<JsonObject>(), rep));

  JsonDocument doc2;
  TEST_ASSERT_TRUE(parse(doc2, R"({ "display": { "theme": "green" } })"));   // no schema key: taken as 1
  TEST_ASSERT_EQUAL(SCHEMA_VERSION, config::migrateDocument(doc2.as<JsonObject>(), rep));
  TEST_ASSERT_EQUAL(1, doc2["schema"].as<int>());
}

// ---- round trip -------------------------------------------------------------------
void test_save_reload_round_trip() {
  JsonDocument doc;
  TEST_ASSERT_TRUE(parse(doc, SAMPLE_EXAMPLE));
  TEST_ASSERT_TRUE(config::load(doc.as<JsonVariantConst>(), cfg, rep));
  uint32_t rev0 = cfg.revision;

  cfg.display.theme = THEME_GREEN;                  // the edit
  cfg.touch.hasPadPressPct = true; cfg.touch.padPressPct[0] = 1.0f; cfg.touch.padPressPct[1] = 0;
  cfg.touch.padPressPct[2] = 3.0f; cfg.touch.padPressPct[3] = 3.0f;
  strcpy(cfg.levels.levels[0].buttons[2].key, "SPACE");
  cfg.levels.levels[0].buttons[2].keyMode = KeyMode::Hold;
  config::save(cfg, doc.as<JsonObject>());
  uint32_t rev1 = config::bumpRevision(doc.as<JsonObject>());
  TEST_ASSERT_EQUAL(rev0 + 1, rev1);

  std::string text;
  serializeJsonPretty(doc, text);
  TEST_ASSERT_TRUE(text.find("\"keep\": \"me\"") != std::string::npos);   // unknown key preserved

  JsonDocument again;
  TEST_ASSERT_TRUE(parse(again, text.c_str()));
  Config back;
  rep.clear();
  TEST_ASSERT_TRUE(config::load(again.as<JsonVariantConst>(), back, rep));
  dumpReport(rep);
  TEST_ASSERT_EQUAL(0, rep.count);
  TEST_ASSERT_EQUAL(THEME_GREEN, back.display.theme);
  TEST_ASSERT_EQUAL(rev1, back.revision);
  TEST_ASSERT_TRUE(back.touch.hasPadPressPct);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, back.touch.padPressPct[0]);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, back.touch.padPressPct[1]);
  TEST_ASSERT_EQUAL_STRING("SPACE", back.levels.levels[0].buttons[2].key);
  TEST_ASSERT_EQUAL((int)KeyMode::Hold, (int)back.levels.levels[0].buttons[2].keyMode);
  TEST_ASSERT_TRUE(back.levels.levels[0].hasVibration);
  TEST_ASSERT_EQUAL(0, memcmp(back.hardware.padChannels, cfg.hardware.padChannels, 4));
  TEST_ASSERT_EQUAL_STRING(cfg.device.ownerLabel[1], back.device.ownerLabel[1]);
}

// ---- console-style scalar access ------------------------------------------------
void test_scalar_text_access() {
  config::defaults(cfg);
  const SettingDesc* d = findSetting("display.theme");
  TEST_ASSERT_NOT_NULL(d);
  char buf[32];
  TEST_ASSERT_TRUE(config::getScalarText(cfg, *d, buf, sizeof buf));
  TEST_ASSERT_EQUAL_STRING("amber", buf);
  TEST_ASSERT_TRUE(config::setScalarText(cfg, *d, "GREEN", &rep));
  TEST_ASSERT_EQUAL(THEME_GREEN, cfg.display.theme);
  TEST_ASSERT_FALSE(config::setScalarText(cfg, *d, "purple", &rep));

  d = findSetting("power.sleepAfterMin");
  TEST_ASSERT_TRUE(config::setScalarText(cfg, *d, "300", &rep));
  TEST_ASSERT_EQUAL(120, cfg.power.sleepAfterMin);
  TEST_ASSERT_TRUE(hasWarning(rep, "out of range"));
  TEST_ASSERT_TRUE(config::setScalarText(cfg, *d, "0", &rep));
  TEST_ASSERT_EQUAL(0, cfg.power.sleepAfterMin);
  TEST_ASSERT_FALSE(config::setScalarText(cfg, *d, "abc", &rep));
  TEST_ASSERT_NULL(findSetting("no.such.key"));
}

// ---- saving into an empty document (the first save on a blank board) ---------
void test_save_into_empty_document() {
  config::defaults(cfg);
  cfg.display.theme = THEME_CYAN;
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  config::save(cfg, root);
  TEST_ASSERT_EQUAL_STRING("cyan", config::lookup(doc.as<JsonVariantConst>(), "display.theme").as<const char*>());
  TEST_ASSERT_EQUAL(1000, config::lookup(doc.as<JsonVariantConst>(), "power.offHoldMs").as<int>());
  TEST_ASSERT_EQUAL(1, doc["levels"].size());
  TEST_ASSERT_EQUAL(4, doc["hardware"]["padChannels"].size());
  TEST_ASSERT_TRUE(config::lookup(doc.as<JsonVariantConst>(), "touch.padPressPct").isNull());
  size_t keys = 0;
  for (size_t i = 0; i < N_SETTINGS; i++) if (!config::lookup(doc.as<JsonVariantConst>(), SETTINGS[i].path).isNull()) keys++;
  TEST_ASSERT_EQUAL_MESSAGE(N_SETTINGS, keys, "every scalar must be written");

  Config back;
  rep.clear();
  TEST_ASSERT_TRUE(config::load(doc.as<JsonVariantConst>(), back, rep));
  TEST_ASSERT_EQUAL(0, rep.count);
  TEST_ASSERT_EQUAL(THEME_CYAN, back.display.theme);
  TEST_ASSERT_EQUAL(0, memcmp(&back.levels, &cfg.levels, sizeof back.levels));

  // A console-style edit on an empty document: the leaf must be created.
  JsonDocument empty;
  JsonObject r2 = empty.to<JsonObject>();
  config::ensure(r2, "power.offHoldMs").set(2800);
  TEST_ASSERT_EQUAL(2800, empty["power"]["offHoldMs"].as<int>());
  Config trial;
  rep.clear();
  TEST_ASSERT_FALSE_MESSAGE(config::load(empty.as<JsonVariantConst>(), trial, rep, true), "offHoldMs 2800 with the default menu.holdMs 3000 must be rejected");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_save_into_empty_document);
  RUN_TEST(test_defaults);
  RUN_TEST(test_load_example);
  RUN_TEST(test_load_owner_file);
  RUN_TEST(test_clamp_out_of_range);
  RUN_TEST(test_drop_malformed_level);
  RUN_TEST(test_goto_clamped_after_level_removal);
  RUN_TEST(test_partial_put_rejected_as_whole);
  RUN_TEST(test_newer_schema_refused);
  RUN_TEST(test_save_reload_round_trip);
  RUN_TEST(test_scalar_text_access);
  return UNITY_END();
}
