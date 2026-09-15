#include "ble/keyboard.h"
#include "diag/log.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <HIDTypes.h>
#define US_KEYBOARD                         // the core keymap[]: the HID-standard US layout (§8.3 "ASCII via the core keymap[]")
#include <HIDKeyboardTypes.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_store.h>
extern "C" int ble_store_clear(void);       // NimBLE: forget every bond (§8.6)

static const char* TAG = "kbd";
static BleKeyboard* s_self = nullptr;

// ---- HID report map: keyboard (id 1) + consumer control (id 2), as validation 10 -----------------
static const uint8_t REPORT_MAP[] = {
  USAGE_PAGE(1), 0x01, USAGE(1), 0x06, COLLECTION(1), 0x01,
    REPORT_ID(1), 0x01,
    USAGE_PAGE(1), 0x07, USAGE_MINIMUM(1), 0xE0, USAGE_MAXIMUM(1), 0xE7, LOGICAL_MINIMUM(1), 0x00, LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1), 0x01, REPORT_COUNT(1), 0x08, HIDINPUT(1), 0x02,               // modifiers
    REPORT_COUNT(1), 0x01, REPORT_SIZE(1), 0x08, HIDINPUT(1), 0x01,               // reserved
    REPORT_COUNT(1), 0x05, REPORT_SIZE(1), 0x01, USAGE_PAGE(1), 0x08, USAGE_MINIMUM(1), 0x01, USAGE_MAXIMUM(1), 0x05, HIDOUTPUT(1), 0x02,   // LEDs
    REPORT_COUNT(1), 0x01, REPORT_SIZE(1), 0x03, HIDOUTPUT(1), 0x01,              // LED padding
    REPORT_COUNT(1), 0x06, REPORT_SIZE(1), 0x08, LOGICAL_MINIMUM(1), 0x00, LOGICAL_MAXIMUM(1), 0x65,
    USAGE_PAGE(1), 0x07, USAGE_MINIMUM(1), 0x00, USAGE_MAXIMUM(1), 0x65, HIDINPUT(1), 0x00,   // six key codes
  END_COLLECTION(0),
  USAGE_PAGE(1), 0x0C, USAGE(1), 0x01, COLLECTION(1), 0x01,
    REPORT_ID(1), 0x02,
    USAGE_PAGE(1), 0x0C, LOGICAL_MINIMUM(1), 0x00, LOGICAL_MAXIMUM(1), 0x01, REPORT_SIZE(1), 0x01, REPORT_COUNT(1), 0x08,
    USAGE(1), 0xB5, USAGE(1), 0xB6, USAGE(1), 0xB7, USAGE(1), 0xCD, USAGE(1), 0xE2, USAGE(1), 0xE9, USAGE(1), 0xEA, USAGE(2), 0x23, 0x02,
    HIDINPUT(1), 0x02,
  END_COLLECTION(0)
};

static bool keymapLookup(uint8_t c, uint8_t& usage, uint8_t& mod) {
  if (c >= KEYMAP_SIZE) return false;
  usage = keymap[c].usage; mod = keymap[c].modifier;
  return usage != 0;
}

// ---- The three library workarounds of validation 10 ---------------------------------------------
static BLEDescriptorCallbacks s_descriptorNoop;
static void fixDescriptor(BLEDescriptor* d, bool readable) {
  if (!d) return;
  if (readable) d->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_READ_ENCRYPTED);   // 0x2908 readable, else the host drops every report
  d->setCallbacks(&s_descriptorNoop);                                                          // the read handler has no null check
}
static void fixReportReference(BLECharacteristic* c) { if (c) fixDescriptor(c->getDescriptorByUUID(BLEUUID((uint16_t)0x2908)), true); }

