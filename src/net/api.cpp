// The portal's endpoints (FirmwareSpec.md §15.4). Every handler runs on the
// net task: it parses the request, hands the module-touching part to the app
// task through Portal::onApp() and sends the reply from the portal's PSRAM
// buffer. File uploads are written to the card here, in 4 KB chunks under the
// storage lock (§19.3); the sound cache is rebuilt by the app afterwards.
#include "net/api.h"
#include "net/portal.h"
#include "net/page_gz.h"
#include "app/state.h"
#include "audio/wav.h"
#include "ble/keys.h"
#include "config/settings_table.h"
#include "config/levels.h"
#include "config/store.h"
#include "hal/storage.h"
#include "diag/log.h"
#include "util/strutil.h"
#include <Arduino.h>
#include <WebServer.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_core_dump.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

namespace Api {

static const char* TAG = "api";
static Portal*    P = nullptr;
static WebServer* S = nullptr;
static constexpr size_t   SOUND_MAX = 8u * 1024u * 1024u;     // §15.4
static constexpr size_t   WCHUNK = 4096;                       // one card write per lock hold (§19.3)

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------
static void jsonEscape(Print& out, const char* s) {
  for (; s && *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { out.print('\\'); out.print(c); }
    else if ((uint8_t)c < 0x20) out.print(' ');
    else out.print(c);
  }
}
static void sendBuf(int code, const char* type) {
  S->sendHeader("Cache-Control", "no-store");
  S->send_P(code, type, P->buf(), strlen(P->buf()));
}
static void sendError(int code, const char* msg) {
  BufPrint out(P->buf(), P->bufCap());
  out.print("{\"error\":\""); jsonEscape(out, msg); out.print("\"}");
  sendBuf(code, "application/json");
}
static void sendOk() { S->sendHeader("Cache-Control", "no-store"); S->send(200, "application/json", "{\"ok\":true}"); }
static bool busy() { if (!P->running()) { sendError(503, "setup is stopping"); return true; } return false; }

// ---------------------------------------------------------------------------
// The page, the captive-portal probes
// ---------------------------------------------------------------------------
static void hRoot() {
  P->noteRequest(true);
  S->sendHeader("Content-Encoding", "gzip");
  S->sendHeader("Cache-Control", "no-cache");
  S->send_P(200, "text/html; charset=utf-8", (PGM_P)PAGE_GZ, PAGE_GZ_LEN);
}
static void hRedirect() {
  P->noteRequest(false);
  char loc[40]; snprintf(loc, sizeof loc, "http://%s/", P->ip());
  S->sendHeader("Location", loc, true);
  S->sendHeader("Cache-Control", "no-store");
  S->send(302, "text/plain", "");
}
static void hNotFound() {
  if (S->uri().startsWith("/api/")) { P->noteRequest(false); sendError(404, "no such endpoint"); return; }
  hRedirect();
}

// ---------------------------------------------------------------------------
// Schema (the descriptor table, §13.5) and the fixed lists the page needs
// ---------------------------------------------------------------------------
static const char* typeName(sb::SType t) {
  switch (t) { case sb::SType::Bool: return "bool"; case sb::SType::U8: return "u8"; case sb::SType::U16: return "u16"; case sb::SType::U32: return "u32";
               case sb::SType::F32: return "f32"; case sb::SType::Enum: return "enum"; case sb::SType::String: return "string"; default: return "?"; }
}
static void hSchema() {
  P->noteRequest(false);
  JsonDocument d(psramAllocator());
  d["version"] = FW_VERSION;
  JsonArray rows = d["settings"].to<JsonArray>();
  for (size_t i = 0; i < sb::N_SETTINGS; i++) {
    const sb::SettingDesc& s = sb::SETTINGS[i];
    if (!(s.flags & sb::PORTAL)) continue;
    JsonObject o = rows.add<JsonObject>();
    o["path"] = s.path; o["type"] = typeName(s.type); o["label"] = s.label; o["group"] = s.group;
    o["min"] = s.min; o["max"] = s.max; o["step"] = s.step; o["def"] = s.def;
    if (s.enums) { JsonArray e = o["enums"].to<JsonArray>(); int n = sb::enumCount(s.enums); char nm[24]; for (int k = 0; k < n; k++) if (sb::enumName(s.enums, k, nm, sizeof nm)) e.add(nm); }
    if (s.choices) o["choices"] = s.choices;
    o["live"] = (s.flags & sb::LIVE) != 0; o["advanced"] = (s.flags & sb::ADVANCED) != 0; o["reboot"] = (s.flags & sb::REBOOT) != 0;
    o["zeroOff"] = (s.flags & sb::ZERO_OFF) != 0; o["menu"] = (s.flags & sb::MENU) != 0;
    if (s.type == sb::SType::String) o["maxLen"] = s.size - 1;
  }
  JsonArray keys = d["keys"].to<JsonArray>();
  for (uint8_t i = 0; i < sb::keyCount(); i++) keys.add(sb::keyAt(i).name);
  JsonArray acts = d["actions"].to<JsonArray>();
  for (int a = 0; a <= (int)sb::Action::GoToLevel; a++) acts.add(sb::config::actionName((sb::Action)a));
  JsonArray km = d["keyModes"].to<JsonArray>();
  for (int m = 0; m <= (int)sb::KeyMode::Tap; m++) km.add(sb::config::keyModeName((sb::KeyMode)m));
  JsonArray jm = d["jackModes"].to<JsonArray>();
  jm.add("follow"); jm.add("pulse"); jm.add("off");
  JsonArray roles = d["roles"].to<JsonArray>();
  roles.add("sound"); roles.add("level"); roles.add("none");
  JsonObject lim = d["limits"].to<JsonObject>();
  lim["maxLevels"] = sb::MAX_LEVELS; lim["maxSoundPads"] = sb::MAX_SOUND_PADS; lim["maxPattern"] = sb::MAX_PATTERN;
  lim["levelName"] = 16; lim["label"] = 24; lim["typed"] = 64; lim["soundName"] = 40; lim["ownerLine"] = 20; lim["ownerLines"] = sb::OWNER_LINES;
  lim["soundUpload"] = SOUND_MAX; lim["recordingS"] = 30;
  BufPrint out(P->buf(), P->bufCap());
  serializeJson(d, out);
  sendBuf(200, "application/json");
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static void hConfigGet() {
  P->noteRequest(false);
  if (busy()) return;
  BufPrint out(P->buf(), P->bufCap());
  P->onApp([&] { P->app().portalConfig(out, false); });
  sendBuf(200, "application/json");
}
static void hConfigDownload() {
  P->noteRequest(false);
  if (busy()) return;
  bool support = S->hasArg("support") && S->arg("support") == "1";
  BufPrint out(P->buf(), P->bufCap());
  P->onApp([&] { P->app().portalConfig(out, support); });
  S->sendHeader("Content-Disposition", support ? "attachment; filename=\"config-support.json\"" : "attachment; filename=\"config.json\"");
  sendBuf(200, "application/json");
}
// The reply of a PUT or an upload: ok / error, the revision and the warnings.
static void applyAndReply(const char* json, bool replace, bool keepHardware) {
  char err[160] = { 0 };
  bool ok = false; uint32_t rev = 0;
  BufPrint rep(P->buf() + P->bufCap() / 2, P->bufCap() / 2);   // the body sits in the first half while the app parses it
  P->onApp([&] {
    ok = P->app().portalApply(json, replace, keepHardware, err, sizeof err);
    rev = P->app().config().revision;
    P->app().portalReport(rep);
  });
  BufPrint out(P->buf(), P->bufCap() / 2);
  out.print(ok ? "{\"ok\":true" : "{\"ok\":false");
  if (!ok) { out.print(",\"error\":\""); jsonEscape(out, err); out.print("\""); }
  out.printf(",\"revision\":%lu,\"warnings\":", (unsigned long)rev);
  out.print(rep.c_str());
  out.print("}");
  sendBuf(ok ? 200 : 400, "application/json");
}
static void hConfigPut() {
  P->noteRequest(true);
  if (busy()) return;
  if (!S->hasArg("plain")) { sendError(400, "no body"); return; }
  const String& body = S->arg("plain");
  if (body.length() >= P->bufCap() / 2) { sendError(413, "body over the limit"); return; }
  memcpy(P->buf(), body.c_str(), body.length() + 1);           // the String is freed with the request; the buffer outlives the call
  applyAndReply(P->buf(), false, true);
}

// A whole config.json arrives as a multipart file: collected into the buffer, then applied as a document.
static size_t s_cfgLen = 0; static bool s_cfgOver = false;
static void hConfigUploadData() {
  HTTPUpload& up = S->upload();
  if (up.status == UPLOAD_FILE_START) { s_cfgLen = 0; s_cfgOver = false; }
  else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_cfgLen + up.currentSize >= P->bufCap() / 2) { s_cfgOver = true; return; }
    memcpy(P->buf() + s_cfgLen, up.buf, up.currentSize); s_cfgLen += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) { P->buf()[s_cfgLen] = 0; }
  else if (up.status == UPLOAD_FILE_ABORTED) { s_cfgLen = 0; s_cfgOver = true; }
}
static void hConfigUploadDone() {
  P->noteRequest(true);
  if (busy()) return;
  if (s_cfgOver) { sendError(413, "file over the 128 KB limit or the upload stopped"); return; }
  if (!s_cfgLen) { sendError(400, "no file"); return; }
  P->buf()[s_cfgLen] = 0;
  applyAndReply(P->buf(), true, S->hasArg("fromThisBoard") && S->arg("fromThisBoard") == "1");
}

