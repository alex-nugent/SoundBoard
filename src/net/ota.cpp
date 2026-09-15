#include "net/ota.h"
#include "net/portal.h"
#include "app/state.h"
#include "hal/board.h"
#include "diag/log.h"
#include "util/strutil.h"
#include "util/timer.h"
#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClient.h>
#include <NetworkUdp.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform.h>
#include <lwip/dns.h>
#include <stdarg.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char* TAG = "ota";
static constexpr uint32_t JOIN_TIMEOUT_MS = 90000;   // a weak link misses beacons: the driver needs several scans
static constexpr size_t   CHUNK = 4096;                     // one bounded step (§15.4)
static constexpr uint32_t APP_SLOT = 0x640000;              // default_16MB.csv

struct Updater::Http { esp_http_client_handle_t h = nullptr; int status = 0; int64_t length = -1; };

// ---------------------------------------------------------------------------
static void staEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {           // the driver's own account of a join (§16 bench)
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_START: LOG_I(TAG, "sta: started"); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: LOG_I(TAG, "sta: associated, channel %u", (unsigned)info.wifi_sta_connected.channel); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: LOG_I(TAG, "sta: got IP %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str()); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: LOG_W(TAG, "sta: disconnected, reason %u", (unsigned)info.wifi_sta_disconnected.reason); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: LOG_I(TAG, "ap: a phone joined"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: LOG_I(TAG, "ap: a phone left"); break;
    default: break;
  }
}
// TLS needs ~45 KB for its buffers and contexts; the core allocates them from internal RAM, which Wi-Fi and BLE
// have already eaten (mbedtls_ssl_setup -0x7F00 at CP-11). PSRAM is fine for them (the IDF's "external memory"
// option does the same); the SHA and RNG contexts follow, which is harmless.
static void* psCalloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
}
void Updater::begin(AppState& app, Portal& portal) {
  app_ = &app; portal_ = &portal;
  WiFi.onEvent(staEvent);
  mbedtls_platform_set_calloc_free(psCalloc, free);
}

const char* Updater::jobName() const {
  switch (job_) {
    case Job::Idle: return "idle"; case Job::Connecting: return "connecting"; case Job::Checking: return "checking";
    case Job::Downloading: return "downloading"; case Job::Verifying: return "verifying"; case Job::Writing: return "writing";
    case Job::Rebooting: return "rebooting"; case Job::Failed: return "failed"; default: return "done";
  }
}
bool Updater::staConnected() const { return WiFi.status() == WL_CONNECTED; }

void Updater::fail(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); vsnprintf(err_, sizeof err_, fmt, ap); va_end(ap);
  job_ = Job::Failed; pct_ = 0;
  setText("failed: %s", err_);
  LOG_E(TAG, "%s", err_);
  if (http_) { Http h; h.h = (esp_http_client_handle_t)http_; httpClose(h); }
  if (Update.isRunning()) Update.abort();
  if (updating_) leaveUpdating(err_);
}
void Updater::setText(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(text_, sizeof text_, fmt, ap); va_end(ap); }

// The app-task hooks: direct when tick() runs on the app task, through the portal's bridge from the net task.
struct HookCtx { Updater* u; const char* why; void (*fn)(Updater*, const char*); };
void Updater::onApp(void (*fn)(Updater*, const char*), const char* arg) {
  if (!onNet_) { fn(this, arg); return; }
  HookCtx c = { this, arg, fn };
  portal_->callOnApp([](void* p) { HookCtx* c = static_cast<HookCtx*>(p); c->fn(c->u, c->why); }, &c);
}
void Updater::enterUpdating(const char* why) {
  if (updating_) return;
  updating_ = true;
  onApp([](Updater* u, const char* w) { u->app_->enterUpdating(w); }, why);
}
void Updater::leaveUpdating(const char* why) {
  if (!updating_) return;
  updating_ = false;
  onApp([](Updater* u, const char* w) { u->app_->leaveUpdating(w); }, why);
}

// ---------------------------------------------------------------------------
// Wi-Fi station (§16 "Network")
// ---------------------------------------------------------------------------
// A fresh association works where a used one stops answering (bench finding, cause unknown): drop and rejoin, then wait.
bool Updater::rejoin(uint32_t waitMs) {
  LOG_W(TAG, "rejoining the home Wi-Fi (RSSI %d)", (int)WiFi.RSSI());
  WiFi.disconnect(false, 200);
  delay(300);
  WiFi.reconnect();
  uint32_t t0 = millis();
  while (!staConnected() && millis() - t0 < waitMs) delay(100);
  if (staConnected()) LOG_I(TAG, "rejoined in %lu ms, RSSI %d", (unsigned long)(millis() - t0), (int)WiFi.RSSI());
  else LOG_W(TAG, "rejoin: not back after %lu ms", (unsigned long)waitMs);
  return staConnected();
}

