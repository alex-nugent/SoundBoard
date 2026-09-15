// The Bluetooth LE keyboard (FirmwareSpec.md §8): NimBLE through the core BLE
// library and BLEHIDDevice exactly as validation 10, with its three
// workarounds; "Just Works" bonding; one host at a time; the ready state of
// §8.2; the report scheduler of §8.3 lives in sbcore (KeyboardModel).
// NimBLE callbacks run on the host task: they only set flags that tick()
// reads on the app task, which is the only task that sends reports.
#pragma once
#include <stdint.h>
#include <Print.h>
#include "config/config.h"
#include "ble/keyboard_model.h"

class BLEHIDDevice;
class BLECharacteristic;

class BleKeyboard {
 public:
  enum class Link : uint8_t { Off, Advertising, Connecting, Ready };

  bool begin(const sb::Config& cfg);            // §17.2 G: stack up, advertising when enabled; false = init failed (!KBD)
  void configure(const sb::Config& cfg);        // §8.4: enable toggle at runtime, timings; the name waits for the next boot
  void tick(uint32_t now, uint8_t batteryPct, bool batteryValid);

  // §5.2 step 2. press() is false when the step was skipped (disabled, no host ready, nothing to send).
  bool press(const sb::Entry& e, uint16_t pressId, uint32_t now);
  void release(uint16_t pressId, uint32_t now);
  void releaseAll(const char* why, uint32_t now);
  void forget();                                // §8.6: disconnect, ble_store_clear(), advertise again
  void setPaused(bool on);                      // §8.5 setup.pauseKeyboard: off for the duration of SETUP, back after
  void shutdown(uint32_t now);                  // SLEEP / OFF: keys up, host disconnected, advertising stopped

  // Bench (console `kbdtype`, `kbdkey`).
  bool typeText(const char* text, uint32_t now);
  bool keyByName(const char* name, uint16_t holdMs, uint32_t now);

  bool    initOk() const { return init_; }
  bool    enabled() const { return enabled_; }
  Link    link() const;
  uint8_t bonds() const { return bonds_; }
  uint32_t notReadySince() const { return notReadySince_; }   // §11.4 NO TABLET after 10 s: restarted by init, enable and a lost host
  bool    takeMemoryFull() { bool f = memoryFull_; memoryFull_ = false; return f; }
  void    printStatus(Print& out);

  // NimBLE callbacks (host task).
  void cbConnect(uint16_t handle, const uint8_t* idAddr, uint8_t idType, const uint8_t* otaAddr);
  void cbDisconnect();
  void cbSubscribe(bool on);
  void cbUndelivered();

 private:
  void applyEnable();                           // enabled_ = cfgEnabled_ && !paused_, with the host drop / advertising start
  void send(const sb::KbdReport& r);
  void pump(uint32_t now);
  void refreshBonds();
  bool peerBonded() const;
  bool encrypted() const;
  void startAdvertising();
  void stopAdvertising();
  void disconnectHost();

  sb::KeyboardModel   m_;
  BLEHIDDevice*       hid_ = nullptr;
  BLECharacteristic*  inKbd_ = nullptr;
  BLECharacteristic*  inMedia_ = nullptr;
  bool     init_ = false, enabled_ = true, cfgEnabled_ = true, paused_ = false, advertising_ = false, ready_ = false, securityAsked_ = false, memoryFull_ = false, refusing_ = false;
  volatile bool     connected_ = false, subscribed_ = false, connEvent_ = false, discEvent_ = false;
  volatile uint16_t connHandle_ = 0xFFFF;
  volatile uint32_t undelivered_ = 0;
  uint8_t  peerId_[6] = {0}, peerIdType_ = 0, peerOta_[6] = {0};
  uint8_t  bonds_ = 0, lastBattery_ = 255;
  uint32_t notReadySince_ = 0;
  uint32_t connectedAt_ = 0, readyAt_ = 0, nextBattery_ = 0, reports_ = 0, connects_ = 0, typed_ = 0, skipped_ = 0;
  uint16_t benchOwner_ = 0;
  uint32_t benchUntil_ = 0;
  char     name_[25] = "";
  char     addr_[18] = "";
};
