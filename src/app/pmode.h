// P mode (FirmwareSpec.md §4.5): the sampling task around sb::PModeModel.
// It draws 32-bit words from the chip's hardware random number generator at
// pmode.sampleHz and feeds them to the model; a trigger is one PModeTrigger
// event on the bus, which the app task turns into a full press of that pad.
// The generator is true random only while the radio is on (§4.5): start()
// refuses unless the BLE stack is up. Nothing here is saved: a power-up
// always starts with P mode off.
#pragma once
#include <stdint.h>
#include <Print.h>
#include "config/config.h"
#include "app/pmode_model.h"

class PModeTask {
 public:
  bool start(const sb::Config& cfg);           // creates the task; false = already running or no task
  void stop();                                 // asks the task to exit and joins it (<= 100 ms)
  void configure(const sb::Config& cfg);       // §4.5: k, S, the test and the rate apply live; the averages restart at 0.5
  bool on() const { return running_; }
  uint32_t triggers() const;                   // all channels
  uint32_t lastTriggerMs() const { return lastTriggerMs_; }
  const sb::PModeModel& model() const { return m_; }
  uint16_t sampleHz() const { return hz_; }
  void printStatus(Print& out) const;
  void noteTrigger(uint32_t now) { lastTriggerMs_ = now; }

 private:
  static void taskThunk(void* arg);
  void run();

  sb::PModeModel m_;
  volatile bool running_ = false, exited_ = true, reconfigure_ = false;
  volatile float    k_ = 0.01f, sigma_ = 4.5f;
  volatile bool     twoSided_ = true;
  volatile uint16_t hz_ = 1000;
  uint32_t lastTriggerMs_ = 0;
  void*    task_ = nullptr;
};