bool Updater::join(const char* ssid, const char* password, char* err, size_t n) {
  const sb::Config& c = app_->config();
  if (!ssid || !*ssid) { ssid = c.wifi.ssid; password = c.wifi.password; }
  if (!*ssid) { snprintf(err, n, "no home Wi-Fi network is set"); return false; }
  if (busy() && job_ != Job::Connecting) { snprintf(err, n, "an update job is running"); return false; }
  WiFi.mode(portal_->running() ? WIFI_AP_STA : WIFI_STA);   // the AP stays for the phone; it follows the station's channel
  if (joinStartedAt_ && WiFi.status() != WL_CONNECTED) { WiFi.disconnect(false, 300); delay(100); }   // a stuck earlier attempt refuses a new config ("sta is connecting")
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                                        // modem sleep beside BLE and the AP misses beacons (reason 200 at CP-11)
  WiFi.begin(ssid, password && *password ? password : nullptr);
  esp_wifi_set_inactive_time(WIFI_IF_STA, 20);                 // beacon timeout 20 s instead of 6
  joinStartedAt_ = millis() ? millis() : 1;
  job_ = Job::Connecting; err_[0] = 0; pct_ = 0;
  setText("joining %s", ssid);
  LOG_I(TAG, "joining \"%s\"%s", ssid, portal_->running() ? " (AP+STA)" : "");
  return true;
}
bool Updater::ensureConnected(char* err, size_t n) {
  if (staConnected()) return true;
  if (job_ == Job::Connecting) return true;                  // already on its way; the job continues when it lands
  return join("", "", err, n);
}

// ---------------------------------------------------------------------------
// HTTPS (IDF client, the core's certificate bundle; redirects followed by hand: the manual open() flow does not)
// ---------------------------------------------------------------------------
// The TLS client's own resolver (getaddrinfo inside esp-tls) fails now and then with AP+STA up, while the Arduino
// resolver answers every time. So: resolve here, connect by address, and hand the real name to TLS (SNI + certificate
// check) and to the Host header. Redirects (github.com -> objects.githubusercontent.com) are followed the same way.
static char s_location[1024];                                   // GitHub's signed asset URLs run past 500 characters
static esp_err_t httpEvent(esp_http_client_event_t* ev) {
  if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && !strcasecmp(ev->header_key, "Location")) sb::copyStr(s_location, sizeof s_location, ev->header_value ? ev->header_value : "");
  return ESP_OK;
}
// LwIP keeps one global DNS server list. The router's IPv6 advertisements (RDNSS) can push its IPv6 resolvers into
// slots 0 and 1 ahead of the IPv4 one DHCP gave us, and queries to those time out. Pin the list before every lookup:
// slot 0 the station's DHCP resolver, slot 1 a public one in case the router's is the problem.
static void pinDns() {
  char before[64] = ""; size_t o = 0;
  for (int i = 0; i < DNS_MAX_SERVERS && o < sizeof before - 20; i++) { const ip_addr_t* a = dns_getserver(i); o += snprintf(before + o, sizeof before - o, "%s%s", i ? " " : "", ipaddr_ntoa(a)); }
  ip_addr_t d0, d1;
  IPAddress sta = WiFi.dnsIP();
  ipaddr_aton(sta ? sta.toString().c_str() : "8.8.8.8", &d0);
  ipaddr_aton("8.8.8.8", &d1);
  dns_setserver(0, &d0); dns_setserver(1, &d1);
  LOG_I(TAG, "dns: servers were [%s], now [%s 8.8.8.8]", before, ipaddr_ntoa(&d0));
}
// WiFi.hostByName() returns 1 on success and the LwIP error code (also non-zero) on failure: compare with 1.
static bool resolve(const char* host, IPAddress& ip) {
  for (int attempt = 0; attempt < 3; attempt++) {
    ip = IPAddress();
    int r = WiFi.hostByName(host, ip);
    if (r == 1 && ip && ip != IPAddress(255, 255, 255, 255)) return true;
    LOG_W(TAG, "dns: %s lookup %d failed (%d) | RSSI %d", host, attempt + 1, r, (int)WiFi.RSSI());
    delay(500);
  }
  return false;
}
static bool splitUrl(const char* url, char* host, size_t hn, char* path, size_t pn) {  // https://host/path
  if (strncmp(url, "https://", 8)) return false;
  const char* h = url + 8; const char* slash = strchr(h, '/');
  size_t hl = slash ? (size_t)(slash - h) : strlen(h);
  if (!hl || hl >= hn) return false;
  memcpy(host, h, hl); host[hl] = 0;
  sb::copyStr(path, pn, slash ? slash : "/");
  return true;
}

