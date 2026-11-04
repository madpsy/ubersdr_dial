#include "audio.h"

#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <freertos/stream_buffer.h>
#include <libopus/opus.h>
#include <esp_heap_caps.h>
#include <math.h>

#include <WiFi.h>

#include "../hw/board.h"

// Wire format (protocol version 3, as clients/electron/monitor/minimal-radio.js):
//   [timestamp u64][sampleRate u32][channels u8][basebandPower f32][noise f32][opus...]
// little-endian, 21-byte header; noise is passband noise power on v3, so
// SNR = basebandPower - noise. Opus is decoded at 48 kHz to match I2S.
static constexpr int kHdr = 21;
static constexpr int kRate = 48000;
static constexpr int kMaxFrame = 5760;  // 120 ms @ 48 kHz
static constexpr size_t kRingBytes = kRate * 2 * 5 / 2;   // 2.5 s of mono int16 (PSRAM)
// Adaptive jitter buffer. The target starts at 300 ms; a network gap raises it
// to ~1.25x that gap (max 1.5 s) so the next blip in a Wi-Fi bad patch is
// covered, and it decays back after 30 s without gaps. Prebuffer = target,
// gentle 1% catch-up above target + 200 ms, hard jump above target + 600 ms.
static constexpr uint32_t kTargetMinMs = 300, kTargetMaxMs = 1500;
static volatile uint32_t s_target_ms = kTargetMinMs;
static volatile uint32_t s_last_gap_at = 0;
static inline size_t ms_bytes(uint32_t ms) { return (size_t)kRate * 2 * ms / 1000; }
static constexpr int kChunk = 480;                           // 10 ms

static SemaphoreHandle_t s_mx;
static StreamBufferHandle_t s_ring;
static StaticStreamBuffer_t s_ring_struct;
static OpusDecoder *s_dec;
static WebSocketsClient s_ws;

// Requested state (written by the UI under s_mx)
static String s_host, s_prefix, s_uuid;
static uint16_t s_port;
static bool s_tls;
static uint32_t s_want_gen = 0;
static uint32_t s_freq = 14200000;
static char s_mode[8] = "usb";
static int s_lo = 50, s_hi = 2850;
static bool s_tune_dirty = true;
static float s_squelch = -999;     // audio gate min SNR, -999 = off
static bool s_squelch_dirty = true;

static volatile int32_t s_gain_q12 = 4096;
static volatile float s_snr = NAN;

// SNR readings stamped with when their audio reaches the speaker, so meters
// can show the reading for what you're hearing rather than what just arrived
// (the jitter buffer holds 0.3-1.5 s). Written by the net task, read by the UI.
struct SnrStamp { uint32_t play_at; float snr; };
static constexpr int kSnrQ = 256;  // > 1.5 s of 20 ms frames + margin
static SnrStamp s_snr_q[kSnrQ];
static volatile uint32_t s_snr_head = 0;  // total pushed
static constexpr uint32_t kDacLatencyMs = 6 * 240 * 1000 / 48000;  // I2S DMA queue
static volatile uint32_t s_last_audio = 0;
static bool s_connected = false;
static uint32_t s_connected_at = 0;
static bool s_reset_decoder = false;

// Diagnostics (logged every 5 s)
static volatile uint32_t st_frames = 0, st_decode_err = 0, st_dropped = 0, st_underruns = 0, st_trims = 0;
static volatile uint32_t st_decode_us = 0, st_decode_max_us = 0;
static volatile uint32_t s_tone_until = 0;  // test tone (debug)
static volatile int32_t st_peak_dec = 0, st_peak_out = 0;  // pre-gain decoder peak, peak sent to I2S

// ---------------------------------------------------------------- network
static void send_tune() {
  char msg[128];
  xSemaphoreTake(s_mx, portMAX_DELAY);
  snprintf(msg, sizeof(msg),
           "{\"type\":\"tune\",\"frequency\":%u,\"mode\":\"%s\",\"bandwidthLow\":%d,\"bandwidthHigh\":%d}",
           (unsigned)s_freq, s_mode, s_lo, s_hi);
  s_tune_dirty = false;
  xSemaphoreGive(s_mx);
  s_ws.sendTXT(msg);
}

