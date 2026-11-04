#include "sdr.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFiClientSecure.h>
#include <esp32s3/rom/miniz.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

// Threading: all networking (HTTP /connection, TLS handshakes, WebSocket I/O,
// decoding) runs in sdr_task on core 0 so the UI loop on core 1 never blocks.
// Everything the UI reads or writes is guarded by s_mx.
static SemaphoreHandle_t s_mx;
#define LOCK() xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

// Requests from the UI
static String s_req_url;
static uint32_t s_req_gen = 0;  // bumped by sdr_connect / sdr_disconnect

static WebSocketsClient s_ws;
static SdrState s_state = SdrState::Idle;
static String s_status;
static String s_host, s_path_prefix;
static uint16_t s_port = 443;
static bool s_tls = true;
static String s_session;
static bool s_ws_started = false;
static uint32_t s_retry_at = 0;

// Server-reported geometry
static uint32_t s_bin_count = 0;
static double s_bin_bw = 0;
static uint32_t s_max_hz = 0;

// Spectrum state (v2)
static constexpr int kMaxBins = 8192;
static uint8_t *s_codes = nullptr;   // raw FFT order (what deltas index)
static uint8_t *s_unwrapped = nullptr;  // low -> high frequency, for the UI
static uint16_t s_bins = 0;
static float s_ref_db = 0, s_step_db = 1;
static bool s_have_scale = false;
static uint64_t s_frame_center = 0;
static bool s_new_frame = false;

// Requested view
static uint32_t s_want_center = 0, s_want_span = 0;
static bool s_view_dirty = false;
// Server poll divisor (v2's set_rate): 1 = full rate .. 8. Fewer frames when
// the dial doesn't need them is less Wi-Fi, inflate and decode work.
static uint8_t s_want_div = 1;
static bool s_div_dirty = false;
static uint32_t s_fps_n = 0, s_fps_at = 0;
static float s_fps = 0;
static uint32_t s_last_cmd = 0, s_last_ping = 0;

// Reconnect handling: we don't use the library's auto-reconnect (it reuses a
// session UUID the server may already have dropped). Any drop tears down and
// starts a fresh session with backoff.
static bool s_restart = false;
static uint8_t s_attempt = 0;
static uint32_t s_last_frame = 0;
static uint32_t s_gen = 0;
static uint32_t s_connect_started = 0;
static bool s_registered = false;  // /connection accepted for s_session

static String make_uuid() {
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char s[37];
  snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
           b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return s;
}

static bool parse_url(const String &url) {
  String u = url;
  if (u.startsWith("https://")) {
    s_tls = true;
    s_port = 443;
    u = u.substring(8);
  } else if (u.startsWith("http://")) {
    s_tls = false;
    s_port = 80;
    u = u.substring(7);
  } else {
    return false;
  }
  int slash = u.indexOf('/');
  String hostport = slash >= 0 ? u.substring(0, slash) : u;
  s_path_prefix = slash >= 0 ? u.substring(slash) : "/";
  if (!s_path_prefix.endsWith("/")) s_path_prefix += "/";
  int colon = hostport.indexOf(':');
  if (colon >= 0) {
    s_port = hostport.substring(colon + 1).toInt();
    hostport = hostport.substring(0, colon);
  }
  s_host = hostport;
  return !s_host.isEmpty();
}