bool Updater::httpOpen(Http& h, const char* url, char* err, size_t n, int64_t from) {
  // AP+STA: LwIP's default route can land on the soft AP (the phone's side); internet traffic must leave by the station.
  bool wasDefault = WiFi.STA.isDefault();
  if (!wasDefault) WiFi.STA.setDefault();
  LOG_I(TAG, "http: %s (station %s the default interface%s) | heap free %lu, largest block %lu", url, wasDefault ? "was" : "was NOT", wasDefault ? "" : ", made it so",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  static char cur[1024], path[960], ipUrl[1024];              // one fetch at a time; off the task stack
  char host[96];
  sb::copyStr(cur, sizeof cur, url);
  for (int hop = 0; hop < 6; hop++) {
    if (!splitUrl(cur, host, sizeof host, path, sizeof path)) { snprintf(err, n, "bad URL: %s", cur); return false; }
    IPAddress ip;
    if (!resolve(host, ip)) { snprintf(err, n, "DNS failed for %s (no internet on that Wi-Fi?)", host); return false; }
    snprintf(ipUrl, sizeof ipUrl, "https://%s%s", ip.toString().c_str(), path);
    esp_http_client_config_t cfg = {};
    cfg.url = ipUrl;
    cfg.common_name = host;                                    // SNI and the certificate name check use the real host
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = httpEvent;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "SoundBoard/" FW_VERSION;
    h.h = esp_http_client_init(&cfg);
    if (!h.h) { snprintf(err, n, "http init failed"); return false; }
    esp_http_client_set_header(h.h, "Host", host);             // not the address
    char range[48];
    if (from > 0) { snprintf(range, sizeof range, "bytes=%lld-", (long long)from); esp_http_client_set_header(h.h, "Range", range); }
    s_location[0] = 0;
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && e != ESP_OK; attempt++) {     // a weak link loses SYNs: three tries of 20 s
      e = esp_http_client_open(h.h, 0);
      if (e != ESP_OK) LOG_W(TAG, "http: connect attempt %d to %s (%s) failed (%s) | RSSI %d", attempt + 1, host, ip.toString().c_str(), esp_err_to_name(e), (int)WiFi.RSSI());
    }
    if (e != ESP_OK) { snprintf(err, n, "connect failed (%s); no internet on that Wi-Fi, or a weak signal (RSSI %d)?", esp_err_to_name(e), (int)WiFi.RSSI()); httpClose(h); return false; }
    h.length = esp_http_client_fetch_headers(h.h);
    h.status = esp_http_client_get_status_code(h.h);
    if (h.status == 301 || h.status == 302 || h.status == 303 || h.status == 307 || h.status == 308) {
      if (!s_location[0]) { snprintf(err, n, "HTTP %d without a Location", h.status); httpClose(h); return false; }
      LOG_I(TAG, "http: %d -> %s (%u chars)", h.status, s_location, (unsigned)strlen(s_location));
      httpClose(h);
      sb::copyStr(cur, sizeof cur, s_location);
      continue;
    }
    if (from > 0 && h.status == 200) { snprintf(err, n, "the server would not resume the download"); httpClose(h); return false; }
    if (h.status != 200 && h.status != 206) { snprintf(err, n, "HTTP %d from GitHub", h.status); httpClose(h); return false; }
    return true;
  }
  snprintf(err, n, "too many redirects"); httpClose(h); return false;
}
int Updater::httpRead(Http& h, uint8_t* buf, size_t n) { return esp_http_client_read(h.h, (char*)buf, (int)n); }
void Updater::httpClose(Http& h) { if (h.h) { if (http_ == (void*)h.h) http_ = nullptr; esp_http_client_close(h.h); esp_http_client_cleanup(h.h); } h.h = nullptr; }

void Updater::manifestUrl(const char* version, char* out, size_t n) {
  const sb::Config& c = app_->config();
  const char* v = version && *version ? version : c.update.channel;
  if (!v || !*v || !strcmp(v, "latest")) snprintf(out, n, "https://github.com/%s/releases/latest/download/manifest.json", c.update.repo);
  else snprintf(out, n, "https://github.com/%s/releases/download/%s/manifest.json", c.update.repo, v);
}
void Updater::binUrl(char* out, size_t n) { snprintf(out, n, "https://github.com/%s/releases/download/%s/%s", app_->config().update.repo, man_.version, man_.file); }