// ---------------------------------------------------------------------------
// Status, diagnostics, log, core dump
// ---------------------------------------------------------------------------
static void hStatus() {
  P->noteRequest(false);
  if (busy()) return;
  BufPrint out(P->buf(), P->bufCap());
  P->onApp([&] { P->app().portalStatus(out); });
  sendBuf(200, "application/json");
}
static void hDiag() {
  P->noteRequest(false);
  if (busy()) return;
  BufPrint out(P->buf(), P->bufCap());
  P->onApp([&] { P->app().portalDiag(out); });
  sendBuf(200, "application/json");
}
static void hLog() {
  P->noteRequest(false);
  if (busy()) return;
  int n = S->hasArg("tail") ? S->arg("tail").toInt() : 200;
  if (n <= 0) n = 200; if (n > 5000) n = 5000;
  BufPrint out(P->buf(), P->bufCap());
  P->onApp([&] { Log::tail(n, out); });
  if (S->hasArg("download")) S->sendHeader("Content-Disposition", "attachment; filename=\"soundboard-log.txt\"");
  sendBuf(200, "text/plain; charset=utf-8");
}
static void hCoredump() {
  P->noteRequest(false);
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) { sendError(404, "no crash dump in flash"); return; }
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!part || size > part->size) { sendError(404, "no core dump partition"); return; }
  S->sendHeader("Content-Disposition", "attachment; filename=\"soundboard-coredump.bin\"");
  S->sendHeader("X-Firmware-Version", FW_VERSION);
  S->setContentLength(size);
  S->send(200, "application/octet-stream", "");
  size_t off = 0;
  while (off < size) {
    size_t n = size - off; if (n > WCHUNK) n = WCHUNK;
    if (esp_partition_read(part, off, P->buf(), n) != ESP_OK) break;
    S->sendContent(P->buf(), n);
    off += n;
  }
}
static void hFirmwareStatus() {
  P->noteRequest(false);
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st; const char* state = "unknown";
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK)
    state = st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending verify" : st == ESP_OTA_IMG_NEW ? "new" : st == ESP_OTA_IMG_UNDEFINED ? "flashed over USB" : "other";
  BufPrint out(P->buf(), P->bufCap());
  out.print("{\"version\":\""); jsonEscape(out, FW_VERSION); out.printf("\",\"slot\":\"%s\",\"state\":\"%s\",\"usb\":%s,\"job\":\"idle\",\"phase\":\"updates arrive with Phase 11\"}",
                                                                  run ? run->label : "?", state, P->app().battery().usb() ? "true" : "false");
  sendBuf(200, "application/json");
}

