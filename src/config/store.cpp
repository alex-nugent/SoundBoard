#include "config/store.h"
#include "config/loader.h"
#include "config/migrate.h"
#include "config/settings_table.h"
#include "hal/pins.h"
#include "hal/storage.h"
#include "diag/log.h"
#include "util/strutil.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <errno.h>

using namespace ArduinoJson;
using sb::Config;
using sb::ConfigReport;

static const char* TAG = "config";

static constexpr const char* CARD_CFG  = "/config.json";
static constexpr const char* CARD_BAK  = "/config.bak.json";
static constexpr const char* CARD_TMP  = "/config.tmp.json";
static constexpr const char* FLASH_CFG = "/config.json";
static constexpr const char* FLASH_TMP = "/config.json.tmp";
static constexpr size_t MAX_FILE = 256 * 1024;     // JSON body limit (§15.4)
static constexpr size_t CHUNK = 4096;              // one read per lock hold (§13.2)
// Card writes go out `writeChunk` bytes at a time (one lock hold each, §13.2). 512
// would make FATFS issue single-block writes (CMD24) only; `sdchunk` sets it on the
// bench. Unit 1's card failed writes at every chunk size on 2026-09-13 (rail sag
// during programming suspected), so this is not a workaround, just a knob.
uint32_t ConfigStore::writeChunk = 4096;

// §13.1 rule 6: the documents live in PSRAM.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void  deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAllocator s_alloc;
ArduinoJson::Allocator* psramAllocator() { return &s_alloc; }

static void* psAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  return p ? p : malloc(n);
}

// ---------------------------------------------------------------------------
bool ConfigStore::begin() {
  if (!doc_) doc_ = new JsonDocument(&s_alloc);
  if (!scratch_) scratch_ = new JsonDocument(&s_alloc);
  fsOk_ = LittleFS.begin(true);   // formats the spiffs partition on the first boot
  if (!fsOk_) LOG_E(TAG, "LittleFS mount failed: no flash mirror this boot");
  return fsOk_;
}

bool ConfigStore::mountCard() {
  if (card_) return true;
  Storage::Guard g;
  digitalWrite(pins::TFT_CS, HIGH);              // the screen off the bus while the card inits
  if (!SD.begin(pins::SD_CS, SPI, sdHz_)) {
    SD.end();
    pinMode(pins::SD_CS, OUTPUT); digitalWrite(pins::SD_CS, HIGH);
    card_ = false;
    return false;
  }
  card_ = true;
  uint64_t size = SD.cardSize();
  cardBytes_ = size;
  const char* type = SD.cardType() == CARD_SDHC ? "SDHC" : SD.cardType() == CARD_SD ? "SDSC" : SD.cardType() == CARD_MMC ? "MMC" : "?";
  snprintf(cardInfo_, sizeof cardInfo_, "%s %.1f GB", type, size / (1024.0 * 1024 * 1024));
  return true;
}

bool ConfigStore::remountCard(uint32_t hz) {
  unmountCard();
  sdHz_ = hz;
  delay(20);
  return mountCard();
}

bool ConfigStore::probeCard() {
  if (!card_) return false;
  bool alive;
  { Storage::Guard g; alive = SD.exists("/"); }
  if (!alive) { LOG_E(TAG, "card stopped answering: unmounted until recovered (sdcycle / reboot)"); unmountCard(); }
  return alive;
}

void ConfigStore::unmountCard() {
  if (!card_) return;
  Storage::Guard g;
  SD.end();
  pinMode(pins::SD_CS, OUTPUT); digitalWrite(pins::SD_CS, HIGH);
  card_ = false;
}

const char* ConfigStore::sourceName() const {
  switch (source_) {
    case ConfigSource::Card:     return "card";
    case ConfigSource::CardTmp:  return "card (interrupted save adopted)";
    case ConfigSource::Backup:   return "card backup (config.bak.json)";
    case ConfigSource::Mirror:   return "flash mirror";
    case ConfigSource::Defaults: return "defaults";
    default:                     return "none";
  }
}