bool Updater::fetchManifest(const char* version, char* err, size_t n) {
  char url[160]; manifestUrl(version, url, sizeof url);
  pinDns();
  { IPAddress ip; bool dns = resolve("github.com", ip);   // DNS apart from TLS in the log
    LOG_I(TAG, "manifest: %s | github.com -> %s | station %s gateway %s dns %s | heap free %lu", url, dns ? ip.toString().c_str() : "DNS FAILED",
          WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(), (unsigned long)ESP.getFreeHeap());
    if (!dns) { snprintf(err, n, "DNS failed on that Wi-Fi (no internet, or a captive network)"); return false; } }
  Http h;
  if (!httpOpen(h, url, err, n)) return false;
  char* body = static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
  if (!body) { httpClose(h); snprintf(err, n, "out of memory"); return false; }
  int len = 0;
  while (len < 4095) { int r = httpRead(h, (uint8_t*)body + len, 4095 - len); if (r <= 0) break; len += r; }
  body[len] = 0;
  httpClose(h);
  JsonDocument d(psramAllocator());
  DeserializationError e = deserializeJson(d, body, len);
  heap_caps_free(body);
  if (e || d["version"].isNull() || d["file"].isNull() || d["sha256"].isNull()) { snprintf(err, n, "the manifest is not readable"); return false; }
  Manifest m;
  sb::copyStr(m.version, sizeof m.version, d["version"] | "");
  sb::copyStr(m.file, sizeof m.file, d["file"] | "");
  sb::copyStr(m.sha256, sizeof m.sha256, d["sha256"] | "");
  sb::copyStr(m.notes, sizeof m.notes, d["notes"] | "");
  m.size = d["size"] | 0UL; m.schema = d["schema"] | 1; m.minSchema = d["minSchema"] | 1;
  if (!m.size || m.size > APP_SLOT) { snprintf(err, n, "the manifest's image size %lu does not fit the app slot", (unsigned long)m.size); return false; }
  m.valid = true; man_ = m;
  LOG_I(TAG, "manifest: %s, %s, %lu bytes, schema %u (min %u)", m.version, m.file, (unsigned long)m.size, m.schema, m.minSchema);
  return true;
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------
bool Updater::check(const char* version, char* err, size_t n) {
  if (busy() && job_ != Job::Connecting) { snprintf(err, n, "a job is running (%s)", jobName()); return false; }
  sb::copyStr(wantVersion_, sizeof wantVersion_, version ? version : "");
  installAfterCheck_ = false; checkPending_ = true; err_[0] = 0; pct_ = 0;
  if (!ensureConnected(err, n)) return false;
  if (job_ != Job::Connecting) { job_ = Job::Checking; setText("checking for updates"); }
  return true;
}

bool Updater::install(const char* version, char* err, size_t n) {
  if (busy() && job_ != Job::Connecting) { snprintf(err, n, "a job is running (%s)", jobName()); return false; }
  if (!Board::usbPresent()) { snprintf(err, n, "plug in USB power first"); return false; }
  sb::copyStr(wantVersion_, sizeof wantVersion_, version ? version : "");
  installAfterCheck_ = true; checkPending_ = true; err_[0] = 0; pct_ = 0;
  if (!ensureConnected(err, n)) return false;
  if (job_ != Job::Connecting) { job_ = Job::Checking; setText("checking %s", wantVersion_[0] ? wantVersion_ : "the latest release"); }
  return true;
}

bool Updater::startDownload(char* err, size_t n) {
  const sb::Config& c = app_->config();
  if (!man_.valid) { snprintf(err, n, "no release known: check first"); return false; }
  if (!Board::usbPresent()) { snprintf(err, n, "plug in USB power first"); return false; }
  if (c.schema > man_.schema) { snprintf(err, n, "the card's configuration (schema %u) is newer than %s writes (%u)", c.schema, man_.version, man_.schema); return false; }
  if (c.schema < man_.minSchema) { snprintf(err, n, "%s needs a configuration of schema %u or later (the card has %u)", man_.version, man_.minSchema, c.schema); return false; }
  if (!strcmp(man_.version, FW_VERSION)) LOG_W(TAG, "installing %s over the same version", man_.version);
  binUrl(binUrl_, sizeof binUrl_);
  const char* url = binUrl_;
  enterUpdating(man_.version);                                  // §16: quiesce, rail down, pads ignored, progress screen
  Preferences p; if (p.begin("sb-state", false)) { p.putUChar("resumeSetup", 1); p.end(); }   // audio is down: a flash write is allowed
  Http h;
  if (!httpOpen(h, url, err, n)) { leaveUpdating(err); return false; }
  if (h.length > 0 && (uint32_t)h.length != man_.size) { httpClose(h); snprintf(err, n, "the download is %lld bytes, the manifest says %lu", (long long)h.length, (unsigned long)man_.size); leaveUpdating(err); return false; }
  if (!Update.begin(man_.size, U_FLASH)) { httpClose(h); snprintf(err, n, "no room: %s", Update.errorString()); leaveUpdating(err); return false; }
  if (!buf_) buf_ = static_cast<uint8_t*>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM));
  if (!sha_) sha_ = calloc(1, sizeof(mbedtls_sha256_context));
  if (!buf_ || !sha_) { httpClose(h); Update.abort(); snprintf(err, n, "out of memory"); leaveUpdating(err); return false; }
  mbedtls_sha256_init((mbedtls_sha256_context*)sha_); mbedtls_sha256_starts((mbedtls_sha256_context*)sha_, 0);
  http_ = h.h; total_ = man_.size; got_ = 0; pct_ = 0; resumes_ = 0;
  job_ = Job::Downloading; setText("downloading %s", man_.version);
  LOG_I(TAG, "download %s: %lu bytes from %s | heap free %lu", man_.version, (unsigned long)total_, url, (unsigned long)ESP.getFreeHeap());
  lastLogAt_ = millis();
  return true;
}