// §8.2: a fourth bond fails instead of evicting one (the core installs the round-robin handler).
static int storeStatus(struct ble_store_status_event* event, void* arg) {
  if (event->event_code == BLE_STORE_EVENT_OVERFLOW) { LOG_W(TAG, "bond store full: pairing refused"); return BLE_HS_ESTORE_CAP; }
  return ble_store_util_status_rr(event, arg);
}

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*, ble_gap_conn_desc* d) override { if (s_self) s_self->cbConnect(d->conn_handle, d->peer_id_addr.val, d->peer_id_addr.type, d->peer_ota_addr.val); }
  void onDisconnect(BLEServer*, ble_gap_conn_desc*) override { if (s_self) s_self->cbDisconnect(); }
};
class ReportCB : public BLECharacteristicCallbacks {
  bool kbd_;
 public:
  explicit ReportCB(bool kbd) : kbd_(kbd) {}
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc*, uint16_t subValue) override { if (kbd_ && s_self) s_self->cbSubscribe((subValue & 1) != 0); }
  void onStatus(BLECharacteristic*, Status st, uint32_t) override { if (st != Status::SUCCESS_NOTIFY && s_self) s_self->cbUndelivered(); }
};
class OutputCB : public BLECharacteristicCallbacks {};   // the host's LED report: nothing to do with it

// ---- Host-task callbacks: flags only -------------------------------------------------------------
void BleKeyboard::cbConnect(uint16_t handle, const uint8_t* idAddr, uint8_t idType, const uint8_t* otaAddr) {
  memcpy(peerId_, idAddr, 6); peerIdType_ = idType; memcpy(peerOta_, otaAddr, 6);
  connHandle_ = handle; subscribed_ = false; connected_ = true; connEvent_ = true;
}
void BleKeyboard::cbDisconnect() { connected_ = false; subscribed_ = false; connHandle_ = 0xFFFF; discEvent_ = true; }
void BleKeyboard::cbSubscribe(bool on) { subscribed_ = on; }
void BleKeyboard::cbUndelivered() { undelivered_++; }

// ---- Setup ---------------------------------------------------------------------------------------
bool BleKeyboard::begin(const sb::Config& cfg) {
  s_self = this;
  strlcpy(name_, cfg.device.name[0] ? cfg.device.name : "SoundBoard V4", sizeof name_);
  cfgEnabled_ = cfg.keyboard.enabled;
  enabled_ = cfgEnabled_ && !paused_ && !suspended_;
  m_.configure(cfg.keyboard.typeDelayMs, cfg.keyboard.holdKeysMaxMs);
  m_.setKeymap(keymapLookup);
  uint32_t t0 = millis();
  BLEDevice::init(name_);
  BLEServer* server = BLEDevice::createServer();
  if (!server) { LOG_E(TAG, "BLE server not created"); return false; }
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(false);                       // tick() decides (enable state, one host at a time)
  hid_ = new BLEHIDDevice(server);
  inKbd_ = hid_->inputReport(1);
  BLECharacteristic* outKbd = hid_->outputReport(1);
  inMedia_ = hid_->inputReport(2);
  if (!inKbd_ || !outKbd || !inMedia_) { LOG_E(TAG, "HID reports not created"); return false; }
  fixReportReference(inKbd_); fixReportReference(outKbd); fixReportReference(inMedia_);
  if (BLECharacteristic* bat = hid_->batteryService()->getCharacteristic(BLEUUID((uint16_t)0x2a19))) fixDescriptor(bat->getDescriptorByUUID(BLEUUID((uint16_t)0x2904)), false);
  outKbd->setCallbacks(new OutputCB());
  inKbd_->setCallbacks(new ReportCB(true));
  inMedia_->setCallbacks(new ReportCB(false));
  hid_->manufacturer()->setValue("Delve");                    // manufacturer(String) dereferences a characteristic only this creates
  hid_->pnp(0x02, 0xE502, 0xA111, 0x0210);
  hid_->hidInfo(0x00, 0x01);
  BLESecurity::setAuthenticationMode(true, false, true);      // bond, no MITM, secure connections: "Just Works"
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLESecurity::setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  hid_->reportMap((uint8_t*)REPORT_MAP, sizeof REPORT_MAP);
  hid_->startServices();
  hid_->setBatteryLevel(100);
  ble_hs_cfg.store_status_cb = storeStatus;
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C1);
  adv->addServiceUUID(hid_->hidService()->getUUID());
  adv->setScanResponse(true);
  strlcpy(addr_, BLEDevice::getAddress().toString().c_str(), sizeof addr_);
  refreshBonds();
  init_ = true;
  notReadySince_ = millis() ? millis() : 1;
  if (enabled_) startAdvertising();
  LOG_I(TAG, "BLE up in %lu ms: \"%s\" %s, bonds %u/%u, %s | heap free %lu", (unsigned long)(millis() - t0), name_, addr_, (unsigned)bonds_, (unsigned)CONFIG_BT_NIMBLE_MAX_BONDS,
        enabled_ ? "advertising" : "keyboard disabled", (unsigned long)ESP.getFreeHeap());
  return true;
}

