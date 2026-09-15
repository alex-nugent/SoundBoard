// Firmware updates (FirmwareSpec.md §16, §15.4 /api/firmware/*): the home
// Wi-Fi join (AP+STA while SETUP runs), the release manifest from GitHub over
// HTTPS with the core's certificate bundle, the download streamed into the
// spare OTA slot in bounded steps, a `.bin` from the page through the same
// path, rollback to the other slot, and the job state the page and the screen
// poll. Runs on whichever task calls tick(): the `net` task while SETUP is on
// (§19.1), the app task for the console's bench commands. Anything that
// touches module state goes through the app hooks (AppState::enterUpdating /
// leaveUpdating), on the app task.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Print.h>

class AppState;
class Portal;

class Updater {
 public:
  enum class Job : uint8_t { Idle, Connecting, Checking, Downloading, Verifying, Writing, Rebooting, Failed, Done };
  struct Manifest { bool valid = false; char version[32] = ""; char file[64] = ""; uint32_t size = 0; char sha256[65] = ""; uint8_t schema = 1, minSchema = 1; char notes[240] = ""; };

  void begin(AppState& app, Portal& portal);
  void tick(bool onNetTask);                        // one bounded step (≤ 4 KB or ≤ 50 ms, §15.4), plus the join / reboot timers
  void tickApp();                                   // app task while SETUP is off: gives a running job its own task
  bool ownTask() const { return task_ != nullptr; }

  // Requests (any task that also runs tick(); the API and the console).
  bool join(const char* ssid, const char* password, char* err, size_t n);   // "" = wifi.* from the configuration
  bool check(const char* version, char* err, size_t n);                     // "" = update.channel
  bool install(const char* version, char* err, size_t n);                   // USB gate, schema guard; checks first when needed
  bool rollback(char* err, size_t n);
  bool cancel(char* err, size_t n);
  // A `.bin` from the page (the upload callback, net task): start/write/end/abort.
  bool uploadStart(size_t declaredSize, char* err, size_t n);
  bool uploadWrite(const uint8_t* data, size_t len);
  bool uploadEnd(char* err, size_t n);
  void uploadAbort(const char* why);

  // State for the page (§15.4 /api/firmware/status), the screen and `s`.
  Job         job() const { return job_; }
  const char* jobName() const;
  uint8_t     pct() const { return pct_; }
  const char* error() const { return err_; }
  const char* text() const { return text_; }       // one line for the screen: "downloading v0.11.0 45 %"
  const Manifest& manifest() const { return man_; }
  bool        busy() const { return job_ != Job::Idle && job_ != Job::Failed && job_ != Job::Done; }
  bool        installing() const { return job_ == Job::Downloading || job_ == Job::Verifying || job_ == Job::Writing || job_ == Job::Rebooting; }
  bool        staConnected() const;
  void        statusJson(Print& out);              // the whole /api/firmware/status document
  void        printStatus(Print& out);
  void        probe(Print& out);                   // bench: time raw TCP connects to the router and to GitHub (link quality)
  static bool otherSlot(char* version, size_t n, char* state, size_t sn);   // false when there is no other image
  static bool rollbackEligible();

 private:
  struct Http;
  bool httpOpen(Http& h, const char* url, char* err, size_t n, int64_t from = 0);
  void resumeDownload(const char* why);
  bool rejoin(uint32_t waitMs);
  static constexpr int MAX_RESUMES = 20;
  int  httpRead(Http& h, uint8_t* buf, size_t n);
  void httpClose(Http& h);
  bool fetchManifest(const char* version, char* err, size_t n);
  void manifestUrl(const char* version, char* out, size_t n);
  void binUrl(char* out, size_t n);
  bool startDownload(char* err, size_t n);
  void stepDownload();
  void finishDownload();
  void fail(const char* fmt, ...);
  void setText(const char* fmt, ...);
  bool ensureConnected(char* err, size_t n);
  void enterUpdating(const char* why);
  void leaveUpdating(const char* why);
  void onApp(void (*fn)(Updater*, const char*), const char* arg);

  AppState* app_ = nullptr;
  Portal*   portal_ = nullptr;
  bool      onNet_ = false;
  volatile void* task_ = nullptr;                   // the job's own task while SETUP is off
  static void taskThunk(void*);
  Job       job_ = Job::Idle;
  uint8_t   pct_ = 0;
  char      err_[120] = "";
  char      text_[48] = "";
  Manifest  man_;
  char      wantVersion_[32] = "";
  bool      installAfterCheck_ = false, checkPending_ = false, updating_ = false;
  uint32_t  joinStartedAt_ = 0, rebootAt_ = 0, lastLogAt_ = 0;
  // download
  void*     http_ = nullptr;                        // esp_http_client_handle_t
  int64_t   total_ = 0, got_ = 0;
  int       resumes_ = 0;
  char      binUrl_[200] = "";
  uint8_t*  buf_ = nullptr;
  void*     sha_ = nullptr;                         // mbedtls_sha256_context*
  bool      upload_ = false;
  size_t    upBytes_ = 0;
  char      stagedVersion_[32] = "";
};