void Updater::stepDownload() {                                 // ≤ 4 KB per pass: the page's /api/status keeps answering between passes
  if (!http_) { resumeDownload("still down"); return; }         // a reconnect that did not open: try again
  Http h; h.h = (esp_http_client_handle_t)http_;
  int r = httpRead(h, buf_, CHUNK);
  if (r < 0 || (r == 0 && got_ < total_)) { resumeDownload(r < 0 ? "connection lost" : "ended early"); return; }
  if (r == 0) { finishDownload(); return; }
  if (!Board::usbPresent()) { fail("USB power was removed"); return; }
  mbedtls_sha256_update((mbedtls_sha256_context*)sha_, buf_, r);
  if (Update.write(buf_, r) != (size_t)r) { fail("flash write failed: %s", Update.errorString()); return; }
  got_ += r;
  pct_ = (uint8_t)(got_ * 100 / (total_ ? total_ : 1));
  setText("downloading %s %u %%", man_.version, pct_);
  if (millis() - lastLogAt_ > 5000) { lastLogAt_ = millis(); LOG_I(TAG, "download %u %% (%lu bytes) | heap free %lu", pct_, (unsigned long)got_, (unsigned long)ESP.getFreeHeap()); }
  if (got_ >= total_) finishDownload();
}

// A weak link drops the connection now and then: pick the download up where it stopped (HTTP Range), the flash
// write and the running SHA-256 carry on. Bounded, so a dead link ends the job instead of holding the board.
void Updater::resumeDownload(const char* why) {
  { Http h; h.h = (esp_http_client_handle_t)http_; httpClose(h); http_ = nullptr; }
  if (++resumes_ > MAX_RESUMES) { fail("download %s after %lu bytes, gave up after %d reconnects", why, (unsigned long)got_, MAX_RESUMES); return; }
  LOG_W(TAG, "download %s at %lu of %lu bytes (RSSI %d): reconnect %d of %d", why, (unsigned long)got_, (unsigned long)total_, (int)WiFi.RSSI(), resumes_, MAX_RESUMES);
  setText("reconnecting (%d) at %u %%", resumes_, pct_);
  if (!rejoin(30000)) { fail("download %s at %lu bytes and the Wi-Fi did not come back", why, (unsigned long)got_); return; }
  if (!Board::usbPresent()) { fail("USB power was removed"); return; }
  char err[120]; Http h;
  if (!httpOpen(h, binUrl_, err, sizeof err, got_)) { LOG_W(TAG, "resume: %s", err); delay(1000); if (resumes_ >= MAX_RESUMES) fail("%s", err); return; }   // next pass tries again
  if (h.length > 0 && h.length != total_ - got_) { httpClose(h); fail("resume: the server offered %lld bytes, %lld were left", (long long)h.length, (long long)(total_ - got_)); return; }
  http_ = h.h;
  setText("downloading %s %u %%", man_.version, pct_);
}

void Updater::finishDownload() {
  Http h; h.h = (esp_http_client_handle_t)http_; httpClose(h); http_ = nullptr;
  job_ = Job::Verifying; setText("verifying %s", man_.version);
  unsigned char dig[32]; mbedtls_sha256_finish((mbedtls_sha256_context*)sha_, dig); mbedtls_sha256_free((mbedtls_sha256_context*)sha_);
  char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", dig[i]);
  if (strcasecmp(hex, man_.sha256)) { fail("SHA-256 mismatch: the download is not the release's image"); return; }
  job_ = Job::Writing; setText("writing %s", man_.version);
  if (!Update.end(true)) { fail("image rejected: %s", Update.errorString()); return; }   // the boot record changes here
  sb::copyStr(stagedVersion_, sizeof stagedVersion_, man_.version);
  job_ = Job::Rebooting; pct_ = 100; setText("restarting with %s", man_.version);
  rebootAt_ = millis() + 1500; if (!rebootAt_) rebootAt_ = 1;
  LOG_I(TAG, "%s written and verified: restarting", man_.version);
}

