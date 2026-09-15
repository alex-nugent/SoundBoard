// The Wi-Fi settings portal (FirmwareSpec.md §15.2, §19.1 `net` task): the
// soft AP `SoundBoard-xxxx` on 192.168.4.1, a catch-all DNS, mDNS
// `soundboard.local`, the synchronous WebServer bound to the AP address and
// run from its own task on core 0, and the bridge that lets a request handler
// run a piece of work on the app task and wait for it (every module is
// touched from one task only, §19.2). The endpoints are in api.cpp; the app
// side of SETUP (entry, exit, the card, the timers) is app/setup.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Print.h>
#include <IPAddress.h>

class AppState;
class WebServer;
class DNSServer;

// A bounded text buffer with a Print face: JSON and log tails are built into
// the portal's PSRAM buffer on the app task and sent from the net task.
class BufPrint : public Print {
 public:
  BufPrint(char* buf, size_t cap) : b_(buf), cap_(cap) { clear(); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* p, size_t n) override;
  const char* c_str() const { return b_; }
  size_t length() const { return len_; }
  bool overflow() const { return over_; }
  void clear() { len_ = 0; over_ = false; if (b_ && cap_) b_[0] = 0; }
 private:
  char* b_; size_t cap_, len_ = 0; bool over_ = false;
};

class Portal {
 public:
  static constexpr size_t BUF_CAP = 256 * 1024;   // §15.4: JSON body limit; also the response and log-tail buffer
  typedef void (*AppFn)(void* ctx);

  // App task. start() brings the radio, the servers and the net task up; false = the AP did not start (!WIFI).
  bool start(AppState& app, const char* password, bool recovery);
  void stop();                                    // joins the net task (serving its last call), then rule 14's stop order
  bool running() const { return running_; }
  void tickApp();                                 // every app tick: run a call the net task is waiting on

  // Any task: ask the app to stop SETUP on its next tick (the page's "Turn off setup").
  void requestStop(const char* why);
  bool stopRequested() const { return stopReq_; }
  const char* stopReason() const { return stopWhy_; }

  // Net task: run fn(ctx) on the app task and wait for it. False when the portal is not running.
  bool callOnApp(AppFn fn, void* ctx);
  template <class F> bool onApp(F&& f) { return callOnApp([](void* c) { (*static_cast<F*>(c))(); }, (void*)&f); }

  // For the handlers.
  AppState&   app() { return *app_; }
  WebServer&  server() { return *server_; }
  char*       buf() { return buf_; }
  size_t      bufCap() const { return buf_ ? BUF_CAP : 0; }
  void        noteRequest(bool input);            // every request counts; input-class ones restart the idle timer (§15.1)
  bool        recovery() const { return recovery_; }
  const char* ssid() const { return ssid_; }
  const char* ip() const { return ip_; }
  uint8_t     clients() const;
  uint32_t    requests() const { return requests_; }
  uint32_t    lastInputRequestMs() const { return lastInputMs_; }
  uint32_t    lastRequestMs() const { return lastReqMs_; }
  uint32_t    startedAt() const { return startedAt_; }
  uint32_t    netStackMin() const;                // the net task's stack high-water mark, bytes

 private:
  static void netThunk(void* arg);
  void netMain();

  AppState*   app_ = nullptr;
  WebServer*  server_ = nullptr;
  DNSServer*  dns_ = nullptr;
  char*       buf_ = nullptr;
  void*       task_ = nullptr;
  void*       callQ_ = nullptr;                   // depth 1: the call the net task waits on
  void*       done_ = nullptr;                    // binary semaphore: the call ran
  volatile bool running_ = false, stopReq_ = false, netDone_ = true;
  bool        recovery_ = false;
  char        ssid_[33] = "";
  char        ip_[16] = "192.168.4.1";
  char        stopWhy_[32] = "";
  volatile uint32_t requests_ = 0, lastInputMs_ = 0, lastReqMs_ = 0;
  uint32_t    startedAt_ = 0;
};