// ---------------------------------------------------------------------------
// Sounds
// ---------------------------------------------------------------------------
static void hSounds() {
  P->noteRequest(false);
  if (busy()) return;
  BufPrint out(P->buf(), P->bufCap());
  P->onApp([&] { P->app().portalSounds(out); });
  sendBuf(200, "application/json");
}

// The upload transaction (§15.4): /sounds/<name>.part in 4 KB writes under the lock, parsed and checked at the
// end, then renamed into place; an aborted or refused upload leaves nothing behind.
struct Upload {
  File     f;
  bool     active = false, failed = false;
  char     err[96] = { 0 };
  char     name[41] = { 0 };
  char     part[64] = { 0 }, dest[64] = { 0 };
  size_t   bytes = 0, wlen = 0;
  uint8_t* wbuf = nullptr;
  char     info[64] = { 0 };
};
static Upload U;

static void sanitiseName(const String& in, char* out, size_t n) {
  int slash = in.lastIndexOf('/'); String base = slash >= 0 ? in.substring(slash + 1) : in;
  size_t k = 0;
  for (size_t i = 0; i < base.length() && k < n - 1; i++) { char c = base[i]; if (isalnum((uint8_t)c) || c == '.' || c == '_' || c == '-') out[k++] = c; }
  out[k] = 0;
}
static void uploadFail(const char* why) {
  if (!U.failed) { U.failed = true; sb::copyStr(U.err, sizeof U.err, why); LOG_W(TAG, "upload %s: %s", U.name, why); }
  Storage::Guard g;
  if (U.f) U.f.close();
  if (U.part[0]) SD.remove(U.part);
}
static bool uploadFlush() {
  if (!U.wlen) return true;
  Storage::Guard g;
  size_t w = U.f.write(U.wbuf, U.wlen);
  bool ok = w == U.wlen;
  U.wlen = 0;
  return ok;
}
static size_t fileRead(void* ctx, uint32_t off, void* dst, size_t n) {
  File* f = static_cast<File*>(ctx);
  if (!f->seek(off)) return 0;
  return f->read(static_cast<uint8_t*>(dst), n);
}
static void hSoundUploadData() {
  HTTPUpload& up = S->upload();
  if (up.status == UPLOAD_FILE_START) {
    if (U.active) { LOG_W(TAG, "upload refused: one at a time"); return; }
    U = Upload();
    U.active = true;
    if (!U.wbuf) U.wbuf = static_cast<uint8_t*>(heap_caps_malloc(WCHUNK, MALLOC_CAP_SPIRAM));
    sanitiseName(up.filename, U.name, sizeof U.name);
    if (!P->app().store().cardMounted()) { uploadFail("no card"); return; }
    if (!sb::validSoundName(U.name) || !U.name[0]) { uploadFail("the name must be letters, digits, . _ - and end in .wav (40 characters at most)"); return; }
    if (!U.wbuf) { uploadFail("out of memory"); return; }
    snprintf(U.dest, sizeof U.dest, "/sounds/%s", U.name);
    snprintf(U.part, sizeof U.part, "/sounds/%s.part", U.name);
    Storage::Guard g;
    if (!SD.exists("/sounds")) SD.mkdir("/sounds");
    if (SD.exists(U.dest)) { U.part[0] = 0; uploadFail("a sound with that name exists: rename or delete it first"); return; }   // the mutex is recursive
    if (SD.exists(U.part)) SD.remove(U.part);
    U.f = SD.open(U.part, FILE_WRITE);
    if (!U.f) { U.part[0] = 0; uploadFail("could not create the file"); return; }
    LOG_I(TAG, "upload %s: started", U.name);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!U.active || U.failed) return;
    if (U.bytes + up.currentSize > SOUND_MAX) { uploadFail("over 8 MB"); return; }
    size_t off = 0;
    while (off < up.currentSize) {
      size_t n = up.currentSize - off; if (n > WCHUNK - U.wlen) n = WCHUNK - U.wlen;
      memcpy(U.wbuf + U.wlen, up.buf + off, n); U.wlen += n; off += n;
      if (U.wlen == WCHUNK && !uploadFlush()) { uploadFail("card write failed"); return; }
    }
    U.bytes += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (!U.active || U.failed) return;
    if (!uploadFlush()) { uploadFail("card write failed"); return; }
    { Storage::Guard g; U.f.close(); }
    // Validate (§6.3 parser), then commit.
    sb::wav::Info info; sb::wav::Err err;
    {
      Storage::Guard g;
      File f = SD.open(U.part, FILE_READ);
      if (!f) { uploadFail("could not read the file back"); return; }
      err = sb::wav::parse(&fileRead, &f, f.size(), info);
      f.close();
    }
    if (err != sb::wav::Err::None) { char why[80]; snprintf(why, sizeof why, "not a usable WAV: %s (16-bit PCM, 1-2 channels, 8-48 kHz)", sb::wav::errName(err)); uploadFail(why); return; }
    bool ok;
    { Storage::Guard g; ok = SD.rename(U.part, U.dest); }
    if (!ok) { uploadFail("could not rename into place"); return; }
    U.part[0] = 0;
    snprintf(U.info, sizeof U.info, "%.2f s, %u ch, %lu Hz, %lu KB", info.frames / (float)info.rate, (unsigned)info.channels, (unsigned long)info.rate, (unsigned long)(U.bytes / 1024));
    LOG_I(TAG, "upload %s: done, %s", U.name, U.info);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (U.active) uploadFail("upload stopped");
  }
}
static void hSoundUploadDone() {
  P->noteRequest(true);
  bool failed = U.failed; char err[96]; sb::copyStr(err, sizeof err, U.err);
  char name[41]; sb::copyStr(name, sizeof name, U.name);
  char info[64]; sb::copyStr(info, sizeof info, U.info);
  U.active = false;
  if (failed) { sendError(400, err[0] ? err : "upload failed"); return; }
  if (!P->running()) { sendError(503, "setup is stopping"); return; }
  P->onApp([&] { P->app().rescanSounds(); });
  BufPrint out(P->buf(), P->bufCap());
  out.print("{\"ok\":true,\"name\":\""); jsonEscape(out, name); out.print("\",\"info\":\""); jsonEscape(out, info); out.print("\"}");
  sendBuf(200, "application/json");
}
static void hSoundDelete() {
  P->noteRequest(true);
  if (busy()) return;
  char name[41]; sanitiseName(S->arg("name"), name, sizeof name);
  char err[120] = { 0 }; bool ok = false;
  P->onApp([&] { ok = P->app().portalSoundDelete(name, err, sizeof err); });
  if (!ok) { sendError(400, err[0] ? err : "delete failed"); return; }
  sendOk();
}
static void hSoundRename() {
  P->noteRequest(true);
  if (busy()) return;
  char name[41], to[41]; sanitiseName(S->arg("name"), name, sizeof name); sanitiseName(S->arg("to"), to, sizeof to);
  char err[120] = { 0 }; bool ok = false;
  P->onApp([&] { ok = P->app().portalSoundRename(name, to, err, sizeof err); });
  if (!ok) { sendError(400, err[0] ? err : "rename failed"); return; }
  sendOk();
}