bool Updater::rollback(char* err, size_t n) {
  if (busy()) { snprintf(err, n, "a job is running (%s)", jobName()); return false; }
  if (!rollbackEligible()) { snprintf(err, n, "the other slot has no valid image to go back to"); return false; }
  char v[32], st[24]; otherSlot(v, sizeof v, st, sizeof st);
  enterUpdating(v);
  Preferences p; if (p.begin("sb-state", false)) { p.putUChar("resumeSetup", 1); p.end(); }
  if (!Update.rollBack()) { snprintf(err, n, "rollback refused"); leaveUpdating(err); return false; }
  job_ = Job::Rebooting; pct_ = 100; setText("restarting with %s", v);
  rebootAt_ = millis() + 1500; if (!rebootAt_) rebootAt_ = 1;
  LOG_W(TAG, "going back to %s (the other slot): restarting", v);
  return true;
}

bool Updater::cancel(char* err, size_t n) {
  if (job_ == Job::Rebooting || job_ == Job::Writing) { snprintf(err, n, "too late: the image is written"); return false; }
  if (!busy()) { snprintf(err, n, "nothing to cancel"); return false; }
  if (http_) { Http h; h.h = (esp_http_client_handle_t)http_; httpClose(h); http_ = nullptr; }
  if (Update.isRunning()) Update.abort();
  if (job_ == Job::Connecting) WiFi.disconnect(false);
  job_ = Job::Idle; pct_ = 0; checkPending_ = installAfterCheck_ = false; setText("cancelled");
  Preferences p; if (p.begin("sb-state", false)) { p.remove("resumeSetup"); p.end(); }
  leaveUpdating("cancelled from the page");
  LOG_I(TAG, "cancelled");
  return true;
}

// A `.bin` from the page (§16 ".bin upload"): the same Update path; the embedded version is read back afterwards.
bool Updater::uploadStart(size_t declaredSize, char* err, size_t n) {
  if (busy()) { snprintf(err, n, "a job is running (%s)", jobName()); return false; }
  if (!Board::usbPresent()) { snprintf(err, n, "plug in USB power first"); return false; }
  if (declaredSize > APP_SLOT) { snprintf(err, n, "the file is bigger than the app slot"); return false; }
  enterUpdating("a file from the page");
  Preferences p; if (p.begin("sb-state", false)) { p.putUChar("resumeSetup", 1); p.end(); }
  if (!Update.begin(declaredSize ? declaredSize : UPDATE_SIZE_UNKNOWN, U_FLASH)) { snprintf(err, n, "no room: %s", Update.errorString()); leaveUpdating(err); return false; }
  upload_ = true; upBytes_ = 0; job_ = Job::Writing; pct_ = 0; err_[0] = 0; total_ = declaredSize;
  setText("receiving a file");
  LOG_I(TAG, "upload: started (%lu bytes declared)", (unsigned long)declaredSize);
  return true;
}
bool Updater::uploadWrite(const uint8_t* data, size_t len) {
  if (!upload_) return false;
  if (!Board::usbPresent()) { fail("USB power was removed"); upload_ = false; return false; }
  if (Update.write((uint8_t*)data, len) != len) { fail("flash write failed: %s", Update.errorString()); upload_ = false; return false; }
  upBytes_ += len;
  if (total_) { pct_ = (uint8_t)(upBytes_ * 100 / total_); setText("receiving a file %u %%", pct_); }
  return true;
}
bool Updater::uploadEnd(char* err, size_t n) {
  if (!upload_) { snprintf(err, n, "%s", err_[0] ? err_ : "no upload"); return false; }
  upload_ = false;
  if (!Update.end(true)) { fail("image rejected: %s", Update.errorString()); snprintf(err, n, "%s", err_); return false; }
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);   // after end() the boot record points at it; describe what was written
  esp_app_desc_t desc;
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  sb::copyStr(stagedVersion_, sizeof stagedVersion_, (boot && esp_ota_get_partition_description(boot, &desc) == ESP_OK) ? desc.version : "the uploaded file");
  (void)next;
  job_ = Job::Rebooting; pct_ = 100; setText("restarting with %s", stagedVersion_);
  rebootAt_ = millis() + 1500; if (!rebootAt_) rebootAt_ = 1;
  LOG_I(TAG, "upload: %lu bytes written (%s): restarting", (unsigned long)upBytes_, stagedVersion_);
  return true;
}
void Updater::uploadAbort(const char* why) { if (upload_) { upload_ = false; fail("upload stopped: %s", why); } }

