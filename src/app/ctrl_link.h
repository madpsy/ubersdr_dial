#pragma once
#include <Arduino.h>

#include "dial_link.h"

// Controller mode: the dial as a remote for the UberSDR v2 page (SDR Control
// panel -> Dial). The records are the same whichever way they travel
// (docs/PROTOCOL.md, mirrored by static/v2/src/controls/dial.js); a transport
// only carries them:
//   Bluetooth: one GATT characteristic per record kind (ble_ctrl.cpp)
//   USB:       framed over the USB serial port, Web Serial (usb_ctrl.cpp)

struct CtrlTransport {
  const char *symbol;  // header and waiting-panel icon
  const char *how;     // waiting-panel instructions (short lines: round screen)
  void (*start)();
  bool (*connected)();  // a page is attached
  void (*write_input)(const uint8_t *buf, size_t len);  // a batch of input records
  void (*poll)();       // read side, from the UI loop (nullptr: callbacks do it)
  String (*name)();     // the name the page sees, e.g. "UberSDR Dial 40E8"
};

extern const CtrlTransport kBleTransport;  // ble_ctrl.cpp
extern const CtrlTransport kUsbTransport;  // usb_ctrl.cpp

// Start the transport and route the dial screen through it (kCtrlLink).
void ctrl_start(const CtrlTransport *t);
const CtrlTransport *ctrl_transport();
String ctrl_name();
extern const DialLink kCtrlLink;

// Waterfall lines from the page: SPEC_POINTS bytes, dB = b/2 - 160, spanning
// centre +/- span/2.
static constexpr int CTRL_SPEC_POINTS = 160;

// For the transports: one record each, as the page wrote it.
void ctrl_rx_state(const uint8_t *p, size_t n);
void ctrl_rx_spec(const uint8_t *p, size_t n);
void ctrl_rx_name(const uint8_t *p, size_t n);
void ctrl_rx_lost();  // the page went away: forget its state and name