uint32_t ConfigStore::revision() const {
  if (!doc_) return 0;
  return (*doc_)["revision"] | 0UL;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
bool ConfigStore::readFile(bool card, const char* path, char*& buf, size_t& len) {
  buf = nullptr; len = 0;
  fs::FS& fs = card ? (fs::FS&)SD : (fs::FS&)LittleFS;
  File f;
  { Storage::Guard g; f = fs.open(path, FILE_READ); }
  if (!f || f.isDirectory()) { if (f) f.close(); return false; }
  size_t size = f.size();
  if (size == 0 || size > MAX_FILE) { LOG_E(TAG, "%s: size %u not usable", path, (unsigned)size); f.close(); return false; }
  buf = static_cast<char*>(psAlloc(size + 1));
  if (!buf) { f.close(); return false; }
  size_t got = 0;
  while (got < size) {
    size_t want = size - got < CHUNK ? size - got : CHUNK;
    int n;
    { Storage::Guard g; n = f.read(reinterpret_cast<uint8_t*>(buf) + got, want); }
    if (n <= 0) break;
    got += (size_t)n;
  }
  { Storage::Guard g; f.close(); }
  buf[got] = 0;
  len = got;
  if (got != size) { LOG_E(TAG, "%s: short read %u of %u", path, (unsigned)got, (unsigned)size); free(buf); buf = nullptr; return false; }
  return true;
}

// The smallest prefix that ArduinoJson rejects as invalid is where the error
// is; a document that merely ends early (a missing closing brace) reports its
// last line.
void ConfigStore::locateError(const char* buf, size_t len, int& line, int& col) {
  JsonDocument tmp(&s_alloc);               // its own document: scratch_ may hold a candidate
  size_t pos = len;
  DeserializationError e = deserializeJson(tmp, buf, len);
  if (e != DeserializationError::IncompleteInput) {
    size_t lo = 0, hi = len;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      e = deserializeJson(tmp, buf, mid);
      if (e == DeserializationError::InvalidInput || e == DeserializationError::TooDeep) hi = mid; else lo = mid + 1;
    }
    pos = lo;
  }
  line = 1; col = 1;
  for (size_t i = 0; i < pos && i < len; i++) { if (buf[i] == '\n') { line++; col = 1; } else col++; }
}

bool ConfigStore::parseFile(bool card, const char* path, JsonDocument& into) {
  char* buf; size_t len;
  if (!readFile(card, path, buf, len)) return false;
  DeserializationError e = deserializeJson(into, (const char*)buf, len);
  bool ok = !e;
  if (!ok) {
    int line, col;
    locateError(buf, len, line, col);
    snprintf(parseErr_, sizeof parseErr_, "%s%s: %s at line %d, column %d", card ? "card " : "flash ", path, e.c_str(), line, col);
    LOG_E(TAG, "%s", parseErr_);
    into.clear();
  } else if (!into.is<JsonObject>()) {
    snprintf(parseErr_, sizeof parseErr_, "%s%s: not a JSON object", card ? "card " : "flash ", path);
    LOG_E(TAG, "%s", parseErr_);
    into.clear();
    ok = false;
  }
  free(buf);
  return ok;
}

bool ConfigStore::serialise(char*& buf, size_t& len) {
  size_t need = measureJsonPretty(*doc_) + 1;
  buf = static_cast<char*>(psAlloc(need));
  if (!buf) return false;
  len = serializeJsonPretty(*doc_, buf, need);
  buf[len] = 0;
  return true;
}

static bool writeChunked(File& f, const char* buf, size_t len) {
  static uint8_t bounce[CHUNK];              // internal RAM: the SD write path reads its source directly
  size_t chunk = ConfigStore::writeChunk; if (chunk < 64 || chunk > CHUNK) chunk = CHUNK;
  size_t done = 0;
  while (done < len) {
    size_t n = len - done < chunk ? len - done : chunk;
    size_t w;
    errno = 0;
    memcpy(bounce, buf + done, n);
    { Storage::Guard g; w = f.write(bounce, n); }
    if (w != n) { LOG_E(TAG, "write: %u of %u bytes at offset %u, errno %d", (unsigned)w, (unsigned)n, (unsigned)done, errno); return false; }
    done += n;
  }
  return true;
}

