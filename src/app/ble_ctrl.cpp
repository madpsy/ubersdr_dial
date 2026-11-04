#include "ble_ctrl.h"

#include <NimBLEDevice.h>
#include <WiFi.h>

#include "../ui/theme.h"

static const char *kService = "89540001-8c77-4d5f-9411-e7534b50c873";
static const char *kInput = "89540002-8c77-4d5f-9411-e7534b50c873";
static const char *kState = "89540003-8c77-4d5f-9411-e7534b50c873";
static const char *kName = "89540004-8c77-4d5f-9411-e7534b50c873";
static const char *kSpec = "89540005-8c77-4d5f-9411-e7534b50c873";

// Spectrum line assembly (written by the NimBLE host task, read by the UI)
static uint8_t s_line_build[BLE_SPEC_POINTS], s_line_ready[BLE_SPEC_POINTS];
static uint8_t s_line_seq = 0;
static int s_line_have = 0;
static uint32_t s_line_center = 0, s_line_span = 0, s_ready_center = 0, s_ready_span = 0;
static volatile bool s_line_new = false;


static NimBLECharacteristic *s_in;
static volatile bool s_connected = false;
static String s_name;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static BleRxState s_state;
static char s_peer[33] = "";

// Queued input records, flushed from the UI loop (several per notification).
static uint8_t s_q[40];
static int s_qlen = 0;
static uint32_t s_last_flush = 0;

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
    portENTER_CRITICAL(&s_mux);
    s_state.valid = false;
    s_peer[0] = 0;
    portEXIT_CRITICAL(&s_mux);
    Serial.println("[ble] browser disconnected, advertising again");
    NimBLEDevice::startAdvertising();
  }
};

static inline int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

class StateCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.size() < 18 || (uint8_t)v[0] != 1) return;  // version 1 only
    const uint8_t *p = (const uint8_t *)v.data();
    BleRxState s;
    s.valid = true;
    s.freq_hz = le32(p + 1);
    s.mode = p[5];
    s.snr = (int8_t)p[6] == -128 ? NAN : (float)(int8_t)p[6];
    s.audio_up = p[7] & 1;
    s.sql_on = p[7] & 2;
    s.sql_open = p[7] & 4;
    s.muted = p[7] & 8;
    s.locked = p[7] & 16;
    s.volume = p[8];
    s.sql_db = (int8_t)p[9] == -128 ? NAN : (float)(int8_t)p[9];
    s.bw_lo = le16(p + 10);
    s.bw_hi = le16(p + 12);
    s.step_hz = le32(p + 14);
    s.updated_ms = millis();
    portENTER_CRITICAL(&s_mux);
    s_state = s;
    portEXIT_CRITICAL(&s_mux);
  }
};

class SpecCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.size() < 12 || (uint8_t)v[0] != 1) return;
    const uint8_t *p = (const uint8_t *)v.data();
    uint8_t seq = p[1], off = p[2], cnt = p[3];
    if (off + cnt > BLE_SPEC_POINTS || v.size() < 12u + cnt) return;
    if (off == 0) {  // a new line starts
      s_line_seq = seq;
      s_line_have = 0;
      s_line_center = le32(p + 4);
      s_line_span = le32(p + 8);
    } else if (seq != s_line_seq) {
      return;  // a piece of a line whose start we missed
    }
    memcpy(s_line_build + off, p + 12, cnt);
    s_line_have += cnt;
    if (s_line_have >= BLE_SPEC_POINTS) {
      portENTER_CRITICAL(&s_mux);
      memcpy(s_line_ready, s_line_build, BLE_SPEC_POINTS);
      s_ready_center = s_line_center;
      s_ready_span = s_line_span;
      s_line_new = true;
      portEXIT_CRITICAL(&s_mux);
      s_line_have = 0;
    }
  }
};

class NameCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    portENTER_CRITICAL(&s_mux);
    size_t n = v.size() < sizeof(s_peer) - 1 ? v.size() : sizeof(s_peer) - 1;
    memcpy(s_peer, v.data(), n);
    s_peer[n] = 0;
    portEXIT_CRITICAL(&s_mux);
    Serial.printf("[ble] receiver name \"%.*s\"\n", (int)n, v.data());
  }
};

