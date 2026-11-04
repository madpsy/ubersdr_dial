// USB transport for controller mode: the same records as Bluetooth, framed over
// the USB serial port so the v2 page can reach the dial with Web Serial.
//
// The port also carries the firmware's log text, so every frame is
// COBS-encoded (no zero bytes inside) and has a zero byte before and after it.
// Text never contains a zero byte, so the reader splits on zeros and anything
// that doesn't decode to a frame with a good CRC is log text, skipped.
//
//   frame (before COBS): [type u8][payload ...][crc16 LE]
//   crc16: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type + payload
//
//   1 HELLO  page -> dial [version = 1]; sent on open and then once a second.
//            dial -> page [version = 1][name, UTF-8]; the answer to each.
//   2 INPUT  dial -> page: input records, as the BLE input notification
//   3 STATE  page -> dial: the 20-byte state record
//   4 NAME   page -> dial: receiver name
//   5 SPEC   page -> dial: a spectrum line record (sent whole: 172 bytes)
//   6 BYE    page -> dial: the page is closing the port
//
// A HELLO connects the page; after that any frame keeps it connected. Three
// seconds with nothing and the dial goes back to waiting.
#include <WiFi.h>
#include <lvgl.h>

#include "ctrl_link.h"

enum : uint8_t { F_HELLO = 1, F_INPUT = 2, F_STATE = 3, F_NAME = 4, F_SPEC = 5, F_BYE = 6 };

static const uint32_t kTimeoutMs = 3000;
static String s_name;
static bool s_connected = false;
static uint32_t s_last_hello = 0;
static uint8_t s_rx[400];  // one COBS frame in; the biggest (SPEC) is ~178
static size_t s_rxlen = 0;
static bool s_rx_overflow = false;

static uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)*p++ << 8;
    for (int i = 0; i < 8; i++) c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

// COBS: returns the decoded length, or 0 if it isn't valid COBS.
static size_t cobs_decode(const uint8_t *in, size_t n, uint8_t *out) {
  size_t i = 0, o = 0;
  while (i < n) {
    uint8_t code = in[i++];
    if (!code || i + code - 1 > n) return 0;
    for (uint8_t k = 1; k < code; k++) out[o++] = in[i++];
    if (code < 0xFF && i < n) out[o++] = 0;
  }
  return o;
}

static size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out) {
  size_t o = 1, code_at = 0;
  uint8_t code = 1;
  for (size_t i = 0; i < n; i++) {
    if (in[i]) {
      out[o++] = in[i];
      code++;
    }
    if (!in[i] || code == 0xFF) {
      out[code_at] = code;
      code_at = o++;
      code = 1;
    }
  }
  out[code_at] = code;
  return o;
}

static void send_frame(uint8_t type, const uint8_t *payload, size_t n) {
  uint8_t raw[64];
  if (n + 3 > sizeof(raw)) return;
  raw[0] = type;
  memcpy(raw + 1, payload, n);
  uint16_t c = crc16(raw, n + 1);
  raw[n + 1] = c & 0xFF;
  raw[n + 2] = c >> 8;
  uint8_t out[sizeof(raw) + sizeof(raw) / 254 + 3];
  out[0] = 0;
  size_t len = cobs_encode(raw, n + 3, out + 1) + 1;
  out[len++] = 0;
  // One write, so a log line from another task can't land inside the frame.
  Serial.write(out, len);
}

static void set_connected(bool on) {
  if (on == s_connected) return;
  s_connected = on;
  if (!on) ctrl_rx_lost();
  Serial.printf("[usb] page %s\n", on ? "connected" : "gone, waiting");
}

static void on_frame(const uint8_t *f, size_t n) {
  if (n < 3) return;
  uint16_t c = f[n - 2] | (f[n - 1] << 8);
  if (crc16(f, n - 2) != c) return;  // log text, or a damaged frame
  const uint8_t *p = f + 1;
  size_t len = n - 3;
  if (s_connected) s_last_hello = millis();  // any frame keeps the link up
  switch (f[0]) {
    case F_HELLO: {
      s_last_hello = millis();
      set_connected(true);
      uint8_t reply[40];
      reply[0] = 1;
      size_t nl = s_name.length() < sizeof(reply) - 1 ? s_name.length() : sizeof(reply) - 1;
      memcpy(reply + 1, s_name.c_str(), nl);
      send_frame(F_HELLO, reply, nl + 1);
      break;
    }
    case F_BYE: set_connected(false); break;
    // Records only count once the page has said hello.
    case F_STATE: if (s_connected) ctrl_rx_state(p, len); break;
    case F_NAME: if (s_connected) ctrl_rx_name(p, len); break;
    case F_SPEC: if (s_connected) ctrl_rx_spec(p, len); break;
  }
}

static void usb_poll() {
  int avail = Serial.available();
  while (avail-- > 0) {
    int b = Serial.read();
    if (b < 0) break;
    if (b == 0) {
      if (s_rxlen && !s_rx_overflow) {
        uint8_t dec[sizeof(s_rx)];
        size_t n = cobs_decode(s_rx, s_rxlen, dec);
        if (n) on_frame(dec, n);
      }
      s_rxlen = 0;
      s_rx_overflow = false;
    } else if (s_rxlen < sizeof(s_rx)) {
      s_rx[s_rxlen++] = (uint8_t)b;
    } else {
      s_rx_overflow = true;
    }
  }
  if (s_connected && (millis() - s_last_hello > kTimeoutMs || !HWCDC::isConnected())) set_connected(false);
}

static void usb_start() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "UberSDR Dial %02X%02X", mac[4], mac[5]);
  s_name = name;
  Serial.printf("[usb] controller over USB serial as \"%s\"\n", name);
}

static bool usb_connected() { return s_connected; }
static String usb_name() { return s_name; }
static void usb_write_input(const uint8_t *buf, size_t len) { send_frame(F_INPUT, buf, len); }

const CtrlTransport kUsbTransport = {
    LV_SYMBOL_USB,
    "Plug into a computer, open\nUberSDR in Chrome or the\ndesktop app, then SDR Control\n" LV_SYMBOL_RIGHT
    "  Dial  " LV_SYMBOL_RIGHT "  USB  " LV_SYMBOL_RIGHT "  Connect",
    usb_start,
    usb_connected,
    usb_write_input,
    usb_poll,
    usb_name,
};