bool ConfigStore::writeCard(const char* buf, size_t len, char* err, size_t errLen) {
  if (!card_) { snprintf(err, errLen, "no card"); return false; }
  Storage::lock(); SD.remove(CARD_TMP); File f = SD.open(CARD_TMP, FILE_WRITE); Storage::unlock();
  if (!f) { snprintf(err, errLen, "cannot create %s", CARD_TMP); return false; }
  bool ok = writeChunked(f, buf, len);
  { Storage::Guard g; f.flush(); f.close(); }
  if (!ok) { snprintf(err, errLen, "write to %s failed", CARD_TMP); Storage::Guard g; SD.remove(CARD_TMP); return false; }

  // Re-open and re-parse: the bytes on the card must be a valid document.
  if (!parseFile(true, CARD_TMP, *scratch_)) {
    snprintf(err, errLen, "verify of %s failed", CARD_TMP);
    Storage::Guard g; SD.remove(CARD_TMP); return false;
  }
  uint32_t wrote = (*scratch_)["revision"] | 0UL;
  scratch_->clear();
  if (wrote != revision()) { snprintf(err, errLen, "verify: revision mismatch"); Storage::Guard g; SD.remove(CARD_TMP); return false; }

  Storage::Guard g;
  if (source_ == ConfigSource::Card || source_ == ConfigSource::CardTmp) {
    // The running configuration came from config.json: rotate it to the backup (FAT cannot rename over a name).
    if (SD.exists(CARD_BAK) && !SD.remove(CARD_BAK)) { snprintf(err, errLen, "cannot delete %s", CARD_BAK); return false; }
    if (SD.exists(CARD_CFG) && !SD.rename(CARD_CFG, CARD_BAK)) { snprintf(err, errLen, "cannot rename %s to %s", CARD_CFG, CARD_BAK); return false; }
  } else {
    // From the backup, the mirror or defaults: keep the backup, drop the bad file.
    if (SD.exists(CARD_CFG) && !SD.remove(CARD_CFG)) { snprintf(err, errLen, "cannot delete %s", CARD_CFG); return false; }
  }
  if (!SD.rename(CARD_TMP, CARD_CFG)) { snprintf(err, errLen, "cannot rename %s to %s", CARD_TMP, CARD_CFG); return false; }
  source_ = ConfigSource::Card;
  cardBad_ = false;
  return true;
}

bool ConfigStore::writeMirror(const char* buf, size_t len, char* err, size_t errLen) {
  if (!fsOk_) { snprintf(err, errLen, "flash not mounted"); return false; }
  Storage::lock(); LittleFS.remove(FLASH_TMP); File f = LittleFS.open(FLASH_TMP, FILE_WRITE); Storage::unlock();
  if (!f) { snprintf(err, errLen, "cannot create the mirror"); return false; }
  bool ok = writeChunked(f, buf, len);
  { Storage::Guard g; f.flush(); f.close(); }
  if (!ok) { snprintf(err, errLen, "mirror write failed"); Storage::Guard g; LittleFS.remove(FLASH_TMP); return false; }
  Storage::Guard g;
  if (!LittleFS.rename(FLASH_TMP, FLASH_CFG)) {                // atomic on LittleFS
    snprintf(err, errLen, "mirror rename failed"); LittleFS.remove(FLASH_TMP); return false;
  }
  mirrorOk_ = true;
  return true;
}

bool ConfigStore::copyDoc(JsonDocument& dst, JsonDocument& src) {
  dst.clear();
  return dst.set(src.as<JsonVariantConst>());
}