static void on_audio(const uint8_t *p, size_t len) {
  if (len <= kHdr) return;
  float power, noise;
  memcpy(&power, p + 13, 4);
  memcpy(&noise, p + 17, 4);
  s_snr = (power > -900 && noise > -900) ? power - noise : NAN;
  {
    // This frame plays after everything already queued (plus the DAC's DMA).
    uint32_t queued_ms = xStreamBufferBytesAvailable(s_ring) / (kRate * 2 / 1000);
    SnrStamp &st = s_snr_q[s_snr_head % kSnrQ];
    st.play_at = millis() + queued_ms + kDacLatencyMs;
    st.snr = s_snr;
    s_snr_head = s_snr_head + 1;
  }

  static int16_t pcm[kMaxFrame];
  if (s_reset_decoder) {
    opus_decoder_ctl(s_dec, OPUS_RESET_STATE);
    s_reset_decoder = false;
  }
  uint32_t t0 = micros();
  int n = opus_decode(s_dec, p + kHdr, len - kHdr, pcm, kMaxFrame, 0);
  uint32_t dt = micros() - t0;
  st_decode_us += dt;
  if (dt > st_decode_max_us) st_decode_max_us = dt;
  st_frames++;
  if (n <= 0) {
    st_decode_err++;
    return;
  }
  int32_t g = s_gain_q12;
  for (int i = 0; i < n; i++) {
    int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (a > st_peak_dec) st_peak_dec = a;
    int32_t v = ((int32_t)pcm[i] * g) >> 12;
    pcm[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
  }
  // Whole frames only: a partial write can be an odd byte count, which would
  // misalign every following int16 sample into full-scale white noise.
  uint32_t now_ms = millis();
  if (s_last_audio && now_ms - s_last_audio > 300) {
    uint32_t gap = now_ms - s_last_audio;
    uint32_t want = gap * 5 / 4;
    if (want > kTargetMaxMs) want = kTargetMaxMs;
    if (want > s_target_ms) s_target_ms = want;
    s_last_gap_at = now_ms;
    Serial.printf("[gap] audio %lu ms at t=%lu, buffer target now %lu ms\n", (unsigned long)gap,
                  (unsigned long)now_ms, (unsigned long)s_target_ms);
  }
  if (xStreamBufferSpacesAvailable(s_ring) >= (size_t)n * 2) xStreamBufferSend(s_ring, pcm, n * 2, 0);
  else st_dropped++;
  s_last_audio = millis();
}

static void ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[audio] connected, internal heap %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      s_connected = true;
      s_connected_at = millis();
      s_reset_decoder = true;
      send_tune();
      s_squelch_dirty = true;  // a fresh session starts with the gate disabled
      break;
    case WStype_DISCONNECTED:
      if (s_connected) Serial.println("[audio] disconnected");
      s_connected = false;
      s_snr = NAN;
      break;
    case WStype_BIN:
      on_audio(payload, length);
      break;
    case WStype_TEXT:
      if (length > 9 && strstr((const char *)payload, "\"error\""))
        Serial.printf("[audio] server: %.*s\n", (int)length, (const char *)payload);
      break;
    default:
      break;
  }
}

static void net_task(void *) {
  uint32_t cur_gen = 0, last_tune = 0, last_ping = 0, last_report = 0;
  bool started = false;
  for (;;) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t want = s_want_gen;
    String host = s_host, prefix = s_prefix, uuid = s_uuid;
    uint16_t port = s_port;
    bool tls = s_tls, dirty = s_tune_dirty;
    uint32_t freq = s_freq;
    String mode = s_mode;
    xSemaphoreGive(s_mx);

    if (want != cur_gen) {
      if (started) s_ws.disconnect();
      started = false;
      s_connected = false;
      s_snr = NAN;
      cur_gen = want;
      if (want && uuid.length()) {
        String path = prefix + "ws?frequency=" + String(freq) + "&mode=" + mode + "&user_session_id=" + uuid +
                      "&format=opus&version=3";
        if (tls) s_ws.beginSSL(host.c_str(), port, path.c_str());
        else s_ws.begin(host.c_str(), port, path.c_str());
        s_ws.onEvent(ws_event);
        s_ws.setReconnectInterval(2000);
        started = true;
      }
    }
    // Watchdog: connected but no audio for 2.5 s -> reopen just the audio
    // socket. After Wi-Fi loss the stock lwIP retransmit timer starts at 3 s and
    // doubles; a fresh connection starts streaming immediately instead.
    if (started && s_connected) {
      uint32_t last = s_last_audio > s_connected_at ? s_last_audio : s_connected_at;
      if (millis() - last > 2500) {
        Serial.println("[audio] no audio for 2.5 s, reconnecting audio socket");
        cur_gen = 0;  // forces a fresh begin on the next pass
        continue;
      }
    }
    if (started) {
      uint32_t t0 = millis();
      s_ws.loop();
      uint32_t dt = millis() - t0;
      if (dt > 200) Serial.printf("[gap] audio ws.loop blocked %lu ms at t=%lu\n", (unsigned long)dt, (unsigned long)millis());
      uint32_t now = millis();
      if (s_connected && dirty && now - last_tune >= 100) {
        send_tune();
        last_tune = now;
      }
      if (s_connected && s_squelch_dirty) {
        char m[64];
        xSemaphoreTake(s_mx, portMAX_DELAY);
        snprintf(m, sizeof(m), "{\"type\":\"set_audio_gate\",\"min_snr\":%.1f}", s_squelch);
        s_squelch_dirty = false;
        xSemaphoreGive(s_mx);
        s_ws.sendTXT(m);
      }
      if (s_connected && now - last_ping > 20000) {
        s_ws.sendTXT("{\"type\":\"ping\"}");
        last_ping = now;
      }
    }
    if (millis() - last_report > 5000) {
      last_report = millis();
      if (s_target_ms > kTargetMinMs && millis() - s_last_gap_at > 30000)
        s_target_ms = s_target_ms - 50 < kTargetMinMs ? kTargetMinMs : s_target_ms - 50;
      uint32_t fr = st_frames;
      Serial.printf("[audio] live=%d frames=%u decErr=%u dropped=%u underruns=%u trims=%u dec avg/max=%u/%u us "
                    "buf=%u/%u ms rssi=%d peak dec/out=%d/%d gain=%d | stack free %u, internal %u (largest %u)\n",
                    audio_live(), (unsigned)fr, (unsigned)st_decode_err, (unsigned)st_dropped,
                    (unsigned)st_underruns, (unsigned)st_trims, fr ? (unsigned)(st_decode_us / fr) : 0,
                    (unsigned)st_decode_max_us, (unsigned)(xStreamBufferBytesAvailable(s_ring) / 96), (unsigned)s_target_ms, (int)WiFi.RSSI(), (int)st_peak_dec,
                    (int)st_peak_out, (int)s_gain_q12,
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr),  // bytes on ESP-IDF
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      st_frames = st_decode_err = st_dropped = st_underruns = st_trims = 0;
      st_decode_us = st_decode_max_us = 0;
      st_peak_dec = st_peak_out = 0;
    }
    vTaskDelay(1);
  }
}

