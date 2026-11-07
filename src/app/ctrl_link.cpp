#include "ctrl_link.h"

#include "../ui/theme.h"

static const CtrlTransport *s_t = &kBleTransport;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static DialState s_state;
static char s_peer[33] = "";

// Spectrum line assembly (written by whichever task the transport reads on,
// read by the UI loop).
static uint8_t s_line_build[CTRL_SPEC_POINTS], s_line_ready[CTRL_SPEC_POINTS];
static uint8_t s_line_seq = 0;
static int s_line_have = 0;
static uint32_t s_line_center = 0, s_line_span = 0, s_ready_center = 0, s_ready_span = 0;
static volatile bool s_line_new = false;

// Queued input records, flushed from the UI loop (several per write).
static uint8_t s_q[40];
static int s_qlen = 0;
static uint32_t s_last_flush = 0;

static inline int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ctrl_rx_state(const uint8_t *p, size_t n) {
  if (n < 18 || p[0] != 1) return;  // version 1 only
  DialState s;
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
  // Flag b5: bytes 18-19 hold the baseband power, dBFS x 100 (older pages
  // leave them zero and the flag clear).
  if (n >= 20 && (p[7] & 32)) s.dbfs = (int16_t)le16(p + 18) / 100.0f;
  // Bytes 20-27: the receiver's tuning range (older pages send 20 bytes). Zero,
  // or a max not above the min, is unknown: the 10 kHz - 30 MHz default stays.
  if (n >= 28) {
    uint32_t lo = le32(p + 20), hi = le32(p + 24);
    if (hi > lo) {
      s.min_hz = lo;
      s.max_hz = hi;
    }
  }
  s.updated_ms = millis();
  portENTER_CRITICAL(&s_mux);
  s_state = s;
  portEXIT_CRITICAL(&s_mux);
}

void ctrl_rx_spec(const uint8_t *p, size_t n) {
  if (n < 12 || p[0] != 1) return;
  uint8_t seq = p[1], off = p[2], cnt = p[3];
  if (off + cnt > CTRL_SPEC_POINTS || n < 12u + cnt) return;
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
  if (s_line_have >= CTRL_SPEC_POINTS) {
    portENTER_CRITICAL(&s_mux);
    memcpy(s_line_ready, s_line_build, CTRL_SPEC_POINTS);
    s_ready_center = s_line_center;
    s_ready_span = s_line_span;
    s_line_new = true;
    portEXIT_CRITICAL(&s_mux);
    s_line_have = 0;
  }
}

void ctrl_rx_name(const uint8_t *p, size_t n) {
  if (n > sizeof(s_peer) - 1) n = sizeof(s_peer) - 1;
  portENTER_CRITICAL(&s_mux);
  // Over USB the page repeats it every second, so only a change is news.
  bool same = strlen(s_peer) == n && !memcmp(s_peer, p, n);
  memcpy(s_peer, p, n);
  s_peer[n] = 0;
  portEXIT_CRITICAL(&s_mux);
  if (!same) Serial.printf("[ctrl] receiver name \"%.*s\"\n", (int)n, (const char *)p);
}

void ctrl_rx_lost() {
  portENTER_CRITICAL(&s_mux);
  s_state.valid = false;
  s_peer[0] = 0;
  portEXIT_CRITICAL(&s_mux);
  s_qlen = 0;
}

void ctrl_start(const CtrlTransport *t) {
  s_t = t;
  s_t->start();
}

const CtrlTransport *ctrl_transport() { return s_t; }
String ctrl_name() { return s_t->name(); }

// ---------------------------------------------------------------- DialLink
static bool link_connected() { return s_t->connected(); }

static DialState link_state() {
  portENTER_CRITICAL(&s_mux);
  DialState s = s_state;
  portEXIT_CRITICAL(&s_mux);
  return s;
}

static void link_send(uint8_t code, int8_t value) {
  // A turn of nothing is nothing; a tap at the very centre (98, 0) is a tap.
  if (!s_t->connected() || (!value && code != DIAL_TAP_TUNE)) return;
  // Merge consecutive turns of the same page into one record.
  if (s_qlen >= 2 && s_q[s_qlen - 2] == code && (code <= DIAL_PAGE_STEP || code == DIAL_PAN)) {
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

static void link_loop() {
  if (s_t->poll) s_t->poll();
  // ~50 writes/s at most; anything turned in between rides along.
  if (!s_qlen || !s_t->connected() || millis() - s_last_flush < 20) return;
  s_t->write_input(s_q, s_qlen);
  s_qlen = 0;
  s_last_flush = millis();
}

static bool link_take_line(float *db, int *n, uint32_t *center_hz, uint32_t *span_hz) {
  if (!s_line_new) return false;
  uint8_t raw[CTRL_SPEC_POINTS];
  portENTER_CRITICAL(&s_mux);
  memcpy(raw, s_line_ready, CTRL_SPEC_POINTS);
  *center_hz = s_ready_center;
  *span_hz = s_ready_span;
  s_line_new = false;
  portEXIT_CRITICAL(&s_mux);
  for (int i = 0; i < CTRL_SPEC_POINTS; i++) db[i] = raw[i] / 2.0f - 160;
  *n = CTRL_SPEC_POINTS;
  return true;
}

static void link_status(String *text, uint32_t *dot) {
  DialState st = link_state();
  char peer[sizeof(s_peer)];
  portENTER_CRITICAL(&s_mux);
  memcpy(peer, s_peer, sizeof(peer));
  portEXIT_CRITICAL(&s_mux);
  String sym = String(s_t->symbol) + " ";
  if (!s_t->connected()) {
    *text = sym + "waiting";
    *dot = COL_DIM;
  } else {
    *text = sym + (peer[0] ? String(peer) : String("connected"));
    *dot = st.valid && st.audio_up ? COL_OK : COL_WARN;
  }
}

// v2's zoom limits: MIN_ZOOM_SPAN_HZ (spectrum-connection.js) up to the whole
// receiver: its max frequency (30 MHz unless the page says otherwise).
static void link_zoom_range(uint32_t *min_hz, uint32_t *max_hz) {
  uint32_t top = link_state().max_hz;
  *min_hz = 2048;
  *max_hz = top > *min_hz ? top : DIAL_DEFAULT_MAX_HZ;
}

const DialLink kCtrlLink = {
    false, link_loop, link_connected, link_state, link_send, link_take_line, link_status, link_zoom_range,
};
