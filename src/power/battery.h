// Battery measurement (FirmwareSpec.md §12.1, §12.2, §12.6): the two ADC
// methods selected by NVS `sb-cal/method`, the one-time import of validation
// 03's K, the 10 s sampling with quiet-window preference, the filter and the
// percentage (model in sbcore), USB and "full" detection, the RtcState copy.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <Print.h>
#include "power/battery_model.h"

struct RtcState;

class Battery {
 public:
  enum Method : uint8_t { Divider, Pulldown };

  void begin();                                  // NVS method/K/tcal (importing battcal/k once), ADC attach, first reading
  void restore(const RtcState& r);               // sleep-wake: filter, percentage, USB and full state carried over
  void save(RtcState& r) const;                  // sleep/off entry
  void tick(uint32_t now, bool quiet);           // every 10 s, preferably in a quiet window; forced after 60 s without one
  bool sampleNow();                              // one averaged reading into the filter (~10 ms)
  uint16_t readRawMv(uint16_t& adcMv);           // one 32-conversion average -> VBAT mV by the current method
  bool calibrate(float meterVolts, char* msg, size_t n);   // K from a meter reading on VBAT (unit 1); stores method "pulldown"
  bool clearCalibration();                       // back to the design divider
  void setFake(uint16_t mv, bool noUsb = false); // bench only: 0 = real readings; otherwise every sample reads this (RAM, lost at reset); noUsb also fakes USB absent
  uint16_t fake() const { return fakeMv_; }

  bool     valid() const { return valid_; }      // false: implausible reading or bad K (!BATT)
  uint16_t millivolts() const { return mv_; }
  uint8_t  percent() const { return pct_; }
  bool     usb() const { return usb_; }
  bool     full() const { return full_; }
  bool     changed();                            // percentage / USB / full / validity changed since the last call
  Method   method() const { return method_; }
  const char* methodName() const { return method_ == Pulldown ? "pulldown" : "divider"; }
  float    k() const { return k_; }
  float    tcal() const { return tcal_; }
  uint32_t lastSampleMs() const { return lastSample_; }
  void     printStatus(Print& out) const;

 private:
  bool store();
  uint16_t readAdc(bool pulldown);               // 32-conversion average of the ADC mV, with or without the pull-down leg
  Method   method_ = Divider;
  float    k_ = 6.525f, tcal_ = 33.0f;
  bool     kBad_ = false;
  sb::BatteryFilter filter_;
  bool     valid_ = false, usb_ = false, full_ = false, changed_ = false;
  uint16_t mv_ = 0, fakeMv_ = 0;
  bool     fakeNoUsb_ = false;
  uint8_t  pct_ = 0;
  uint32_t lastSample_ = 0, nextSample_ = 0, forceAt_ = 0;
  int64_t  fullSinceUs_ = 0;
  uint16_t prevRaw_ = 0;                                       // raw mV of the previous sample (USB-change log)
  uint8_t  lastSig_ = 0xFF;
};