// ---------------------------------------------------------------- output
static void i2s_task(void *) {
  static int16_t mono[kChunk];
  static int16_t raw[kChunk + kChunk / 100];   // +1% for gentle catch-up
  static int16_t scratch[kChunk];
  static int32_t stereo[kChunk * 2];  // 32-bit slots: BCK = 64 fs for the DAC's PLL
  bool prebuffering = true, fade_in = false;
  for (;;) {
    size_t avail = xStreamBufferBytesAvailable(s_ring);
    const uint32_t tgt = s_target_ms;
    const size_t kTarget = ms_bytes(tgt), kPrebuffer = kTarget;
    const size_t kHighWater = ms_bytes(tgt + 200), kJumpAt = ms_bytes(tgt + 600);
    // Big backlog after a long stall: jump back to the target in one go
    // rather than playing fast for seconds.
    if (avail > kJumpAt) {
      size_t drop = (avail - kTarget) & ~(size_t)1;
      while (drop) {
        size_t n = drop > sizeof(scratch) ? sizeof(scratch) : drop;
        size_t r = xStreamBufferReceive(s_ring, scratch, n, 0);
        if (!r) break;
        drop -= r;
      }
      st_trims++;
      fade_in = true;
      avail = xStreamBufferBytesAvailable(s_ring);
    }
    bool fast = avail > kHighWater;  // a little behind: resample 1% faster
    if (prebuffering && avail >= kPrebuffer) {
      prebuffering = false;
      fade_in = true;
    }
    size_t got = 0;
    if ((int32_t)(s_tone_until - millis()) > 0) {
      // Debug: quiet 1 kHz tone straight to the DAC, bypassing network + decoder.
      static uint32_t ph = 0;
      for (int i = 0; i < kChunk; i++, ph++) mono[i] = (int16_t)(1000 * sinf(2 * M_PI * 1000 * ph / kRate));
      got = sizeof(mono);
    } else if (!prebuffering) {
      if (fast) {
        got = xStreamBufferReceive(s_ring, raw, sizeof(raw), 0);
        size_t n = got / 2;
        if (n == sizeof(raw) / 2) {
          // Linear-interpolate n input samples onto kChunk outputs (inaudible
          // 1% speed-up; dropping samples instead is what crackles).
          for (int i = 0; i < kChunk; i++) {
            uint32_t pos = (uint32_t)i * ((n - 1) << 8) / (kChunk - 1);  // Q8
            uint32_t k = pos >> 8, f = pos & 0xFF;
            int32_t a = raw[k], b2 = raw[k + 1 < n ? k + 1 : k];
            mono[i] = (int16_t)(a + (((b2 - a) * (int32_t)f) >> 8));
          }
          got = sizeof(mono);
        } else {
          memcpy(mono, raw, got);
        }
      } else {
        got = xStreamBufferReceive(s_ring, mono, sizeof(mono), 0);
      }
      if (got < sizeof(mono)) {  // underrun: refill first
        prebuffering = true;
        st_underruns++;
      }
    }
    size_t ns = got / 2;
    for (int i = 0; i < kChunk; i++) {
      int32_t v = i < (int)ns ? mono[i] : 0;
      if (fade_in) v = v * i / kChunk;  // 10 ms ramp after a gap: no click
      int32_t a = v < 0 ? -v : v;
      if (a > st_peak_out) st_peak_out = a;
      stereo[2 * i] = v << 16;
      stereo[2 * i + 1] = v << 16;
    }
    if (ns) fade_in = false;
    size_t written;
    i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, portMAX_DELAY);  // paces at 48 kHz
  }
}