// ---------------------------------------------------------------------------
// SETUP off (console bench, or a job outliving the portal): the blocking steps (connects of up to 20 s) cannot run on
// the app task (task watchdog), so they get a task of their own that lives while the job does.
void Updater::taskThunk(void* p) {
  Updater* u = static_cast<Updater*>(p);
  while (u->busy() || u->rebootAt_) { u->tick(true); vTaskDelay(pdMS_TO_TICKS(5)); }
  u->task_ = nullptr;
  vTaskDelete(nullptr);
}
void Updater::tickApp() {
  if (task_ || (!busy() && !rebootAt_)) return;
  if (xTaskCreatePinnedToCore(&Updater::taskThunk, "ota", 12 * 1024, this, 1, (TaskHandle_t*)&task_, 0) != pdPASS) { task_ = nullptr; fail("no task for the update"); }
}

void Updater::tick(bool onNetTask) {
  onNet_ = onNetTask;
  uint32_t now = millis();
  if (rebootAt_ && due(now, rebootAt_)) { rebootAt_ = 0; Log::tick(); Serial.flush(); delay(30); ESP.restart(); }
  switch (job_) {
    case Job::Connecting: {
      if (staConnected()) {
        LOG_I(TAG, "joined \"%s\": %s, RSSI %d | heap free %lu", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (unsigned long)ESP.getFreeHeap());
        if (checkPending_) { job_ = Job::Checking; setText("checking for updates"); }
        else { job_ = Job::Done; setText("joined %s", WiFi.SSID().c_str()); }
      } else if (due(now, joinStartedAt_ + JOIN_TIMEOUT_MS)) {
        wl_status_t st = WiFi.status();
        fail("could not join the home Wi-Fi: %s", st == WL_NO_SSID_AVAIL ? "network not found (2.4 GHz only; hidden networks need the exact name)" :
             st == WL_CONNECT_FAILED ? "wrong password" : st == WL_CONNECTION_LOST ? "connection lost" : st == WL_DISCONNECTED ? "disconnected (wrong password?)" : "no answer");
        WiFi.disconnect(false);
      }
      break;
    }
    case Job::Checking: {
      char err[120];
      checkPending_ = false;
      bool ok = false;
      for (int attempt = 0; attempt < 3 && !ok; attempt++) {           // the whole fetch again: a weak link loses a hop now and then
        ok = fetchManifest(wantVersion_, err, sizeof err);
        if (!ok) { LOG_W(TAG, "check %d of 3: %s", attempt + 1, err); if (attempt < 2) rejoin(30000); }
      }
      if (!ok) { fail("%s", err); break; }
      if (!installAfterCheck_) { job_ = Job::Done; setText("%s is available", man_.version); break; }
      installAfterCheck_ = false;
      if (!startDownload(err, sizeof err)) fail("%s", err);
      break;
    }
    case Job::Downloading: stepDownload(); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Slots and status
// ---------------------------------------------------------------------------
static const char* otaStateName(esp_ota_img_states_t st) {
  switch (st) { case ESP_OTA_IMG_NEW: return "new"; case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify"; case ESP_OTA_IMG_VALID: return "valid";
                case ESP_OTA_IMG_INVALID: return "invalid"; case ESP_OTA_IMG_ABORTED: return "aborted"; case ESP_OTA_IMG_UNDEFINED: return "flashed over USB"; default: return "?"; }
}
bool Updater::otherSlot(char* version, size_t n, char* state, size_t sn) {
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) return false;
  esp_ota_img_states_t st; if (esp_ota_get_state_partition(other, &st) != ESP_OK) st = ESP_OTA_IMG_UNDEFINED;
  sb::copyStr(state, sn, otaStateName(st));
  esp_app_desc_t d;
  if (esp_ota_get_partition_description(other, &d) != ESP_OK) { sb::copyStr(version, n, ""); return false; }
  sb::copyStr(version, n, d.version);
  return true;
}
bool Updater::rollbackEligible() {                             // §16: the other slot valid and describable; a USB-flashed image counts as valid
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) return false;
  esp_ota_img_states_t st; if (esp_ota_get_state_partition(other, &st) != ESP_OK) return false;
  if (st != ESP_OTA_IMG_VALID && st != ESP_OTA_IMG_UNDEFINED) return false;
  esp_app_desc_t d; return esp_ota_get_partition_description(other, &d) == ESP_OK;
}

