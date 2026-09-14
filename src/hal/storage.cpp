#include "hal/storage.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Storage {

static SemaphoreHandle_t s_mutex = nullptr;
static IdleFn s_idle = nullptr;
struct Item { WriteFn fn; void* arg; };
static Item   s_queue[4];
static uint8_t s_n = 0;

void begin() {
  if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();   // priority inheritance: the audio task is never starved for longer than one chunk
}
void lock()   { if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
bool tryLock(uint32_t timeoutMs) { return s_mutex && xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE; }
void unlock() { if (s_mutex) xSemaphoreGiveRecursive(s_mutex); }

static bool idle() { return s_idle ? s_idle() : true; }

bool deferredWrite(WriteFn fn, void* arg) {
  if (idle() && s_n == 0) { fn(arg); return true; }
  if (s_n >= 4) return false;
  s_queue[s_n++] = { fn, arg };
  return true;
}

void setIdleCheck(IdleFn isIdle) { s_idle = isIdle; }

void tick(uint32_t) {
  while (s_n > 0 && idle()) {
    Item it = s_queue[0];
    for (uint8_t i = 1; i < s_n; i++) s_queue[i - 1] = s_queue[i];
    s_n--;
    it.fn(it.arg);
  }
}

bool pending() { return s_n > 0; }

}  // namespace Storage
