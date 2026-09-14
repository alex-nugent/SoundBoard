// The `levels` array: loader, saver, name tables (FirmwareSpec.md §5.1, §13.4).
#pragma once
#include <ArduinoJson.h>
#include "config/config.h"
#include "config/report.h"

namespace sb {
namespace config {

void blankEntry(Entry& e);
void defaultLevels(Config& c);                       // one level, blank entries
void loadLevels(ArduinoJson::JsonVariantConst root, Config& c, ConfigReport& rep, bool strict);
void saveLevels(const Config& c, ArduinoJson::JsonObject root);

const char* actionName(Action a);
bool        actionFromName(const char* s, Action& out);
const char* keyModeName(KeyMode m);
bool        keyModeFromName(const char* s, KeyMode& out);
const char* roleName(Role r);
bool        roleFromName(const char* s, Role& out);
const char* jackName(JackMode j);                    // Follow/Pulse/Off; Inherit -> nullptr
bool        jackFromName(const char* s, JackMode& out);

}  // namespace config
}  // namespace sb
