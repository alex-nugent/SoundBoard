// The configuration store (FirmwareSpec.md §13.2): the card files, the flash
// mirror, the load order, the save transaction, and the candidate-validate-
// apply path used by the console (and by the portal from Phase 10).
#pragma once
#include <ArduinoJson.h>
#include <Print.h>
#include "config/config.h"
#include "config/report.h"

enum class ConfigSource : uint8_t { None, Card, CardTmp, Backup, Mirror, Defaults };

ArduinoJson::Allocator* psramAllocator();   // §13.1 rule 6: documents live in PSRAM (the portal builds its replies with it too)

class ConfigStore {
 public:
  bool begin();                       // LittleFS (formats on first use), documents in PSRAM
  bool mountCard();                   // SD at 20 MHz under the storage lock; false = no card
  void unmountCard();
  bool remountCard(uint32_t hz);      // bench: another SD clock (`sdclk` on the console)
  bool probeCard();                   // after an I/O error: is the card still answering? (clears cardMounted() if not)
  static uint32_t writeChunk;         // bytes per card write call; 512 keeps every write a single-block command
  uint32_t sdHz() const { return sdHz_; }
  bool cardMounted() const { return card_; }
  const char* cardInfo() const { return cardInfo_; }
  uint64_t cardBytes() const { return cardBytes_; }

  // §13.2 load order. Fills `live`; the report holds the warnings of the copy in use.
  void load(sb::Config& live, sb::ConfigReport& rep);

  // §13.2 save protocol. Returns false with the reason in `err`.
  bool save(sb::Config& live, bool userAction, char* err, size_t errLen);

  // Console edits: `set <path> <json-or-text>` and `merge <json-object>`. The
  // change is validated strictly on a candidate copy and applied only if it
  // passes (§13.4); nothing is written until save().
  bool setPath(const char* path, const char* text, sb::Config& live, sb::ConfigReport& rep, char* err, size_t errLen);
  bool mergeText(const char* text, sb::Config& live, sb::ConfigReport& rep, char* err, size_t errLen);
  // The portal (§15.4): a partial PUT merges; a whole-file upload replaces the document. `hardware.*` from the
  // text is dropped unless keepHardware (§15.3: "this file came from this board").
  bool applyText(const char* text, bool replace, bool keepHardware, sb::Config& live, sb::ConfigReport& rep, char* err, size_t errLen);
  // An in-place edit of a candidate copy (sound delete / rename: entries and cues rewritten in one save).
  typedef bool (*EditFn)(ArduinoJson::JsonObject root, void* ctx);
  bool editDocument(EditFn fn, void* ctx, sb::Config& live, sb::ConfigReport& rep, char* err, size_t errLen);
  // Recovery "Reset settings" (§15.1): every scalar row to its default; levels, roles, hardware.*, the owner
  // label, the cues and wifi.* stay.
  bool resetScalars(sb::Config& live, sb::ConfigReport& rep, char* err, size_t errLen);
  // The live document as text (§15.4 GET /api/config and the downloads); stripSecrets removes the passwords.
  void exportTo(const sb::Config& live, Print& out, bool pretty, bool stripSecrets);   // every key present (the effective values)

  // Full factory reset: the defaults document written to the card and the mirror.
  bool factory(sb::Config& live, char* err, size_t errLen);

  void dump(Print& out);                                        // the live document, pretty-printed
  bool getText(const sb::Config& live, const char* path, Print& out);   // effective value at a path

  ConfigSource source() const { return source_; }
  const char* sourceName() const;
  bool schemaTooNew() const { return schemaTooNew_; }         // card file newer than this firmware: saves refused
  bool cardConfigBad() const { return cardBad_; }             // the card had a config.json that could not be used
  bool lastSaveCardLess() const { return cardLess_; }
  bool mirrorValid() const { return mirrorOk_; }
  bool flashOk() const { return fsOk_; }
  const char* lastParseError() const { return parseErr_; }
  const char* loadMessage() const { return message_; }        // the status-line message of the load, "" if none
  uint32_t revision() const;

 private:
  bool readFile(bool card, const char* path, char*& buf, size_t& len);
  bool parseFile(bool card, const char* path, ArduinoJson::JsonDocument& into);
  void locateError(const char* buf, size_t len, int& line, int& col);
  bool serialise(char*& buf, size_t& len);
  bool writeCard(const char* buf, size_t len, char* err, size_t errLen);
  bool writeMirror(const char* buf, size_t len, char* err, size_t errLen);
  bool copyDoc(ArduinoJson::JsonDocument& dst, ArduinoJson::JsonDocument& src);
  bool applyCandidate(ArduinoJson::JsonDocument& cand, sb::Config& live, sb::ConfigReport& rep, char* err, size_t errLen);
  static bool knownPath(const char* path);

  ArduinoJson::JsonDocument* doc_ = nullptr;      // the live document (PSRAM)
  ArduinoJson::JsonDocument* scratch_ = nullptr;  // candidates, mirror parse, verification
  uint32_t sdHz_ = 20000000;
  bool card_ = false, fsOk_ = false, mirrorOk_ = false, schemaTooNew_ = false, cardBad_ = false, cardLess_ = false;
  ConfigSource source_ = ConfigSource::None;
  char parseErr_[96] = { 0 };
  char message_[48]  = { 0 };
  char cardInfo_[32] = { 0 };
  uint64_t cardBytes_ = 0;
};