void BleKeyboard::configure(const sb::Config& cfg) {
  m_.configure(cfg.keyboard.typeDelayMs, cfg.keyboard.holdKeysMaxMs);
  cfgEnabled_ = cfg.keyboard.enabled;
  applyEnable();
}

void BleKeyboard::setPaused(bool on) {                        // §8.5
  if (on == paused_) return;
  paused_ = on;
  LOG_I(TAG, "keyboard %s for SETUP (setup.pauseKeyboard)", on ? "paused" : "resumed");
  applyEnable();
}

void BleKeyboard::stopStack() {
  if (!init_) return;
  releaseAll("ble off", millis());
  if (connected_) { disconnectHost(); delay(30); }
  stopAdvertising();
  BLEDevice::deinit(false);
  init_ = false; enabled_ = false;
  LOG_W(TAG, "BLE stack stopped until the next boot | heap free %lu", (unsigned long)ESP.getFreeHeap());
}

void BleKeyboard::setSuspended(bool on) { if (on == suspended_) return; suspended_ = on; applyEnable(); }

void BleKeyboard::applyEnable() {
  bool en = cfgEnabled_ && !paused_ && !suspended_;
  if (en == enabled_) return;
  enabled_ = en;
  if (!init_) return;
  if (!en) {                                                  // §8.4: on -> off drops the host; the stack stays up
    releaseAll("keyboard disabled", millis());
    if (connected_) disconnectHost();
    stopAdvertising();
    LOG_I(TAG, "keyboard disabled: host dropped, advertising stopped");
  } else {
    notReadySince_ = millis() ? millis() : 1;
    startAdvertising();
    LOG_I(TAG, "keyboard enabled: advertising");
  }
}

void BleKeyboard::refreshBonds() {
  int n = 0;
  if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) != 0) n = 0;
  bonds_ = (uint8_t)(n < 0 ? 0 : n);
}

bool BleKeyboard::peerBonded() const {
  ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS]; int n = 0;
  if (ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) return false;
  for (int i = 0; i < n; i++) if (!memcmp(peers[i].val, peerId_, 6)) return true;
  return false;
}

bool BleKeyboard::encrypted() const {
  ble_gap_conn_desc d;
  uint16_t h = connHandle_;
  return h != 0xFFFF && ble_gap_conn_find(h, &d) == 0 && d.sec_state.encrypted;
}

void BleKeyboard::startAdvertising() { if (!init_ || advertising_ || connected_) return; BLEDevice::startAdvertising(); advertising_ = true; }
void BleKeyboard::stopAdvertising()  { if (!init_ || !advertising_) return; BLEDevice::stopAdvertising(); advertising_ = false; }
void BleKeyboard::disconnectHost()   { uint16_t h = connHandle_; if (h != 0xFFFF && BLEDevice::getServer()) BLEDevice::getServer()->disconnect(h); }