// ---------------------------------------------------------------------------
// Play and actions
// ---------------------------------------------------------------------------
static void fillArgs(AppState::PortalArgs& a) {
  memset(&a, 0, sizeof a);
  sb::copyStr(a.name, sizeof a.name, S->arg("name").c_str());
  sb::copyStr(a.value, sizeof a.value, S->arg("value").c_str());
  sb::copyStr(a.path, sizeof a.path, S->arg("path").c_str());
  sb::copyStr(a.pattern, sizeof a.pattern, S->arg("pattern").c_str());
  a.level = S->hasArg("level") ? S->arg("level").toInt() : 0;
  a.button = S->hasArg("button") ? S->arg("button").toInt() : -1;
  a.jack = S->hasArg("jack") ? S->arg("jack").toInt() : 0;
  a.volts = S->hasArg("volts") ? (float)S->arg("volts").toFloat() : 0.0f;
}
static void hPlay() {
  P->noteRequest(true);
  if (busy()) return;
  AppState::PortalArgs a; fillArgs(a);
  // /api/play: `name` is the sound file; `cue` the cue; `level` + `button` an entry.
  sb::copyStr(a.value, sizeof a.value, S->arg("name").c_str());
  sb::copyStr(a.name, sizeof a.name, S->arg("cue").c_str());
  char err[100] = { 0 }; bool ok = false;
  P->onApp([&] { ok = P->app().portalPlay(a, err, sizeof err); });
  if (!ok) { sendError(400, err); return; }
  sendOk();
}
static void hAction() {
  P->noteRequest(true);
  if (busy()) return;
  AppState::PortalArgs a; fillArgs(a);
  if (!a.name[0]) { sendError(400, "which action?"); return; }
  char err[120] = { 0 }; bool ok = false;
  BufPrint extra(P->buf() + P->bufCap() / 2, 512);
  P->onApp([&] { ok = P->app().portalAction(a, extra, err, sizeof err); });
  if (!ok) { sendError(400, err); return; }
  BufPrint out(P->buf(), 1024);
  out.print("{\"ok\":true"); out.print(extra.c_str()); out.print("}");
  sendBuf(200, "application/json");
}