// POST /connection registers the session UUID and tells us if we're allowed.
static bool post_connection(String *why) {
  String url = String(s_tls ? "https://" : "http://") + s_host + ":" + s_port + s_path_prefix + "connection";
  WiFiClientSecure tls;
  WiFiClient plain;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("UberSDR-Dial/0.1");
  bool ok = s_tls ? http.begin(tls, url) : http.begin(plain, url);
  if (!ok) {
    *why = "bad URL";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{\"user_session_id\":\"" + s_session + "\"}");
  String body = http.getString();
  http.end();
  if (code <= 0) {
    // Ours or theirs? A TLS setup that couldn't get memory isn't the receiver.
    char err[64];
    int tls_err = s_tls ? tls.lastError(err, sizeof(err)) : 0;
    *why = tls_err == -0x7F00 /* MBEDTLS_ERR_SSL_ALLOC_FAILED */ ? "dial out of memory" : "receiver not responding";
    return false;
  }
  if (code == 502 || code == 503 || code == 504) {
    *why = "receiver unavailable (" + String(code) + ")";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    *why = "HTTP " + String(code);
    return false;
  }
  if (!(doc["allowed"] | false)) {
    *why = doc["reason"] | "not allowed";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- decoding
static void handle_json(const char *txt, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, txt, len)) return;
  const char *type = doc["type"] | "";
  LOCK();
  if (!strcmp(type, "config") || !strcmp(type, "status")) {
    if (doc["binCount"].is<int>()) s_bin_count = doc["binCount"];
    if (doc["binBandwidth"].is<double>()) s_bin_bw = doc["binBandwidth"];
    if (!strcmp(type, "config") && s_max_hz == 0) {
      double c = doc["centerFreq"] | 0.0, tot = doc["totalBandwidth"] | 0.0;
      // First config is the receiver's default full-span view.
      if (c > 0 && tot > 0) s_max_hz = (uint32_t)(c + tot / 2);
      Serial.printf("[sdr] config bins=%u binbw=%.2f coverage=0..%u Hz\n", (unsigned)s_bin_count, s_bin_bw,
                    (unsigned)s_max_hz);
      s_view_dirty = true;  // apply our saved view now that binCount is known
    }
  } else if (!strcmp(type, "error")) {
    String err = doc["error"] | "error";
    Serial.printf("[sdr] server error: %s\n", err.c_str());
    s_status = err;
    String l = err;
    l.toLowerCase();
    if (l.indexOf("invalid session") >= 0 || l.indexOf("refresh") >= 0) {
      // Session expired: re-register with a fresh UUID.
      s_restart = true;
    }
  }
  UNLOCK();
}

static void handle_gzip(const uint8_t *p, size_t len) {
  // gzip member: 10-byte header (+ optional fields), raw deflate, 8-byte trailer
  if (len < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return;
  uint8_t flg = p[3];
  size_t off = 10;
  if (flg & 0x04) off += 2 + (p[off] | (p[off + 1] << 8));
  if (flg & 0x08) while (off < len && p[off++]) {}
  if (flg & 0x10) while (off < len && p[off++]) {}
  if (flg & 0x02) off += 2;
  if (off >= len - 8) return;
  uint32_t isize = p[len - 4] | (p[len - 3] << 8) | (p[len - 2] << 16) | ((uint32_t)p[len - 1] << 24);
  if (isize == 0 || isize > 64 * 1024) return;
  char *out = (char *)malloc(isize + 1);
  if (!out) return;
  // The ROM inflater's state is ~11 KB: keep it off the (8-16 KB) task stack.
  static tinfl_decompressor *inf = nullptr;
  if (!inf) inf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
  if (!inf) {
    free(out);
    return;
  }
  tinfl_init(inf);
  size_t in_len = len - 8 - off, out_len = isize;
  tinfl_status st = tinfl_decompress(inf, p + off, &in_len, (mz_uint8 *)out, (mz_uint8 *)out, &out_len,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (st == TINFL_STATUS_DONE) handle_json(out, out_len);
  free(out);
}

static void handle_spec(const uint8_t *m, size_t len) {
  if (len < 24 || m[4] != 2) return;
  uint8_t flags = m[5];
  uint64_t center = 0;
  for (int i = 7; i >= 0; i--) center = (center << 8) | m[16 + i];
  const uint8_t *body = m + 24;
  size_t blen = len - 24;
  LOCK();
  struct Unlock { ~Unlock() { UNLOCK(); } } unlock_on_return;
  if (flags == 0x05) {  // full frame
    if (blen < 3) return;
    int16_t ref = (int16_t)(body[0] | (body[1] << 8));
    uint8_t step = body[2];
    if (!step) return;
    size_t n = blen - 3;
    if (n > kMaxBins) n = kMaxBins;
    memcpy(s_codes, body + 3, n);
    s_bins = n;
    s_ref_db = ref / 100.0f;
    s_step_db = step / 100.0f;
    s_have_scale = true;
  } else if (flags == 0x06) {  // delta: change mask (LSB-first) + values
    if (!s_have_scale || !s_bins) return;
    size_t mlen = (s_bins + 7) / 8;
    if (blen < mlen) return;
    size_t set = 0;
    for (size_t i = 0; i < mlen; i++) set += __builtin_popcount(body[i]);
    if (blen != mlen + set) return;  // malformed: wait for the next keyframe
    const uint8_t *v = body + mlen;
    for (size_t i = 0; i < mlen; i++) {
      uint8_t b = body[i];
      while (b) {
        int bit = __builtin_ctz(b);
        size_t idx = i * 8 + bit;
        if (idx < s_bins) s_codes[idx] = *v;
        v++;
        b &= b - 1;
      }
    }
  } else {
    return;
  }
  s_frame_center = center;
  s_new_frame = true;
  uint32_t now_ms = millis();
  if (s_last_frame && now_ms - s_last_frame > 500)
    Serial.printf("[gap] spectrum %lu ms at t=%lu\n", (unsigned long)(now_ms - s_last_frame), (unsigned long)now_ms);
  s_last_frame = now_ms;
  s_fps_n++;
  if (now_ms - s_fps_at >= 10000) {
    s_fps = s_fps_at ? s_fps_n * 1000.0f / (now_ms - s_fps_at) : 0;
    if (s_fps_at) Serial.printf("[sdr] %.1f frames/s (divisor %u)\n", s_fps, (unsigned)s_want_div);
    s_fps_n = 0;
    s_fps_at = now_ms;
  }
  s_attempt = 0;  // frames flowing: reset reconnect backoff
}

static void ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[sdr] websocket connected");
      LOCK();
      s_state = SdrState::Connected;
      s_status = "live";
      s_last_ping = s_last_frame = millis();
      s_view_dirty = true;  // a new session starts at the server default view
      s_div_dirty = s_want_div > 1;  // and at full rate
      UNLOCK();
      break;
    case WStype_DISCONNECTED:
      Serial.printf("[sdr] websocket disconnected (%.*s), heap %u internal %u\n", (int)length,
                    payload ? (const char *)payload : "", (unsigned)ESP.getFreeHeap(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      s_restart = true;
      break;
    case WStype_ERROR:
      Serial.printf("[sdr] websocket error (%.*s)\n", (int)length, payload ? (const char *)payload : "");
      s_restart = true;
      break;
    case WStype_TEXT:
      handle_json((const char *)payload, length);
      break;
    case WStype_BIN:
      if (length >= 4 && !memcmp(payload, "SPEC", 4)) handle_spec(payload, length);
      else handle_gzip(payload, length);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------- public
static void start_session() {
  String uuid = make_uuid();
  LOCK();
  s_session = uuid;
  s_status = "registering";
  UNLOCK();
  String why;
  if (!post_connection(&why)) {
    Serial.printf("[sdr] /connection refused: %s\n", why.c_str());
    LOCK();
    s_state = SdrState::Error;
    s_status = why + " - retrying";
    s_retry_at = millis() + 10000;
    UNLOCK();
    return;
  }
  LOCK();
  s_gen++;
  s_registered = true;  // audio may start now, in parallel with the spectrum handshake
  UNLOCK();
  s_connect_started = millis();
  Serial.printf("[sdr] opening spectrum socket, internal heap %u (largest %u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  String path = s_path_prefix + "ws/user-spectrum?user_session_id=" + s_session + "&mode=binary8&version=2";
  if (s_tls) s_ws.beginSSL(s_host.c_str(), s_port, path.c_str());
  else s_ws.begin(s_host.c_str(), s_port, path.c_str());
  s_ws.onEvent(ws_event);
  // NB: the library also gates the *first* connect on this interval, so it
  // must stay small. Drops are still handled by our own restart logic.
  s_ws.setReconnectInterval(2000);
  s_ws_started = true;
  LOCK();
  s_restart = false;
  s_status = "connecting";
  s_have_scale = false;
  UNLOCK();
}

// Task-side: apply a connect/disconnect request from the UI.
static void apply_request(const String &url) {
  if (s_ws_started) s_ws.disconnect();
  s_ws_started = false;
  LOCK();
  s_max_hz = 0;
  s_bin_count = 0;
  s_have_scale = false;
  s_new_frame = false;
  s_attempt = 0;
  s_restart = false;
  s_retry_at = 0;
  s_registered = false;
  if (url.isEmpty()) {
    s_state = SdrState::Idle;
    s_status = "";
  } else if (!parse_url(url)) {
    s_state = SdrState::Error;
    s_status = "bad URL";
  } else {
    s_state = SdrState::Connecting;
    s_status = "connecting";
  }
  UNLOCK();
}

static void sdr_task(void *) {
  uint32_t seen_gen = 0;
  for (;;) {
    LOCK();
    uint32_t req = s_req_gen;
    String url = s_req_url;
    SdrState st = s_state;
    uint32_t retry_at = s_retry_at;
    UNLOCK();
    if (req != seen_gen) {
      seen_gen = req;
      apply_request(url);
      continue;
    }
    if (st == SdrState::Idle || (st == SdrState::Error && !s_ws_started && s_host.isEmpty())) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!s_ws_started) {
      if ((int32_t)(millis() - retry_at) >= 0) start_session();
      else vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    uint32_t t0 = millis();
    s_ws.loop();  // may run callbacks, which lock as needed
    uint32_t dt = millis() - t0;
    if (dt > 200) Serial.printf("[gap] spectrum ws.loop blocked %lu ms at t=%lu\n", (unsigned long)dt, (unsigned long)millis());

    uint32_t now = millis();
    LOCK();
    // A failed TLS/WebSocket connect raises no event: time it out ourselves.
    if (s_state == SdrState::Connecting && now - s_connect_started > 15000) {
      Serial.println("[sdr] connect timed out");
      s_connect_started = now;
      s_restart = true;
    }
    // Watchdog: connected but nothing arriving.
    if (s_state == SdrState::Connected && now - s_last_frame > 10000) {
      Serial.println("[sdr] no spectrum for 10 s, restarting session");
      s_restart = true;
    }
    bool restart = s_restart;
    if (restart) {
      s_restart = false;
      s_registered = false;
      uint32_t backoff = 1000u << (s_attempt < 4 ? s_attempt : 4);  // 1,2,4,8,16 s
      if (s_attempt < 255) s_attempt++;
      s_retry_at = now + backoff;
      s_state = SdrState::Connecting;
      s_status = "reconnecting (" + String(s_attempt) + ")";
    }
    char msg[96];
    msg[0] = 0;
    bool ping = false;
    if (!restart && s_state == SdrState::Connected) {
      if (s_view_dirty && s_bin_count && now - s_last_cmd >= 120) {
        double bw = (double)s_want_span / s_bin_count;
        snprintf(msg, sizeof(msg), "{\"type\":\"zoom\",\"frequency\":%u,\"binBandwidth\":%.4f}",
                 (unsigned)s_want_center, bw);
        s_view_dirty = false;
        s_last_cmd = now;
      }
      if (!msg[0] && s_div_dirty) {
        snprintf(msg, sizeof(msg), "{\"type\":\"set_rate\",\"divisor\":%u}", (unsigned)s_want_div);
        s_div_dirty = false;
      }
      if (now - s_last_ping > 20000) {
        ping = true;
        s_last_ping = now;
      }
    }
    UNLOCK();
    if (restart) {
      s_ws.disconnect();
      s_ws_started = false;
      continue;
    }
    if (msg[0]) s_ws.sendTXT(msg);
    if (ping) s_ws.sendTXT("{\"type\":\"ping\"}");
    vTaskDelay(1);
  }
}

void sdr_init() {
  s_mx = xSemaphoreCreateMutex();
  s_codes = (uint8_t *)calloc(kMaxBins, 1);
  s_unwrapped = (uint8_t *)calloc(kMaxBins, 1);
  // TLS handshakes + HTTP + JSON need a roomy stack.
  xTaskCreatePinnedToCore(sdr_task, "sdr_net", 16384, nullptr, 2, nullptr, 0);
}

void sdr_connect(const String &base_url) {
  LOCK();
  s_req_url = base_url;
  s_req_gen++;
  s_state = SdrState::Connecting;  // reflect immediately in the UI
  s_status = "connecting";
  UNLOCK();
}

void sdr_disconnect() {
  LOCK();
  s_req_url = "";
  s_req_gen++;
  s_state = SdrState::Idle;
  s_status = "";
  UNLOCK();
}

void sdr_loop() {}  // networking now lives in sdr_task

bool sdr_session(SdrSession *out) {
  LOCK();
  struct Unlock { ~Unlock() { UNLOCK(); } } unlock_on_return;
  // Available as soon as the session is registered, so the audio socket's TLS
  // handshake can run alongside the spectrum one instead of after it.
  if (!s_registered || s_state == SdrState::Idle || s_state == SdrState::Error) return false;
  out->host = s_host;
  out->path_prefix = s_path_prefix;
  out->uuid = s_session;
  out->port = s_port;
  out->tls = s_tls;
  out->gen = s_gen;
  return true;
}

SdrState sdr_state() { return s_state; }
String sdr_status_text() {
  LOCK();
  String s = s_status;
  UNLOCK();
  return s;
}
uint32_t sdr_max_hz() { return s_max_hz; }

void sdr_set_view(uint32_t center_hz, uint32_t span_hz) {
  LOCK();
  if (center_hz != s_want_center || span_hz != s_want_span) {
    s_want_center = center_hz;
    s_want_span = span_hz;
    s_view_dirty = true;
  }
  UNLOCK();
}

void sdr_set_rate(int divisor) {
  uint8_t d = divisor < 1 ? 1 : (divisor > 8 ? 8 : divisor);
  LOCK();
  if (d != s_want_div) {
    s_want_div = d;
    s_div_dirty = true;
  }
  UNLOCK();
}

float sdr_frames_per_s() { return s_fps; }

bool sdr_take_frame(SpectrumFrame *out) {
  LOCK();
  struct Unlock { ~Unlock() { UNLOCK(); } } unlock_on_return;
  if (!s_new_frame || !s_have_scale) return false;
  s_new_frame = false;
  // radiod sends bins in FFT order (DC .. +Nyquist, then -Nyquist .. DC).
  // Rotate by half so index 0 is the lowest frequency, as the web and Python
  // clients do: unwrapped = raw[N/2:] + raw[:N/2].
  uint16_t half = s_bins / 2;
  memcpy(s_unwrapped, s_codes + half, s_bins - half);
  memcpy(s_unwrapped + (s_bins - half), s_codes, half);
  out->codes = s_unwrapped;
  out->bins = s_bins;
  out->ref_db = s_ref_db;
  out->step_db = s_step_db;
  out->center_hz = s_frame_center;
  out->bin_bw_hz = s_bin_bw;
  return true;
}
