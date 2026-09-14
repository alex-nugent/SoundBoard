#include "config/migrate.h"
#include "config/config.h"

namespace sb {
namespace config {

// migrate_N_to_N+1(root): rename keys, change meanings. None exist yet: schema
// 1 is the first. Add a case below when SCHEMA_VERSION bumps.

int migrateDocument(ArduinoJson::JsonObject root, ConfigReport& rep) {
  int s;
  ArduinoJson::JsonVariant sv = root["schema"];
  if (sv.isNull()) { s = 1; root["schema"] = 1; }
  else if (!sv.is<int>()) { rep.warn("schema: not a number, assuming %d", (int)SCHEMA_VERSION); s = SCHEMA_VERSION; root["schema"] = s; }
  else s = sv.as<int>();

  if (s > SCHEMA_VERSION) return -1;
  if (s < 1) { s = 1; root["schema"] = 1; }

  while (s < SCHEMA_VERSION) {
    switch (s) {
      // case 1: migrate_1_to_2(root); break;
      default: break;
    }
    s++;
    root["schema"] = s;
    rep.warn("configuration migrated to schema %d", s);
  }
  return s;
}

}  // namespace config
}  // namespace sb
