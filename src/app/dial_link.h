#pragma once
#include <Arduino.h>

// What the dial screen talks to. The screen is the same in both modes; only
// the far end differs:
//   Controller mode (Bluetooth or USB): the UberSDR v2 page does the work and
//     sends its state and waterfall lines back (ctrl_link).
//   Wi-Fi (receiver mode): the dial is the radio itself (local_radio), and
//     answers the same codes the same way the page would.
// Codes are the BLE protocol's (docs/PROTOCOL.md, controls/dial.js).

// UberSDR's own fallback coverage, for a receiver whose range isn't known.
static constexpr uint32_t DIAL_DEFAULT_MIN_HZ = 10000, DIAL_DEFAULT_MAX_HZ = 30000000;

// Receiver state shown by the screen.
struct DialState {
  bool valid = false;  // there is a receiver to show (else the waiting panel)
  uint32_t freq_hz = 0;
  uint8_t mode = 255;  // index into the v2 mode table (kV2Modes)
  float snr = NAN;     // NAN = no reading
  float dbfs = NAN;    // baseband power, dBFS (what v2's S-meter shows); NAN = no reading
  bool audio_up = false, sql_on = false, sql_open = false, muted = false, locked = false;
  uint8_t volume = 0;  // 0..100
  float sql_db = NAN;  // threshold; NAN = off
  int16_t bw_lo = 0, bw_hi = 0;
  uint32_t step_hz = 0;
  // The receiver's tuning range (/api/description's tuning_range); UberSDR's
  // default 10 kHz - 30 MHz when it isn't known (an older page, no description).
  uint32_t min_hz = DIAL_DEFAULT_MIN_HZ, max_hz = DIAL_DEFAULT_MAX_HZ;
  uint32_t updated_ms = 0;  // changes whenever anything above does
};

// v2 MODES order (static/v2/src/radio/constants.js), as sent in the state.
extern const char *const kV2Modes[];
extern const int kV2NumModes;

// Input codes (dial -> receiver)
enum : uint8_t {
  DIAL_PAGE_TUNE = 1, DIAL_PAGE_VOL = 2, DIAL_PAGE_ZOOM = 3, DIAL_PAGE_SQL = 4, DIAL_PAGE_BW = 5, DIAL_PAGE_STEP = 6,
  DIAL_MODE_TAP = 16, DIAL_MODE_HOLD = 17, DIAL_FREQ_TAP = 18, DIAL_FREQ_HOLD = 19,
  DIAL_MUTE_TAP = 20, DIAL_MUTE_HOLD = 21, DIAL_CENTER_TAP = 22,
  DIAL_MODE0 = 32, DIAL_BAND0 = 48, DIAL_STEP0 = 64,
  DIAL_LOCK = 80, DIAL_SQL_TOGGLE = 81, DIAL_SQL_AUTO = 82, DIAL_ZOOM_IN = 83, DIAL_ZOOM_OUT = 84,
  DIAL_ZOOM_RESET = 85,
  DIAL_WF_RATE = 96,   // value: waterfall lines a second wanted
  DIAL_PAN = 97,       // value: drag, px of 360 (content moved right = +)
  DIAL_TAP_TUNE = 98,  // value: -127..127 across the view
  DIAL_STEP_AUTO = 99, // receiver mode only: tuning step back to automatic (from the span)
};

// v2's TUNING_STEPS (index = code - DIAL_STEP0) and band list (code - DIAL_BAND0).
extern const uint32_t kDialSteps[];
extern const int kDialNumSteps;
struct DialBand { const char *name; uint32_t lo, hi, start; };
extern const DialBand kDialBands[];
extern const int kDialNumBands;

// The widest a waterfall line can be (one point per screen column).
static constexpr int DIAL_LINE_MAX = 360;

struct DialLink {
  bool local;  // the dial is the receiver (Wi-Fi) rather than a remote for one
  void (*loop)();
  bool (*connected)();  // the link itself is up (a page connected / socket live)
  DialState (*state)();
  void (*send)(uint8_t code, int8_t value);
  // A new waterfall line: dB per point across centre +/- span/2 (<= -200 =
  // nothing there). Returns true once per line.
  bool (*take_line)(float *db, int *n, uint32_t *center_hz, uint32_t *span_hz);
  // Header: what we're connected to, and the status dot's colour.
  void (*status)(String *text, uint32_t *dot_col);
  // Narrowest and widest waterfall span the zoom controls reach (for the rim).
  void (*zoom_range)(uint32_t *min_hz, uint32_t *max_hz);
};

extern const DialLink kCtrlLink;   // controller mode, Bluetooth or USB (ctrl_link.cpp)
extern const DialLink kLocalLink;  // receiver mode (local_radio.cpp)