void Updater::statusJson(Print& out) {
  JsonDocument d(psramAllocator());
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st; if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) st = ESP_OTA_IMG_UNDEFINED;
  d["version"] = FW_VERSION; d["slot"] = run ? run->label : "?"; d["state"] = otaStateName(st);
  char v[32], s[24]; bool other = otherSlot(v, sizeof v, s, sizeof s);
  JsonObject o = d["other"].to<JsonObject>(); o["present"] = other; o["version"] = v; o["state"] = s; o["eligible"] = rollbackEligible();
  d["usb"] = Board::usbPresent();
  d["job"] = jobName(); d["pct"] = pct_; d["error"] = err_; d["text"] = text_;
  JsonObject w = d["sta"].to<JsonObject>();
  w["connected"] = staConnected(); w["ssid"] = staConnected() ? WiFi.SSID() : String(app_->config().wifi.ssid); w["ip"] = staConnected() ? WiFi.localIP().toString() : String(""); w["rssi"] = staConnected() ? WiFi.RSSI() : 0;
  w["configured"] = app_->config().wifi.ssid[0] != 0;
  if (man_.valid) { JsonObject m = d["available"].to<JsonObject>(); m["version"] = man_.version; m["size"] = man_.size; m["notes"] = man_.notes; m["schema"] = man_.schema; m["minSchema"] = man_.minSchema; m["same"] = !strcmp(man_.version, FW_VERSION); }
  d["repo"] = app_->config().update.repo; d["channel"] = app_->config().update.channel; d["schema"] = app_->config().schema;
  d["heap"] = ESP.getFreeHeap();
  serializeJson(d, out);
}

void Updater::probe(Print& out) {
  if (!staConnected()) { out.println("probe: the station is not joined"); return; }
  out.printf("probe: RSSI %d, channel %d, gateway %s, default interface %s\n", (int)WiFi.RSSI(), WiFi.channel(), WiFi.gatewayIP().toString().c_str(), WiFi.STA.isDefault() ? "station" : "NOT the station");
  for (int round = 0; round < 3; round++) {
    NetworkClient c; uint32_t t0 = millis(); bool ok = c.connect(WiFi.gatewayIP(), 80, 2000); uint32_t dt = millis() - t0; c.stop();
    esp_task_wdt_reset();                                    // the console runs this on the app task: keep the watchdog fed between steps
    IPAddress gh(140, 82, 112, 3); bool dns = true;               // a known GitHub front door; a lookup here could stall past the watchdog
    NetworkClient g; uint32_t t1 = millis(); bool ok2 = dns && g.connect(gh, 443, 2000); uint32_t dt2 = millis() - t1; g.stop();
    esp_task_wdt_reset();
    NetworkClient w; uint32_t t2 = millis(); bool ok3 = w.connect(IPAddress(1, 1, 1, 1), 443, 2000); uint32_t dt3 = millis() - t2; w.stop();
    // WAN UDP: a bare DNS query for example.com straight to 8.8.8.8
    static const uint8_t q[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
    NetworkUDP u; bool ok4 = false; uint32_t t3 = millis(), dt4 = 0;
    if (u.begin(0) && u.beginPacket(IPAddress(8, 8, 8, 8), 53) && u.write(q, sizeof q) == sizeof q && u.endPacket()) {
      while (millis() - t3 < 2000 && !ok4) { if (u.parsePacket() > 0) ok4 = true; else delay(10); }
      dt4 = millis() - t3;
    }
    u.stop();
    out.printf("  router:80 %s %lu ms | github.com:443 (%s) %s %lu ms | 1.1.1.1:443 %s %lu ms | udp 8.8.8.8:53 %s %lu ms\n",
               ok ? "ok" : "FAILED", (unsigned long)dt, dns ? gh.toString().c_str() : "no DNS", ok2 ? "ok" : "FAILED", (unsigned long)dt2,
               ok3 ? "ok" : "FAILED", (unsigned long)dt3, ok4 ? "ok" : "FAILED", (unsigned long)dt4);
  }
}

void Updater::printStatus(Print& out) {
  char v[32], s[24]; bool other = otherSlot(v, sizeof v, s, sizeof s);
  out.printf("update: heap free %lu B (largest block %lu B) | job %s%s%s | %u %% | other slot %s%s%s (%s) | station %s%s | repo %s (%s) | last release seen %s\n",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             jobName(), text_[0] ? " " : "", text_, (unsigned)pct_, other ? v : "none", other ? " " : "", other ? s : "", rollbackEligible() ? "eligible" : "not eligible",
             staConnected() ? "joined " : "off", staConnected() ? WiFi.SSID().c_str() : "", app_->config().update.repo, app_->config().update.channel, man_.valid ? man_.version : "none");
}
