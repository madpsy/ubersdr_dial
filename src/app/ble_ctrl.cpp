#include "ble_ctrl.h"

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <lvgl.h>

static const char *kService = "89540001-8c77-4d5f-9411-e7534b50c873";
static const char *kInput = "89540002-8c77-4d5f-9411-e7534b50c873";
static const char *kState = "89540003-8c77-4d5f-9411-e7534b50c873";
static const char *kName = "89540004-8c77-4d5f-9411-e7534b50c873";
static const char *kSpec = "89540005-8c77-4d5f-9411-e7534b50c873";

static NimBLECharacteristic *s_in;
static volatile bool s_connected = false;
static String s_name;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
    s_connected = true;
    // Ask for a short connection interval (7.5-15 ms): the default the host
    // picks can be 30-50 ms, which can't carry the waterfall and state.
    srv->updateConnParams(desc->conn_handle, 6, 12, 0, 400);
    Serial.printf("[ble] browser connected (interval %u x1.25 ms), asking for 7.5-15 ms\n",
                  (unsigned)desc->conn_itvl);
  }
  void onDisconnect(NimBLEServer *srv) override {
    s_connected = false;
    ctrl_rx_lost();
    Serial.println("[ble] browser disconnected, advertising again");
    NimBLEDevice::startAdvertising();
  }
};

// Each characteristic carries one kind of record, exactly as written.
class RecordCB : public NimBLECharacteristicCallbacks {
 public:
  explicit RecordCB(void (*fn)(const uint8_t *, size_t)) : fn_(fn) {}
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    fn_((const uint8_t *)v.data(), v.size());
  }

 private:
  void (*fn_)(const uint8_t *, size_t);
};

static void ble_start() {
  uint8_t mac[6];
  WiFi.macAddress(mac);  // same factory MAC base; Wi-Fi itself stays off
  char name[32];
  snprintf(name, sizeof(name), "UberSDR Dial %02X%02X", mac[4], mac[5]);
  s_name = name;

  NimBLEDevice::init(name);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);  // a whole 172-byte waterfall line in one write
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  NimBLEService *svc = srv->createService(kService);
  s_in = svc->createCharacteristic(kInput, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  const uint32_t wr = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
  svc->createCharacteristic(kState, wr)->setCallbacks(new RecordCB(ctrl_rx_state));
  svc->createCharacteristic(kName, wr)->setCallbacks(new RecordCB(ctrl_rx_name));
  svc->createCharacteristic(kSpec, wr)->setCallbacks(new RecordCB(ctrl_rx_spec));
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kService);  // the page's requestDevice() filters on this
  adv->setScanResponse(true);     // name goes in the scan response
  adv->start();
  Serial.printf("[ble] advertising as \"%s\"\n", name);
}

static bool ble_connected() { return s_connected; }
static String ble_name() { return s_name; }

static void ble_write_input(const uint8_t *buf, size_t len) {
  s_in->setValue(buf, len);
  s_in->notify();
}

const CtrlTransport kBleTransport = {
    LV_SYMBOL_BLUETOOTH,
    "Open UberSDR in Chrome\nor the desktop app, then\nSDR Control  " LV_SYMBOL_RIGHT "  Dial  " LV_SYMBOL_RIGHT
    "  Connect",
    ble_start,
    ble_connected,
    ble_write_input,
    nullptr,
    ble_name,
};