// ---- The app-task tick ---------------------------------------------------------------------------
void BleKeyboard::tick(uint32_t now, uint8_t batteryPct, bool batteryValid) {
  if (!init_) return;
  if (connEvent_) {
    connEvent_ = false; connects_++; connectedAt_ = now; securityAsked_ = false; advertising_ = false; refusing_ = false;
    refreshBonds();
    bool known = peerBonded();
    LOG_I(TAG, "host connected %02X:%02X:%02X:%02X:%02X:%02X (%s, bonds %u/%u)", peerOta_[5], peerOta_[4], peerOta_[3], peerOta_[2], peerOta_[1], peerOta_[0],
          known ? "bonded" : "new", (unsigned)bonds_, (unsigned)CONFIG_BT_NIMBLE_MAX_BONDS);
    if (!known && bonds_ >= CONFIG_BT_NIMBLE_MAX_BONDS) {      // §8.2: the store is full, a fourth host is refused
      LOG_W(TAG, "keyboard memory full: a fourth host is refused (forget hosts on the settings page)");
      memoryFull_ = true; refusing_ = true;
      disconnectHost();
    }
  }
  if (discEvent_) {
    discEvent_ = false;
    LOG_I(TAG, "host disconnected%s", refusing_ ? " (refused)" : "");
    refusing_ = false;
  }
  bool r = connected_ && subscribed_ && !refusing_ && encrypted();
  if (r != ready_) {
    ready_ = r;
    m_.setReady(r);                                           // losing the host drops the queue and any held key
    if (r) { readyAt_ = now; notReadySince_ = 0; refreshBonds(); LOG_I(TAG, "host ready (encrypted, subscribed) %lu ms after connect; bonds %u/%u", (unsigned long)(now - connectedAt_), (unsigned)bonds_, (unsigned)CONFIG_BT_NIMBLE_MAX_BONDS); }
    else { notReadySince_ = now ? now : 1; LOG_I(TAG, "host no longer ready"); }
  }
  if (connected_ && !refusing_ && !securityAsked_ && !encrypted() && now - connectedAt_ > 500) {   // validation 10: some hosts wait for us
    securityAsked_ = true;
    uint16_t h = connHandle_;
    if (h != 0xFFFF) { int rc = ble_gap_security_initiate(h); LOG_I(TAG, "link still unencrypted 500 ms after connect: pairing requested (rc %d)", rc); }
  }
  if (!connected_ && enabled_ && !advertising_) startAdvertising();
  if (benchOwner_ && (int32_t)(now - benchUntil_) >= 0) { uint16_t o = benchOwner_; benchOwner_ = 0; m_.release(o); }
  pump(now);
  if (ready_ && (int32_t)(now - nextBattery_) >= 0) {         // §8.1: battery service every 60 s
    nextBattery_ = now + 60000;
    uint8_t pct = batteryValid ? batteryPct : 100;
    if (pct != lastBattery_) { lastBattery_ = pct; hid_->setBatteryLevel(pct); }
  }
}

void BleKeyboard::send(const sb::KbdReport& r) {
  BLECharacteristic* c = r.id == 1 ? inKbd_ : inMedia_;
  c->setValue((uint8_t*)r.data, r.len);
  c->notify();
  reports_++;
}

void BleKeyboard::pump(uint32_t now) {
  sb::KbdReport r;
  for (uint8_t n = 0; n < 8 && m_.next(now, r); n++) { if (ready_) send(r); }
  if (m_.takeForcedFlag()) LOG_W(TAG, "held key released by keyboard.holdKeysMaxMs (forced release #%lu)", (unsigned long)m_.forced());
}

// ---- §5.2 step 2 ---------------------------------------------------------------------------------
bool BleKeyboard::press(const sb::Entry& e, uint16_t pressId, uint32_t now) {
  if (!init_ || !enabled_) return false;
  bool hasText = e.type[0] != 0;
  const sb::KeyDef* key = e.key[0] ? sb::findKey(e.key) : nullptr;
  if (!hasText && !key) return false;
  if (!ready_) { skipped_++; LOG_D(TAG, "press [%u]: no host ready, nothing sent", (unsigned)pressId); return false; }
  bool ok = false;
  switch (e.keyMode) {
    case sb::KeyMode::Type: ok = hasText ? m_.type(e.type) : m_.tap(*key); if (ok && hasText) typed_++; break;
    case sb::KeyMode::Hold: ok = key ? m_.hold(*key, pressId) : m_.type(e.type); break;
    case sb::KeyMode::Tap:  ok = key ? m_.tap(*key) : m_.type(e.type); break;
  }
  if (!ok) LOG_W(TAG, "press [%u] dropped: the report queue is full (%u waiting)", (unsigned)pressId, (unsigned)m_.queued());
  else LOG_I(TAG, "press [%u]: %s %s", (unsigned)pressId, e.keyMode == sb::KeyMode::Type ? "type" : e.keyMode == sb::KeyMode::Hold ? "hold" : "tap",
             (e.keyMode == sb::KeyMode::Type && hasText) ? e.type : (key ? key->name : e.type));
  pump(now);                                                  // the first report leaves now (budget: 20 ms)
  return ok;
}

