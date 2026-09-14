// Configuration document <-> typed Config (FirmwareSpec.md §13.4, §13.5).
// Pure: no filesystem, no Arduino. The store (src/config/store.cpp) reads and
// writes the files and calls these.
#pragma once
#include <ArduinoJson.h>
#include "config/config.h"
#include "config/report.h"
#include "config/settings_table.h"

namespace sb {
namespace config {

// Every field to its default: descriptor defaults, structured defaults, one
// blank level.
void defaults(Config& c);

// Document -> Config with validation. Missing keys take defaults; out-of-range
// values are clamped with a warning; structural problems drop the offending
// level or fall back to the default. `strict` (a portal PUT or a console edit)
// turns structural problems and cross-field violations into errors so the edit
// is rejected as a whole. Returns rep.ok().
bool load(ArduinoJson::JsonVariantConst root, Config& out, ConfigReport& rep, bool strict = false);

// Config -> document: writes every known key at its path (creating objects),
// leaving unknown keys untouched so they round-trip. `schema` is written;
// `revision` is written only if the document has none (the store bumps it).
void save(const Config& c, ArduinoJson::JsonObject root);

// Increments the document's `revision` (§13.2) and returns the new value.
uint32_t bumpRevision(ArduinoJson::JsonObject root);

// Dotted-path helpers over a document.
ArduinoJson::JsonVariantConst lookup(ArduinoJson::JsonVariantConst root, const char* path);
ArduinoJson::JsonVariant      ensure(ArduinoJson::JsonObject root, const char* path);   // leaf slot, parents created

// Scalar access by descriptor (console get/set, Quick Menu).
bool getScalarText(const Config& c, const SettingDesc& d, char* out, size_t n);
bool setScalarText(Config& c, const SettingDesc& d, const char* text, ConfigReport* rep);   // false if unparsable

// The cross-field rules of §13.4 on their own (used by load()).
void crossField(Config& c, ConfigReport& rep, bool strict);

// A string that ArduinoJson copies into the document (a plain const char* is
// stored by pointer, which would dangle when the Config changes).
inline char* copied(const char* s) { return const_cast<char*>(s ? s : ""); }

}  // namespace config
}  // namespace sb

namespace sb {
namespace config {
// Deep-merges `patch` into `target` (§13.4: a partial PUT is merged into a copy
// of the current document before validation). Objects merge recursively;
// arrays and scalars replace; a null in the patch removes the key.
void merge(ArduinoJson::JsonObject target, ArduinoJson::JsonObjectConst patch);
}  // namespace config
}  // namespace sb
