#include "util/event.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace EventBus {

static QueueHandle_t s_q = nullptr;

void begin() { if (!s_q) s_q = xQueueCreate(32, sizeof(Event)); }
bool post(const Event& e) { return s_q && xQueueSend(s_q, &e, portMAX_DELAY) == pdTRUE; }
bool postCoalesced(const Event& e) { return s_q && xQueueSend(s_q, &e, 0) == pdTRUE; }
bool poll(Event& out) { return s_q && xQueueReceive(s_q, &out, 0) == pdTRUE; }

}  // namespace EventBus