void BleKeyboard::release(uint16_t pressId, uint32_t now) { if (!init_) return; m_.release(pressId); pump(now); }

void BleKeyboard::releaseAll(const char* why, uint32_t now) {
  if (!init_) return;
  if (m_.holding()) LOG_I(TAG, "held %s released: %s", m_.heldKey()->name, why);
  m_.releaseAll();
  pump(now);
}

void BleKeyboard::forget() {
  if (!init_) return;
  releaseAll("forget hosts", millis());
  if (connected_) { disconnectHost(); delay(50); }
  int r = ble_store_clear();
  refreshBonds();
  LOG_I(TAG, "all keyboard hosts forgotten (ble_store_clear -> %d, bonds now %u); forget the board on the host too", r, (unsigned)bonds_);
  if (enabled_ && !connected_) { advertising_ = false; startAdvertising(); }
}

void BleKeyboard::shutdown(uint32_t now) {
  if (!init_) return;
  releaseAll("sleep", now);
  if (connected_) { disconnectHost(); delay(30); }
  stopAdvertising();
}

// ---- Bench ---------------------------------------------------------------------------------------
bool BleKeyboard::typeText(const char* text, uint32_t now) {
  if (!init_ || !ready_) return false;
  bool ok = m_.type(text); if (ok) typed_++;
  pump(now);
  return ok;
}

bool BleKeyboard::keyByName(const char* name, uint16_t holdMs, uint32_t now) {
  const sb::KeyDef* k = sb::findKey(name);
  if (!k || !init_ || !ready_) return false;
  bool ok;
  if (holdMs) { benchOwner_ = 0xFFF0; benchUntil_ = now + holdMs; ok = m_.hold(*k, benchOwner_); }
  else ok = m_.tap(*k);
  pump(now);
  return ok;
}

// ---- State ---------------------------------------------------------------------------------------
BleKeyboard::Link BleKeyboard::link() const {
  if (!init_ || !enabled_) return Link::Off;
  if (ready_) return Link::Ready;
  if (connected_ && !refusing_) return Link::Connecting;
  return Link::Advertising;
}

void BleKeyboard::printStatus(Print& out) {
  if (!init_) { out.println("keyboard: BLE not initialised"); return; }
  const char* st = !enabled_ ? "disabled" : ready_ ? "READY" : connected_ ? "connecting" : advertising_ ? "advertising" : "idle";
  out.printf("keyboard: %s | \"%s\" %s | bonds %u/%u | host %s", st, name_, addr_, (unsigned)bonds_, (unsigned)CONFIG_BT_NIMBLE_MAX_BONDS,
             connected_ ? "" : "none");
  if (connected_) out.printf("%02X:%02X:%02X:%02X:%02X:%02X (%s, %s)", peerOta_[5], peerOta_[4], peerOta_[3], peerOta_[2], peerOta_[1], peerOta_[0],
                             encrypted() ? "encrypted" : "not encrypted", subscribed_ ? "subscribed" : "not subscribed");
  out.printf(" | reports %lu sent, %lu undelivered | held %s | queue %u%s | typed %lu, skipped %lu (no host), dropped %lu (queue full), forced releases %lu\n",
             (unsigned long)reports_, (unsigned long)undelivered_, m_.holding() ? m_.heldKey()->name : "--", (unsigned)m_.queued(), m_.typing() ? " (typing)" : "",
             (unsigned long)typed_, (unsigned long)skipped_, (unsigned long)m_.dropped(), (unsigned long)m_.forced());
}