void ble_ctrl_start() {
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
  NimBLECharacteristic *st = svc->createCharacteristic(kState, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  st->setCallbacks(new StateCB());
  NimBLECharacteristic *nm = svc->createCharacteristic(kName, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  nm->setCallbacks(new NameCB());
  NimBLECharacteristic *sp = svc->createCharacteristic(kSpec, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  sp->setCallbacks(new SpecCB());
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kService);  // the page's requestDevice() filters on this
  adv->setScanResponse(true);     // name goes in the scan response
  adv->start();
  Serial.printf("[ble] advertising as \"%s\"\n", name);
}

bool ble_ctrl_connected() { return s_connected; }
String ble_ctrl_name() { return s_name; }

String ble_ctrl_peer_name() {
  char tmp[sizeof(s_peer)];
  portENTER_CRITICAL(&s_mux);
  memcpy(tmp, s_peer, sizeof(tmp));
  portEXIT_CRITICAL(&s_mux);
  return String(tmp);
}

BleRxState ble_ctrl_state() {
  portENTER_CRITICAL(&s_mux);
  BleRxState s = s_state;
  portEXIT_CRITICAL(&s_mux);
  return s;
}

bool ble_ctrl_take_line(uint8_t *out, uint32_t *center_hz, uint32_t *span_hz) {
  if (!s_line_new) return false;
  portENTER_CRITICAL(&s_mux);
  memcpy(out, s_line_ready, BLE_SPEC_POINTS);
  *center_hz = s_ready_center;
  *span_hz = s_ready_span;
  s_line_new = false;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void ble_ctrl_send(uint8_t code, int8_t value) {
  // A turn of nothing is nothing; a tap at the very centre (98, 0) is a tap.
  if (!s_connected || (!value && code != 98)) return;
  // Merge consecutive turns of the same page into one record.
  if (s_qlen >= 2 && s_q[s_qlen - 2] == code && (code <= BLE_PAGE_STEP || code == 97)) {
    int v = (int8_t)s_q[s_qlen - 1] + value;
    if (v >= -127 && v <= 127) {
      s_q[s_qlen - 1] = (uint8_t)(int8_t)v;
      return;
    }
  }
  if (s_qlen + 2 > (int)sizeof(s_q)) return;  // full: drop (it's a knob, not a ledger)
  s_q[s_qlen++] = code;
  s_q[s_qlen++] = (uint8_t)value;
}

void ble_ctrl_loop() {
  // ~50 notifications/s at most; anything turned in between rides along.
  if (!s_qlen || !s_connected || millis() - s_last_flush < 20) return;
  s_in->setValue(s_q, s_qlen);
  s_in->notify();
  s_qlen = 0;
  s_last_flush = millis();
}

// ---------------------------------------------------------------- DialLink
static bool link_take_line(float *db, int *n, uint32_t *center_hz, uint32_t *span_hz) {
  uint8_t raw[BLE_SPEC_POINTS];
  if (!ble_ctrl_take_line(raw, center_hz, span_hz)) return false;
  for (int i = 0; i < BLE_SPEC_POINTS; i++) db[i] = raw[i] / 2.0f - 160;
  *n = BLE_SPEC_POINTS;
  return true;
}

static void link_status(String *text, uint32_t *dot) {
  BleRxState st = ble_ctrl_state();
  String peer = ble_ctrl_peer_name();
  if (!s_connected) {
    *text = LV_SYMBOL_BLUETOOTH " waiting";
    *dot = COL_DIM;
  } else {
    *text = String(LV_SYMBOL_BLUETOOTH " ") + (peer.length() ? peer : String("connected"));
    *dot = st.valid && st.audio_up ? COL_OK : COL_WARN;
  }
}

const DialLink kBleLink = {
    false, ble_ctrl_loop, ble_ctrl_connected, ble_ctrl_state, ble_ctrl_send, link_take_line, link_status,
};