// ---------------------------------------------------------------------------
void bind(Portal& p) {
  P = &p; S = &p.server();
  S->on("/", HTTP_GET, hRoot);
  S->on("/index.html", HTTP_GET, hRoot);
  S->on("/api/schema", HTTP_GET, hSchema);
  S->on("/api/config", HTTP_GET, hConfigGet);
  S->on("/api/config", HTTP_PUT, hConfigPut);
  S->on("/api/config", HTTP_POST, hConfigPut);                  // the sign-in sheet's WebView may not PUT
  S->on("/api/config/download", HTTP_GET, hConfigDownload);
  S->on("/api/config/upload", HTTP_POST, hConfigUploadDone, hConfigUploadData);
  S->on("/api/status", HTTP_GET, hStatus);
  S->on("/api/diag", HTTP_GET, hDiag);
  S->on("/api/log", HTTP_GET, hLog);
  S->on("/api/coredump", HTTP_GET, hCoredump);
  S->on("/api/firmware/status", HTTP_GET, hFirmwareStatus);
  S->on("/api/sounds", HTTP_GET, hSounds);
  S->on("/api/sounds/upload", HTTP_POST, hSoundUploadDone, hSoundUploadData);
  S->on("/api/sounds/delete", HTTP_POST, hSoundDelete);
  S->on("/api/sounds/rename", HTTP_POST, hSoundRename);
  S->on("/api/play", HTTP_POST, hPlay);
  S->on("/api/action", HTTP_POST, hAction);
  // Captive-portal probes (validation 11): a redirect makes the OS open its sign-in sheet on the page.
  for (const char* path : { "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
                            "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt", "/fwlink", "/check_network_status.txt" })
    S->on(path, hRedirect);
  S->onNotFound(hNotFound);
}

}  // namespace Api