// ---------------------------------------------------------------- public
void audio_init() {
  s_mx = xSemaphoreCreateMutex();
  uint8_t *store = (uint8_t *)ps_malloc(kRingBytes + 1);
  s_ring = xStreamBufferCreateStatic(kRingBytes, 1, store, &s_ring_struct);

  int dsz = opus_decoder_get_size(1);
  s_dec = (OpusDecoder *)ps_malloc(dsz);
  opus_decoder_init(s_dec, kRate, 1);

  pinMode(PIN_DAC_MUTE, OUTPUT);
  digitalWrite(PIN_DAC_MUTE, HIGH);  // LOW mutes the PCM5100A

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 240;
  cfg.tx_desc_auto_clear = true;
  i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_BCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DO;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins);

  // Opus decode uses variable-length arrays on the stack: give it room.
  xTaskCreatePinnedToCore(net_task, "audio_net", 20480, nullptr, 3, nullptr, 0);  // peak use ~10 KB
  xTaskCreatePinnedToCore(i2s_task, "audio_i2s", 4096, nullptr, 5, nullptr, 0);
}

void audio_set_session(const String &host, uint16_t port, bool tls, const String &prefix, const String &uuid,
                       uint32_t gen) {
  xSemaphoreTake(s_mx, portMAX_DELAY);
  if (gen != s_want_gen) {
    s_host = host;
    s_port = port;
    s_tls = tls;
    s_prefix = prefix;
    s_uuid = uuid;
    s_want_gen = gen;
    s_tune_dirty = true;
  }
  xSemaphoreGive(s_mx);
}

void audio_stop() { audio_set_session("", 0, false, "", "", 0); }

void audio_tune(uint32_t freq, const char *mode, int lo, int hi) {
  xSemaphoreTake(s_mx, portMAX_DELAY);
  if (freq != s_freq || strcmp(mode, s_mode) || lo != s_lo || hi != s_hi) {
    s_freq = freq;
    strlcpy(s_mode, mode, sizeof(s_mode));
    s_lo = lo;
    s_hi = hi;
    s_tune_dirty = true;
  }
  xSemaphoreGive(s_mx);
}

void audio_set_squelch(float min_snr_db) {
  xSemaphoreTake(s_mx, portMAX_DELAY);
  if (min_snr_db != s_squelch) {
    s_squelch = min_snr_db;
    s_squelch_dirty = true;
  }
  xSemaphoreGive(s_mx);
}

void audio_set_volume(uint8_t vol, bool muted) {
  // Perceptual-ish curve; 100% = 2x digital gain (SSB audio arrives quiet).
  float g = muted ? 0 : (vol / 100.0f) * (vol / 100.0f) * 2.0f;
  s_gain_q12 = (int32_t)(g * 4096);
}

void audio_test_tone(uint32_t ms) { s_tone_until = millis() + ms; }

// Debug: stop/start the I2S peripheral (clocks off) to test RF interference.
void audio_i2s_enable(bool on) {
  if (on) i2s_start(I2S_NUM_0);
  else i2s_stop(I2S_NUM_0);
}

bool audio_has_session() { return s_want_gen != 0; }

bool audio_live() { return s_connected && millis() - s_last_audio < 1000; }
float audio_snr_db() { return audio_live() ? (float)s_snr : NAN; }

float audio_snr_db_playing() {
  if (!audio_live()) return NAN;
  // Newest reading whose audio has already started playing.
  uint32_t now = millis(), head = s_snr_head;
  for (uint32_t k = 0; k < kSnrQ && k < head; k++) {
    const SnrStamp &st = s_snr_q[(head - 1 - k) % kSnrQ];
    if ((int32_t)(now - st.play_at) >= 0) return st.snr;
  }
  return NAN;  // nothing audible yet (still prebuffering)
}