// ---------------------------------------------------------------------------
// Load order (§13.2)
// ---------------------------------------------------------------------------
void ConfigStore::load(Config& live, ConfigReport& rep) {
  rep.clear();
  schemaTooNew_ = cardBad_ = false;
  source_ = ConfigSource::None;
  parseErr_[0] = 0; message_[0] = 0;
  doc_->clear(); scratch_->clear();
  JsonDocument& cand = *scratch_;           // the card's candidate
  ConfigReport scratchRep;                  // migration notes of copies not used are dropped

  bool haveCard = false, haveBackup = false, cardHadConfig = false, adoptedTmp = false;
  mountCard();
  if (card_) {
    Storage::lock();
    bool cfgExists = SD.exists(CARD_CFG), tmpExists = SD.exists(CARD_TMP), bakExists = SD.exists(CARD_BAK);
    Storage::unlock();
    if (!cfgExists && tmpExists) {          // a save was interrupted between the two renames
      if (parseFile(true, CARD_TMP, cand)) {
        Storage::Guard g;
        if (SD.rename(CARD_TMP, CARD_CFG)) { cfgExists = true; adoptedTmp = true; LOG_W(TAG, "interrupted save: %s adopted as %s", CARD_TMP, CARD_CFG); }
      }
      cand.clear();
    }
    if (cfgExists) {
      cardHadConfig = true;
      if (parseFile(true, CARD_CFG, cand)) {
        int s = sb::config::migrateDocument(cand.as<JsonObject>(), rep);
        if (s < 0) { schemaTooNew_ = true; cardBad_ = true; LOG_E(TAG, "card config.json has schema %d, newer than this firmware (%d)", (int)cand["schema"], (int)sb::SCHEMA_VERSION); cand.clear(); }
        else haveCard = true;
      } else cardBad_ = true;
    }
    if (!haveCard && bakExists) {
      if (parseFile(true, CARD_BAK, cand) && sb::config::migrateDocument(cand.as<JsonObject>(), scratchRep) >= 0) haveBackup = true;
      else cand.clear();
    }
  }

  // The mirror is parsed straight into the live document; a winning card copy replaces it.
  bool haveMirror = false;
  if (fsOk_) {
    bool exists; { Storage::Guard g; exists = LittleFS.exists(FLASH_CFG); }
    if (exists && parseFile(false, FLASH_CFG, *doc_) && sb::config::migrateDocument(doc_->as<JsonObject>(), scratchRep) >= 0) haveMirror = true;
    else doc_->clear();
  }
  mirrorOk_ = haveMirror;

  char err[64];
  if (haveCard) {
    uint32_t revCard = cand["revision"] | 0UL;
    uint32_t revMir  = haveMirror ? ((*doc_)["revision"] | 0UL) : 0;
    if (haveMirror && revMir > revCard) {
      source_ = ConfigSource::Mirror;
      LOG_W(TAG, "mirror revision %lu is newer than the card's %lu: using the mirror and writing it to the card", (unsigned long)revMir, (unsigned long)revCard);
      char* buf; size_t len;
      if (serialise(buf, len)) { source_ = ConfigSource::Card; if (!writeCard(buf, len, err, sizeof err)) { LOG_E(TAG, "card refresh failed: %s", err); source_ = ConfigSource::Mirror; } free(buf); }
    } else {
      copyDoc(*doc_, cand);
      source_ = adoptedTmp ? ConfigSource::CardTmp : ConfigSource::Card;
      if (!haveMirror || revMir != revCard) {
        char* buf; size_t len;
        if (serialise(buf, len)) { if (!writeMirror(buf, len, err, sizeof err)) LOG_E(TAG, "mirror refresh failed: %s", err); else LOG_I(TAG, "mirror refreshed to revision %lu", (unsigned long)revCard); free(buf); }
      }
    }
  } else if (haveBackup) {
    copyDoc(*doc_, cand);
    source_ = ConfigSource::Backup;
    sb::copyStr(message_, sizeof message_, "config.json unreadable, using backup");
    LOG_W(TAG, "%s", message_);
  } else if (haveMirror) {
    source_ = ConfigSource::Mirror;
    if (cardHadConfig) {
      sb::copyStr(message_, sizeof message_, schemaTooNew_ ? "configuration needs newer firmware" : "card configuration unreadable, using flash copy");
      LOG_W(TAG, "%s", message_);
    } else if (!card_) {
      sb::copyStr(message_, sizeof message_, "no card, using flash copy");
      LOG_W(TAG, "%s", message_);
    } else if (card_) {
      char* buf; size_t len;
      if (serialise(buf, len)) { if (writeCard(buf, len, err, sizeof err)) { LOG_I(TAG, "card had no configuration: mirror written to it"); source_ = ConfigSource::Card; } else LOG_E(TAG, "writing the mirror to the card failed: %s", err); free(buf); }
    }
  } else {
    JsonObject root = doc_->to<JsonObject>();
    sb::config::defaults(live);
    sb::config::save(live, root);           // the effective document, so `dump` and the first save show it
    root["revision"] = 0;
    source_ = ConfigSource::Defaults;
    sb::copyStr(message_, sizeof message_, schemaTooNew_ ? "configuration needs newer firmware" : "no configuration, defaults in use");
    LOG_W(TAG, "%s", message_);
  }
  cand.clear();

  sb::config::load(doc_->as<JsonVariantConst>(), live, rep, false);
  LOG_I(TAG, "configuration from %s, revision %lu, %u warning(s)%s", sourceName(), (unsigned long)live.revision, (unsigned)rep.count, card_ ? "" : " (no card)");
  for (uint8_t i = 0; i < rep.count; i++) LOG_W(TAG, "  %s", rep.items[i].text);
}

