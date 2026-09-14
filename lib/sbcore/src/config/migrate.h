// Schema migration chain (FirmwareSpec.md §13.1 rule 7, §13.4).
#pragma once
#include <ArduinoJson.h>
#include "config/report.h"

namespace sb {
namespace config {

// Brings a document up to SCHEMA_VERSION in memory. Returns the schema the
// document now has, or -1 when the file's schema is newer than this firmware
// knows (the caller refuses it for editing). A missing `schema` is taken as 1.
int migrateDocument(ArduinoJson::JsonObject root, ConfigReport& rep);

}  // namespace config
}  // namespace sb
