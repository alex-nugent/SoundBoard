#include "app/pmode.h"
#include "util/event.h"
#include "diag/log.h"
#include <Arduino.h>
#include <esp_random.h>

static const char* TAG = "pmode";

bool PModeTask::start(const sb::Config& cfg) {
  if (running_) return true;
  if (!exited_) stop();                                        // a previous task still winding down
  configure(cfg);
  running_ = true; exited_ = false;
  TaskHandle_t h = nullptr;
  // Core 0 with the radio at the lowest priority: the app and audio tasks never wait for it (§19.1).
  if (xTaskCreatePinnedToCore(taskThunk, "pmode", 4096, this, 1, &h, 0) != pdPASS) {
    running_ = false; exited_ = true;
    LOG_E(TAG, "task not created");
    return false;
  }
  task_ = h;
  LOG_I(TAG, "ON: k %.4g, %u bits/s per pad, %.2f sigma (%.4f), %s", (double)k_, (unsigned)hz_, (double)sigma_, (double)m_.threshold(), twoSided_ ? "two-sided" : "one-sided");
  return true;
}

void PModeTask::stop() {
  if (exited_) { running_ = false; return; }
  running_ = false;
  uint32_t t0 = millis();
  while (!exited_ && millis() - t0 < 100) delay(1);
  if (!exited_) LOG_W(TAG, "task did not exit within 100 ms");
  else LOG_I(TAG, "OFF after %lu words, %lu trigger(s)", (unsigned long)m_.samples(), (unsigned long)triggers());
}

void PModeTask::configure(const sb::Config& cfg) {
  k_ = cfg.pmode.k; sigma_ = cfg.pmode.sigma; twoSided_ = cfg.pmode.test == sb::PMODE_TWO_SIDED;
  hz_ = cfg.pmode.sampleHz ? cfg.pmode.sampleHz : 1000;
  if (running_) reconfigure_ = true;                           // the task applies it between words (no shared writes to the model)
  else m_.configure(k_, sigma_, twoSided_);
}

uint32_t PModeTask::triggers() const {
  uint32_t n = 0;
  for (uint8_t c = 0; c < sb::PModeModel::CHANNELS; c++) n += m_.triggers(c);
  return n;
}

void PModeTask::taskThunk(void* arg) { static_cast<PModeTask*>(arg)->run(); vTaskDelete(nullptr); }

void PModeTask::run() {
  // Every 10 ms: hz / 100 words on average (a remainder accumulator keeps any rate exact). esp_random()
  // paces itself to the generator's safe read rate, so a burst of 50 words costs about a millisecond.
  uint32_t acc = 0;
  TickType_t last = xTaskGetTickCount();
  while (running_) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    if (reconfigure_) { reconfigure_ = false; m_.configure(k_, sigma_, twoSided_); }
    acc += hz_;
    while (acc >= 100 && running_) {
      acc -= 100;
      int hit = m_.feed(esp_random());
      if (hit >= 0) {
        Event e = { Ev::PModeTrigger, millis(), {} };
        e.pad.pos = (uint8_t)hit;
        EventBus::post(e);
      }
    }
  }
  exited_ = true;
}

void PModeTask::printStatus(Print& out) const {
  out.printf("p mode: %s | k %.4g, %u bits/s, %.2f sigma = %.4f from 0.5, %s | %lu words, %lu trigger(s) [%lu %lu %lu %lu] | averages %.4f %.4f %.4f %.4f%s\n",
             running_ ? "ON" : "off", (double)k_, (unsigned)hz_, (double)sigma_, (double)m_.threshold(), twoSided_ ? "two-sided" : "one-sided",
             (unsigned long)m_.samples(), (unsigned long)triggers(), (unsigned long)m_.triggers(0), (unsigned long)m_.triggers(1), (unsigned long)m_.triggers(2), (unsigned long)m_.triggers(3),
             (double)m_.avg(0), (double)m_.avg(1), (double)m_.avg(2), (double)m_.avg(3), running_ ? "" : " (never saved: off at every power-up)");
}
