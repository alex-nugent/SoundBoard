#include "net/portal.h"
#include "net/api.h"
#include "app/state.h"
#include "diag/log.h"
#include "util/strutil.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

static const char* TAG = "net";
static constexpr const char* MDNS_NAME = "soundboard";

size_t BufPrint::write(const uint8_t* p, size_t n) {
  if (!b_ || cap_ == 0) return 0;
  size_t room = cap_ - 1 - len_;
  if (n > room) { n = room; over_ = true; }
  memcpy(b_ + len_, p, n);
  len_ += n; b_[len_] = 0;
  return n;
}

struct Call { Portal::AppFn fn; void* ctx; };

// ---------------------------------------------------------------------------
bool Portal::start(AppState& app, const char* password, bool recovery) {
  if (running_) return true;
  app_ = &app; recovery_ = recovery; stopReq_ = false; stopWhy_[0] = 0;
  requests_ = 0; lastInputMs_ = lastReqMs_ = startedAt_ = millis();
  if (!callQ_) callQ_ = xQueueCreate(1, sizeof(Call));
  if (!done_)  done_ = xSemaphoreCreateBinary();
  if (!buf_) buf_ = static_cast<char*>(heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM));
  if (!callQ_ || !done_ || !buf_) { LOG_E(TAG, "portal: out of memory"); return false; }

  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);          // rule 14: WiFi.macAddress() reads zeros before the radio is up
  snprintf(ssid_, sizeof ssid_, "SoundBoard-%02X%02X", mac[4], mac[5]);
  uint32_t t0 = millis();
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1);                                  // the core's soft-AP default; the server binds to it (§15.2)
  if (!WiFi.softAP(ssid_, password)) {
    LOG_E(TAG, "soft AP \"%s\" did not start", ssid_);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  delay(50);
  sb::copyStr(ip_, sizeof ip_, WiFi.softAPIP().toString().c_str());
  if (!dns_) dns_ = new DNSServer();
  dns_->setTTL(30);
  dns_->start(53, "*", WiFi.softAPIP());
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80); else LOG_W(TAG, "mDNS did not start");
  if (!server_) { server_ = new WebServer(ip, 80); Api::bind(*this); }   // bound to the AP address only (§15.2)
  server_->begin();
  netDone_ = false; running_ = true;
  BaseType_t ok = xTaskCreatePinnedToCore(&Portal::netThunk, "net", 12 * 1024, this, 1, (TaskHandle_t*)&task_, 0);   // §19.1
  if (ok != pdPASS) { LOG_E(TAG, "net task did not start"); running_ = false; netDone_ = true; stop(); return false; }
  LOG_I(TAG, "SETUP on in %lu ms: \"%s\" password \"%s\" http://%s/ (also http://%s.local/)%s | heap free %lu",
        (unsigned long)(millis() - t0), ssid_, password, ip_, MDNS_NAME, recovery ? " | RECOVERY" : "", (unsigned long)ESP.getFreeHeap());
  return true;
}

void Portal::stop() {
  if (!running_ && netDone_) return;
  running_ = false; stopReq_ = true;
  uint32_t t0 = millis();
  while (!netDone_ && millis() - t0 < 6000) { tickApp(); esp_task_wdt_reset(); delay(2); }   // a handler mid-call still gets served
  if (!netDone_) LOG_E(TAG, "net task did not stop");
  task_ = nullptr;
  // Rule 14's stop order.
  if (server_) server_->stop();
  if (dns_) dns_->stop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  stopReq_ = false;
  LOG_I(TAG, "SETUP off after %lu s, %lu request(s) | heap free %lu", (unsigned long)((millis() - startedAt_) / 1000), (unsigned long)requests_, (unsigned long)ESP.getFreeHeap());
}

void Portal::requestStop(const char* why) {
  sb::copyStr(stopWhy_, sizeof stopWhy_, why ? why : "");
  stopReq_ = true;
}

// ---------------------------------------------------------------------------
// The net task and the app-task bridge
// ---------------------------------------------------------------------------
void Portal::netThunk(void* arg) { static_cast<Portal*>(arg)->netMain(); }

void Portal::netMain() {
  while (running_) {
    server_->handleClient();
    dns_->processNextRequest();
    vTaskDelay(1);
  }
  netDone_ = true;
  vTaskDelete(NULL);
}

bool Portal::callOnApp(AppFn fn, void* ctx) {
  if (!running_ && netDone_) return false;
  Call c = { fn, ctx };
  xQueueSend((QueueHandle_t)callQ_, &c, portMAX_DELAY);
  xSemaphoreTake((SemaphoreHandle_t)done_, portMAX_DELAY);
  return true;
}

void Portal::tickApp() {
  if (!callQ_) return;
  Call c;
  while (xQueueReceive((QueueHandle_t)callQ_, &c, 0) == pdTRUE) {
    c.fn(c.ctx);
    xSemaphoreGive((SemaphoreHandle_t)done_);
  }
}

void Portal::noteRequest(bool input) {
  requests_ = requests_ + 1;
  lastReqMs_ = millis();
  if (input) lastInputMs_ = millis();
}

uint8_t Portal::clients() const { return running_ ? WiFi.softAPgetStationNum() : 0; }
uint32_t Portal::netStackMin() const { return task_ ? uxTaskGetStackHighWaterMark((TaskHandle_t)task_) * sizeof(StackType_t) : 0; }
