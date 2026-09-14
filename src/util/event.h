// The event bus (FirmwareSpec.md §19.2): every cross-module notification is an
// Event on one FreeRTOS queue (depth 32) consumed by the app task.
#pragma once
#include <stdint.h>

enum class Ev : uint8_t {
  PadDown, PadUp, PadStuck, BtnEdge, ButtonCommand, AudioStarted, AudioDone, AudioReleased, RailReady, RailDown,
  KcxLine, KcxLink, BleLink, BleBondFull, ConfigApply, ConfigChanged, HttpActivity, BatteryUpdate, UsbChanged,
  Tick1s, SetupRequest, JobProgress, Fault
};

struct Event {
  Ev       type;
  uint32_t t;
  union {
    struct { uint8_t pos, ch; int16_t delta; uint16_t pressId; uint16_t heldMs; } pad;   // delta in 0.1 %; heldMs on PadUp
    struct { uint8_t id; } command;   // sb::ButtonCmd
    // BtnEdge: u32 = button (0 minus, 1 plus) | down << 8
    struct { uint16_t requestId; bool interrupted; } audio;
    uint32_t u32;
  };
};

namespace EventBus {
void begin();
bool post(const Event& e);            // unbounded wait
bool postCoalesced(const Event& e);   // dropped when the queue is full (Tick1s, BatteryUpdate)
bool poll(Event& out);                // non-blocking
}  // namespace EventBus