// ---------------------------------------------------------------------------
// Save (§13.2)
// ---------------------------------------------------------------------------
bool ConfigStore::save(Config& live, bool userAction, char* err, size_t errLen) {
  (void)userAction;
  if (schemaTooNew_) { snprintf(err, errLen, "update the firmware first (card file has a newer schema)"); return false; }
  JsonObject root = doc_->as<JsonObject>();
  if (root.isNull()) root = doc_->to<JsonObject>();
  sb::config::save(live, root);
  uint32_t prev = root["revision"] | 0UL;
  uint32_t rev = sb::config::bumpRevision(root);
  live.revision = rev;

  char* buf; size_t len;
  if (!serialise(buf, len)) { root["revision"] = prev; live.revision = prev; snprintf(err, errLen, "out of memory"); return false; }

  bool ok = true;
  cardLess_ = false;
  if (!mountCard()) {
    cardLess_ = true;
    LOG_W(TAG, "save: no card, writing the mirror only (card-less)");
  } else if (!writeCard(buf, len, err, errLen)) {
    if (!probeCard()) { cardLess_ = true; LOG_W(TAG, "save: card failed (%s), writing the mirror only (card-less)", err); }
    else ok = false;
  }
  if (ok && !writeMirror(buf, len, err, errLen)) ok = false;
  free(buf);
  if (!ok) { root["revision"] = prev; live.revision = prev; LOG_E(TAG, "save failed: %s", err); return false; }
  LOG_I(TAG, "saved revision %lu to %s", (unsigned long)rev, cardLess_ ? "the mirror (card-less)" : "card and mirror");
  return true;
}

// ---------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------
bool ConfigStore::applyCandidate(JsonDocument& cand, Config& live, ConfigReport& rep, char* err, size_t errLen) {
  static Config* trialP = static_cast<Config*>(psAlloc(sizeof(Config)));   // 13 KB: not on the stack, not in internal RAM (Wi-Fi needs it, CP-10)
  Config& trial = *trialP;
  rep.clear();
  int s = sb::config::migrateDocument(cand.as<JsonObject>(), rep);
  if (s < 0) { snprintf(err, errLen, "schema newer than this firmware"); return false; }
  if (!sb::config::load(cand.as<JsonVariantConst>(), trial, rep, true)) {
    const char* first = "validation failed";
    for (uint8_t i = 0; i < rep.count; i++) if (rep.items[i].error) { first = rep.items[i].text; break; }
    snprintf(err, errLen, "%s", first);
    return false;
  }
  trial.revision = live.revision;
  live = trial;
  copyDoc(*doc_, cand);
  return true;
}

bool ConfigStore::knownPath(const char* path) {
  if (!path || !*path) return false;
  static const char* const STRUCTURED[] = {
    "levels", "device.ownerLabel", "hardware.padChannels", "pads.roles", "touch.padPressPct",
    "audio.cues", "audio.cues.startup", "audio.cues.click", "audio.cues.saved", "audio.cues.lowBattery",
    "levelChange.vibration.pattern"
  };
  for (const char* s : STRUCTURED) if (strcmp(s, path) == 0) return true;
  size_t n = strlen(path);
  for (size_t i = 0; i < sb::N_SETTINGS; i++) {
    const char* p = sb::SETTINGS[i].path;
    if (strcmp(p, path) == 0) return true;
    if (strncmp(p, path, n) == 0 && p[n] == '.') return true;   // a section prefix, e.g. "power"
  }
  return false;
}

