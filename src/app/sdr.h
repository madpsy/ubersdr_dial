#pragma once
#include <Arduino.h>

// UberSDR spectrum client: POST /connection, then /ws/user-spectrum with the
// v2 binary protocol (binary8 codes + change-mask deltas).

enum class SdrState { Idle, Connecting, Connected, Error };

struct SpectrumFrame {
  const uint8_t *codes;  // quantised bins, dB = ref + code * step
  uint16_t bins;
  float ref_db;
  float step_db;
  uint64_t center_hz;  // centre the frame was computed at
  double bin_bw_hz;
};

void sdr_init();  // starts the network task (core 0)
void sdr_connect(const String &base_url);  // e.g. https://host/
void sdr_disconnect();
void sdr_loop();  // no-op: kept for compatibility

SdrState sdr_state();
String sdr_status_text();

// Desired view. Sent to the server rate-limited; latest value wins.
void sdr_set_view(uint32_t center_hz, uint32_t span_hz);

// Server poll divisor, as v2's set_rate: 1 = full rate, 2 = half .. 8.
void sdr_set_rate(int divisor);
float sdr_frames_per_s();  // measured over the last 10 s (0 until known)

// Receiver coverage, known after the first config message (0 until then).
uint32_t sdr_max_hz();

// The registered session, for companion sockets (audio) that must share the
// same user_session_id. gen changes every time a fresh session is created.
struct SdrSession {
  String host, path_prefix, uuid;
  uint16_t port;
  bool tls;
  uint32_t gen;
};
bool sdr_session(SdrSession *out);  // true once /connection accepted the session

// Returns true once per newly decoded frame and fills *out.
bool sdr_take_frame(SpectrumFrame *out);
