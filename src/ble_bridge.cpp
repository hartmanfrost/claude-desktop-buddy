#include "ble_bridge.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <string.h>
#include <esp_random.h>

// ESP32-C6 only has the NimBLE host — Bluedroid isn't available — so this
// port uses h2zero/NimBLE-Arduino instead of the BLEDevice classes the
// upstream firmware used. The wire-level Nordic UART Service and the
// pairing flow (LE Secure Connections, DisplayOnly IO, 6-digit passkey)
// are preserved so the desktop side doesn't need any changes.

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static NimBLEServer*         server = nullptr;
static NimBLECharacteristic* txChar = nullptr;
static NimBLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

// ---------------------------------------------------------------------------
// RX characteristic — desktop writes JSON lines here.
// ---------------------------------------------------------------------------
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    if (!v.empty()) rxPush((const uint8_t*)v.data(), v.size());
  }
};

// ---------------------------------------------------------------------------
// Server + pairing callbacks. NimBLE-Arduino v2.x merges server-level and
// security-level events into NimBLEServerCallbacks, so this one class does
// what BLEServerCallbacks + BLESecurityCallbacks did upstream.
// ---------------------------------------------------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    Serial.printf("[ble] disconnected (reason=%d)\n", reason);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t newMtu, NimBLEConnInfo& info) override {
    mtu = newMtu;
    Serial.printf("[ble] mtu=%u\n", mtu);
  }
  uint32_t onPassKeyDisplay() override {
    // Generate a fresh 6-digit passkey on each pairing attempt. esp_random()
    // is a hardware TRNG on the C6, so the value is unpredictable to
    // an attacker even if they snoop the advertising channels.
    passkey = esp_random() % 1000000;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)passkey);
    return passkey;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    secure = info.isEncrypted();
    passkey = 0;
    Serial.printf("[ble] auth %s\n", secure ? "ok" : "FAIL");
    if (!secure && server) {
      server->disconnect(info.getConnHandle());
    }
  }
};

void bleInit(const char* deviceName) {
  NimBLEDevice::init(deviceName);
  // Biggest MTU we can negotiate. macOS typically lands at 185, Linux/Web
  // Bluetooth at 247.
  NimBLEDevice::setMTU(517);
  // DisplayOnly: device shows the 6-digit passkey, central types it.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  // bonding=true, mitm=true, sc=true → LE Secure Connections bonding.
  NimBLEDevice::setSecurityAuth(true, true, true);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

  // NimBLE auto-generates the 0x2902 CCCD descriptor when NOTIFY is set;
  // no need for the explicit BLE2902 the upstream code added.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC
  );
  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->setPreferredParams(0x06, 0x12);   // iOS-friendly connection interval
  adv->setName(deviceName);
  NimBLEDevice::startAdvertising();

  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool     bleConnected() { return connected; }
bool     bleSecure()    { return secure; }
uint32_t blePasskey()   { return passkey; }

void bleClearBonds() {
  int n = NimBLEDevice::getNumBonds();
  if (n > 0) NimBLEDevice::deleteAllBonds();
  Serial.printf("[ble] cleared %d bond(s)\n", n);
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  // ATT notify payload is (negotiated MTU - 3). Chunk to that.
  size_t chunk = mtu > 3 ? (size_t)(mtu - 3) : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    delay(4);
  }
  return sent;
}