bool ConfigStore::setPath(const char* path, const char* text, Config& live, ConfigReport& rep, char* err, size_t errLen) {
  if (schemaTooNew_) { snprintf(err, errLen, "update the firmware first (card file has a newer schema)"); return false; }
  if (!knownPath(path)) { snprintf(err, errLen, "unknown setting \"%s\"", path); return false; }
  JsonDocument& cand = *scratch_;
  if (!copyDoc(cand, *doc_)) { snprintf(err, errLen, "out of memory"); return false; }
  JsonObject root = cand.as<JsonObject>();
  if (root.isNull()) root = cand.to<JsonObject>();
  JsonVariant leaf = sb::config::ensure(root, path);

  const sb::SettingDesc* d = sb::findSetting(path);
  bool asString = d && (d->type == sb::SType::String || d->type == sb::SType::Enum);
  JsonDocument value(&s_alloc);           // the text as JSON if it parses, else as a string
  DeserializationError e = asString ? DeserializationError(DeserializationError::InvalidInput)
                                    : deserializeJson(value, (const char*)text, strlen(text));
  if (!e) leaf.set(value.as<JsonVariantConst>());
  else leaf.set(sb::config::copied(text));

  bool ok = applyCandidate(cand, live, rep, err, errLen);
  cand.clear();
  return ok;
}

bool ConfigStore::mergeText(const char* text, Config& live, ConfigReport& rep, char* err, size_t errLen) {
  if (schemaTooNew_) { snprintf(err, errLen, "update the firmware first (card file has a newer schema)"); return false; }
  JsonDocument patch(&s_alloc);
  DeserializationError e = deserializeJson(patch, (const char*)text, strlen(text));
  if (e) { snprintf(err, errLen, "not valid JSON: %s", e.c_str()); return false; }
  if (!patch.is<JsonObject>()) { snprintf(err, errLen, "expected a JSON object"); return false; }
  JsonDocument& cand = *scratch_;
  if (!copyDoc(cand, *doc_)) { snprintf(err, errLen, "out of memory"); return false; }
  JsonObject root = cand.as<JsonObject>();
  if (root.isNull()) root = cand.to<JsonObject>();
  sb::config::merge(root, patch.as<JsonObjectConst>());
  bool ok = applyCandidate(cand, live, rep, err, errLen);
  cand.clear();
  return ok;
}

bool ConfigStore::applyText(const char* text, bool replace, bool keepHardware, Config& live, ConfigReport& rep, char* err, size_t errLen) {
  if (schemaTooNew_) { snprintf(err, errLen, "update the firmware first (card file has a newer schema)"); return false; }
  JsonDocument patch(&s_alloc);
  DeserializationError e = deserializeJson(patch, (const char*)text, strlen(text));
  if (e) { snprintf(err, errLen, "not valid JSON: %s", e.c_str()); return false; }
  if (!patch.is<JsonObject>()) { snprintf(err, errLen, "expected a JSON object"); return false; }
  JsonDocument& cand = *scratch_;
  if (!copyDoc(cand, *doc_)) { snprintf(err, errLen, "out of memory"); return false; }
  JsonObject root = cand.as<JsonObject>();
  if (root.isNull()) root = cand.to<JsonObject>();
  JsonObject p = patch.as<JsonObject>();
  if (!keepHardware) {
    p.remove("hardware");
    if (replace && !root["hardware"].isNull()) p["hardware"] = root["hardware"];   // the board's own stays
  }
  if (replace) { cand.clear(); copyDoc(cand, patch); }
  else sb::config::merge(root, patch.as<JsonObjectConst>());
  bool ok = applyCandidate(cand, live, rep, err, errLen);
  cand.clear();
  return ok;
}

bool ConfigStore::editDocument(EditFn fn, void* ctx, Config& live, ConfigReport& rep, char* err, size_t errLen) {
  if (schemaTooNew_) { snprintf(err, errLen, "update the firmware first (card file has a newer schema)"); return false; }
  JsonDocument& cand = *scratch_;
  if (!copyDoc(cand, *doc_)) { snprintf(err, errLen, "out of memory"); return false; }
  JsonObject root = cand.as<JsonObject>();
  if (root.isNull()) root = cand.to<JsonObject>();
  bool ok = fn(root, ctx) && applyCandidate(cand, live, rep, err, errLen);
  if (!ok && !err[0]) snprintf(err, errLen, "nothing to change");
  cand.clear();
  return ok;
}

