#pragma once
#include <Arduino.h>

#include "dial_link.h"

// Bluetooth controller mode: the dial as a BLE peripheral driven by the
// UberSDR v2 web UI (SDR Control panel -> Dial, over Web Bluetooth).
// Protocol: docs/BLE_PROTOCOL.md (mirrors static/v2/src/controls/dial.js).

// Input codes, dial -> page
enum : uint8_t {
  BLE_PAGE_TUNE = 1, BLE_PAGE_VOL = 2, BLE_PAGE_ZOOM = 3, BLE_PAGE_SQL = 4, BLE_PAGE_BW = 5, BLE_PAGE_STEP = 6,
  BLE_MODE_TAP = 16, BLE_MODE_HOLD = 17, BLE_FREQ_TAP = 18, BLE_FREQ_HOLD = 19,
  BLE_MUTE_TAP = 20, BLE_MUTE_HOLD = 21, BLE_CENTER_TAP = 22,
};

// Receiver state, page -> dial (shared with receiver mode: dial_link.h)
using BleRxState = DialState;

void ble_ctrl_start();          // init NimBLE + advertise (call once)
bool ble_ctrl_connected();
String ble_ctrl_name();         // advertised name, e.g. "UberSDR Dial 40E8"
String ble_ctrl_peer_name();    // receiver name the page sent ("" until then)
BleRxState ble_ctrl_state();    // snapshot
void ble_ctrl_send(uint8_t code, int8_t value);  // queue an input record

// Waterfall lines from the page (~5/s): SPEC_POINTS bytes, dB = b/2 - 160,
// spanning centre +/- span/2. Returns true once per completed line.
static constexpr int BLE_SPEC_POINTS = 160;
bool ble_ctrl_take_line(uint8_t *out, uint32_t *center_hz, uint32_t *span_hz);
void ble_ctrl_loop();           // flushes queued input (call from the UI loop)