bool ConfigStore::resetScalars(Config& live, ConfigReport& rep, char* err, size_t errLen) {
  if (schemaTooNew_) { snprintf(err, errLen, "update the firmware first (card file has a newer schema)"); return false; }
  JsonDocument& cand = *scratch_;
  if (!copyDoc(cand, *doc_)) { snprintf(err, errLen, "out of memory"); return false; }
  JsonObject root = cand.as<JsonObject>();
  if (root.isNull()) root = cand.to<JsonObject>();
  for (size_t i = 0; i < sb::N_SETTINGS; i++) {
    const sb::SettingDesc& d = sb::SETTINGS[i];
    if (!strncmp(d.path, "hardware.", 9) || !strncmp(d.path, "wifi.", 5)) continue;   // §15.1: kept
    JsonVariant leaf = sb::config::ensure(root, d.path);
    if (d.type == sb::SType::String || d.type == sb::SType::Enum) leaf.set(sb::config::copied(d.def));
    else {
      JsonDocument value(&s_alloc);
      if (!deserializeJson(value, d.def)) leaf.set(value.as<JsonVariantConst>()); else leaf.set(sb::config::copied(d.def));
    }
  }
  bool ok = applyCandidate(cand, live, rep, err, errLen);
  cand.clear();
  return ok;
}

void ConfigStore::exportTo(const Config& live, Print& out, bool pretty, bool stripSecrets) {
  JsonDocument& tmp = *scratch_;
  tmp.clear();
  if (!copyDoc(tmp, *doc_)) { out.print("{}"); return; }
  JsonObject root = tmp.as<JsonObject>();
  if (root.isNull()) root = tmp.to<JsonObject>();
  sb::config::save(live, root);                                 // the effective document: every key, unknown ones kept
  root["revision"] = (unsigned long)live.revision;
  if (stripSecrets) {
    if (!root["setup"].isNull()) root["setup"].as<JsonObject>().remove("password");
    if (!root["wifi"].isNull())  root["wifi"].as<JsonObject>().remove("password");
  }
  if (pretty) serializeJsonPretty(tmp, out); else serializeJson(tmp, out);
  tmp.clear();
}

bool ConfigStore::factory(Config& live, char* err, size_t errLen) {
  static Config* dP = static_cast<Config*>(psAlloc(sizeof(Config)));
  Config& d = *dP;
  sb::config::defaults(d);
  doc_->clear();
  JsonObject root = doc_->to<JsonObject>();
  sb::config::save(d, root);
  root["revision"] = 0;
  live = d;
  schemaTooNew_ = false;
  cardBad_ = false;
  source_ = ConfigSource::Defaults;        // save() then keeps config.bak.json and replaces config.json
  bool ok = save(live, true, err, errLen);
  // Runtime state and touch calibration go too; the battery calibration (sb-cal method/k/tcal) is hardware and stays.
  Preferences p;
  if (p.begin("sb-state", false)) { p.clear(); p.end(); }
  if (p.begin("sb-cal", false)) { if (p.isKey("touch")) p.remove("touch"); p.end(); }
  LOG_W(TAG, "factory reset: defaults written (%s), runtime state and touch baselines cleared", ok ? "ok" : err);
  return ok;
}

void ConfigStore::dump(Print& out) {
  serializeJsonPretty(*doc_, out);
  out.println();
}

bool ConfigStore::getText(const Config& live, const char* path, Print& out) {
  JsonDocument& tmp = *scratch_;
  tmp.clear();
  JsonObject root = tmp.to<JsonObject>();
  sb::config::save(live, root);
  root["revision"] = (unsigned long)live.revision;
  JsonVariantConst v = (path && *path && strcmp(path, ".") != 0) ? sb::config::lookup(root, path) : (JsonVariantConst)root;
  bool ok = !v.isNull();
  if (ok) { serializeJsonPretty(v, out); out.println(); }
  tmp.clear();
  return ok;
}
