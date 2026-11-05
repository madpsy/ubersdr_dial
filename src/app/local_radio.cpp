#include "local_radio.h"

#include <math.h>

#include "../hw/led_ring.h"
#include "../ui/theme.h"
#include "audio.h"
#include "saver.h"
#include "sdr.h"
#include "settings.h"

// Demodulator modes, default passbands and filter limits, as the v2 web UI
// (static/v2/src/radio/constants.js MODES + bandwidthLimits). side: 'u' upper
// sideband, 'l' lower, 'b' both. max_w = widest filter in Hz. This order is
// Settings::bw_hz's index; `v2` is the mode's index in kV2Modes.
struct ModeDef { const char *id; int lo, hi; char side; int max_w; uint8_t v2; };
static const ModeDef kModes[] = {
    {"lsb", -2700, -50, 'l', 5950, 0}, {"usb", 50, 2700, 'u', 5950, 1},   {"cwl", -200, 200, 'b', 1000, 6},
    {"cwu", -200, 200, 'b', 1000, 7},  {"am", -5000, 5000, 'b', 12000, 2}, {"sam", -5000, 5000, 'b', 12000, 3},
    {"nfm", -5000, 5000, 'b', 12000, 4}, {"fm", -8000, 8000, 'b', 16000, 5},
};
static constexpr int kNumModes = sizeof(kModes) / sizeof(kModes[0]);
static_assert(kNumModes <= 8, "Settings::bw_hz holds 8 modes");
static constexpr int kCycleModes = 8;  // v2's mode cycle: everything but IQ
static constexpr int BW_MIN = 100;
static constexpr int SQL_FLOOR = -10, SQL_MAX = 46;  // v2: the floor means "off"

// Zoom ladder (Hz). 0 = receiver's full span.
static const uint32_t kSpans[] = {0, 10000000, 5000000, 2000000, 1000000, 500000, 200000,
                                  100000, 50000, 20000, 10000};
static constexpr int kNumSpans = sizeof(kSpans) / sizeof(kSpans[0]);

static bool s_running = false;
// s_tune is the receive frequency; s_center the middle of the view. The view
// follows the tuned frequency unless panned away.
static uint32_t s_tune, s_center, s_span;
static int s_zoom_idx = 0;
static bool s_follow = true;
static uint32_t s_band_mem[16];
static uint32_t s_audio_gen = 0, s_last_max = 0;
static bool s_saved = true;
static uint32_t s_changed_at = 0;
static int s_wf_rate = 5;  // lines a second the screen wants
static uint32_t s_last_line = 0;
static int s_sql_last = 0;  // threshold to restore when squelch is toggled back on
static bool s_sql_last_ok = false;
static float s_snr_hist[5];  // for auto squelch, as v2's snrHistory
static int s_snr_n = 0;
static uint32_t s_snr_at = 0;
static DialState s_state;
static float s_snr_pub = NAN, s_dbfs_pub = NAN;
static uint32_t s_snr_pub_at = 0;

static bool locked() { return g_settings.tune_locked; }
static uint32_t max_hz() {
  uint32_t m = sdr_max_hz();
  return m ? m : 30000000;
}
static uint32_t span_for(int idx) {
  uint32_t full = max_hz();
  uint32_t s = kSpans[idx] ? kSpans[idx] : full;
  return s > full ? full : s;
}
static int cur_mode_idx() {
  const String &m = g_settings.mode;
  for (int i = 0; i < kNumModes; i++)
    if (m == kModes[i].id) return i;
  return s_tune < 10000000 ? 0 : 1;  // auto: LSB below 10 MHz, USB above
}
static const ModeDef &cur_mode() { return kModes[cur_mode_idx()]; }
static int cur_width() {
  int w = g_settings.bw_hz[cur_mode_idx()];
  return w ? w : cur_mode().hi - cur_mode().lo;
}
static void cur_edges(int *lo, int *hi) {
  const ModeDef &m = cur_mode();
  int w = cur_width();
  if (m.side == 'u') {
    *lo = m.lo;
    *hi = m.lo + w;
  } else if (m.side == 'l') {
    *hi = m.hi;
    *lo = m.hi - w;
  } else {
    *lo = -w / 2;
    *hi = w / 2;
  }
}
static int bw_step() { return cur_mode().max_w > 6000 ? 500 : 50; }
static bool sql_on() { return g_settings.squelch_db > SQL_FLOOR; }

// The user's fixed step, or a "nice" 1/2/5 value around span/100.
static uint32_t tune_step() {
  if (g_settings.step_hz) return g_settings.step_hz;
  uint32_t raw = s_span / 100, p = 1;
  while (p * 10 <= raw) p *= 10;
  if (raw >= 5 * p) return 5 * p;
  if (raw >= 2 * p) return 2 * p;
  return p < 10 ? 10 : p;
}

static void touched() {
  s_saved = false;
  s_changed_at = millis();
}

static void clamp_view() {
  uint32_t mx = max_hz(), half = s_span / 2;
  if (s_center < half) s_center = half;
  if (s_center > mx - half) s_center = mx - half;
}

static int band_of(uint32_t hz) {
  for (int i = 0; i < kDialNumBands; i++)
    if (hz >= kDialBands[i].lo && hz <= kDialBands[i].hi) return i;
  return -1;
}

// Tuning, filter or view changed: place the view, tell both sockets.
static void view_changed() {
  uint32_t mx = max_hz();
  if (s_tune > mx) s_tune = mx;
  if (!s_follow) {
    // Panned away: keep the view unless the tuned frequency has left it.
    int64_t lo = (int64_t)s_center - s_span / 2;
    if ((int64_t)s_tune < lo || (int64_t)s_tune >= lo + s_span) s_follow = true;
  }
  if (s_follow) s_center = s_tune;
  clamp_view();
  sdr_set_view(s_center, s_span);
  int bi = band_of(s_tune);
  if (bi >= 0) s_band_mem[bi] = s_tune;
  int lo, hi;
  cur_edges(&lo, &hi);
  audio_tune(s_tune, cur_mode().id, lo, hi);
  touched();
}

static void set_zoom(int idx) {
  s_zoom_idx = idx < 0 ? 0 : (idx >= kNumSpans ? kNumSpans - 1 : idx);
  s_span = span_for(s_zoom_idx);
  view_changed();
}

static void set_squelch(int db) {
  g_settings.squelch_db = db <= SQL_FLOOR ? -999 : (db > SQL_MAX ? SQL_MAX : db);
  if (sql_on()) {
    s_sql_last = g_settings.squelch_db;
    s_sql_last_ok = true;
  }
  audio_set_squelch(g_settings.squelch_db);
  touched();
}

// v2's autoSquelchValue: the recent average SNR plus 3 dB headroom.
static void auto_squelch() {
  if (!s_snr_n) return;
  float sum = 0;
  for (int i = 0; i < s_snr_n; i++) sum += s_snr_hist[i];
  int v = (int)lroundf(sum / s_snr_n + 3);
  set_squelch(v <= SQL_FLOOR ? SQL_FLOOR + 1 : v);
}

static void set_mode_v2(int v2) {
  if (locked()) return;
  for (int i = 0; i < kNumModes; i++)
    if (kModes[i].v2 == v2) {
      g_settings.mode = kModes[i].id;
      view_changed();
      return;
    }
}

static void go_band(int i) {
  if (locked() || i < 0 || i >= kDialNumBands) return;
  const DialBand &b = kDialBands[i];
  s_tune = s_band_mem[i] ? s_band_mem[i] : b.start;
  // Zoom so the whole band fits (smallest ladder span that covers it).
  uint32_t need = (b.hi - b.lo) * 23 / 20;
  int z = 0;
  for (int k = 0; k < kNumSpans; k++)
    if (span_for(k) >= need) z = k;
  s_follow = true;
  set_zoom(z);
}

static void apply_volume() { audio_set_volume(g_settings.volume, g_settings.muted); }

static void build_state() {
  DialState s;
  s.valid = s_running;
  s.freq_hz = s_tune;
  s.mode = cur_mode().v2;
  s.snr = s_snr_pub;
  s.dbfs = s_dbfs_pub;
  s.audio_up = audio_live();
  s.sql_on = sql_on();
  s.sql_db = sql_on() ? (float)g_settings.squelch_db : NAN;
  s.sql_open = !sql_on() || (!isnan(s.snr) && s.snr >= g_settings.squelch_db);
  s.muted = g_settings.muted;
  s.locked = locked();
  s.volume = g_settings.volume;
  int lo, hi;
  cur_edges(&lo, &hi);
  s.bw_lo = lo;
  s.bw_hi = hi;
  s.step_hz = tune_step();
  s.updated_ms = s_state.updated_ms;
  bool same = s.valid == s_state.valid && s.freq_hz == s_state.freq_hz && s.mode == s_state.mode &&
              (s.snr == s_state.snr || (isnan(s.snr) && isnan(s_state.snr))) &&
              (s.dbfs == s_state.dbfs || (isnan(s.dbfs) && isnan(s_state.dbfs))) && s.audio_up == s_state.audio_up &&
              s.sql_on == s_state.sql_on && s.sql_open == s_state.sql_open && s.muted == s_state.muted &&
              s.locked == s_state.locked && s.volume == s_state.volume && s.bw_lo == s_state.bw_lo &&
              s.bw_hi == s_state.bw_hi && s.step_hz == s_state.step_hz &&
              (s.sql_db == s_state.sql_db || (isnan(s.sql_db) && isnan(s_state.sql_db)));
  if (!same) {
    s.updated_ms = millis() | 1;  // never 0, which the screen starts from
    if (s.updated_ms == s_state.updated_ms) s.updated_ms += 2;
    s_state = s;
  }
}

// ---------------------------------------------------------------- link
static void lr_send(uint8_t code, int8_t v) {
  if (!s_running) return;
  switch (code) {
    case DIAL_PAGE_TUNE: {
      if (locked() || !v) break;
      int64_t step = tune_step();
      int64_t c = ((int64_t)s_tune + step / 2) / step * step + (int64_t)v * step;
      s_tune = c < 0 ? 0 : (uint32_t)c;
      view_changed();
      break;
    }
    case DIAL_PAGE_VOL: {
      int nv = (int)g_settings.volume + v * 2;
      g_settings.volume = nv < 0 ? 0 : (nv > 100 ? 100 : nv);
      if (g_settings.muted && v > 0) g_settings.muted = false;
      apply_volume();
      touched();
      break;
    }
    case DIAL_PAGE_ZOOM: set_zoom(s_zoom_idx + v); break;
    case DIAL_PAGE_SQL: set_squelch((sql_on() ? g_settings.squelch_db : SQL_FLOOR) + v); break;
    case DIAL_PAGE_BW: {
      int w = cur_width() + v * bw_step(), mx = cur_mode().max_w;
      g_settings.bw_hz[cur_mode_idx()] = w < BW_MIN ? BW_MIN : (w > mx ? mx : w);
      view_changed();
      break;
    }
    case DIAL_PAGE_STEP: {
      // From the step in use (auto included) to its neighbour in v2's list.
      uint32_t cur = tune_step();
      int k = 0;
      for (int i = 0; i < kDialNumSteps; i++)
        if (kDialSteps[i] <= cur) k = i;
      k += v;
      k = k < 0 ? 0 : (k >= kDialNumSteps ? kDialNumSteps - 1 : k);
      g_settings.step_hz = kDialSteps[k];
      touched();
      break;
    }
    case DIAL_MODE_TAP:
    case DIAL_MODE_HOLD: {
      int cur = cur_mode().v2, dir = code == DIAL_MODE_TAP ? 1 : -1;
      set_mode_v2(((cur < kCycleModes ? cur : 0) + dir + kCycleModes) % kCycleModes);
      break;
    }
    case DIAL_FREQ_TAP:
    case DIAL_CENTER_TAP:
      s_follow = true;
      view_changed();
      break;
    case DIAL_MUTE_TAP:
      g_settings.muted = !g_settings.muted;
      apply_volume();
      touched();
      break;
    case DIAL_LOCK:
      g_settings.tune_locked = !g_settings.tune_locked;
      settings_save();
      break;
    case DIAL_SQL_TOGGLE:
      if (sql_on()) set_squelch(-999);
      else if (s_sql_last_ok) set_squelch(s_sql_last);
      else auto_squelch();
      break;
    case DIAL_SQL_AUTO: auto_squelch(); break;
    case DIAL_ZOOM_IN: set_zoom(s_zoom_idx + 1); break;
    case DIAL_ZOOM_OUT: set_zoom(s_zoom_idx - 1); break;
    case DIAL_ZOOM_RESET: set_zoom(0); break;
    case DIAL_WF_RATE:
      s_wf_rate = v < 1 ? 1 : (v > 30 ? 30 : v);
      break;
    case DIAL_PAN: {
      // The picture moved right by v of 360 px: the view moves left as much.
      int64_t c = (int64_t)s_center - (int64_t)v * s_span / 360;
      s_center = c < 0 ? 0 : (uint32_t)c;
      s_follow = false;
      clamp_view();
      sdr_set_view(s_center, s_span);
      touched();
      break;
    }
    case DIAL_TAP_TUNE: {
      if (locked()) break;
      int64_t f = (int64_t)s_center + (int64_t)v * (int64_t)s_span / 256;
      int64_t step = tune_step();
      if (f < 0) f = 0;
      s_tune = (uint32_t)((f + step / 2) / step * step);
      view_changed();
      break;
    }
    default:
      if (code >= DIAL_MODE0 && code < DIAL_MODE0 + kCycleModes) set_mode_v2(code - DIAL_MODE0);
      else if (code >= DIAL_BAND0 && code < DIAL_BAND0 + kDialNumBands) go_band(code - DIAL_BAND0);
      else if (code == DIAL_STEP_AUTO) {
        g_settings.step_hz = 0;
        touched();
      } else if (code >= DIAL_STEP0 && code < DIAL_STEP0 + kDialNumSteps) {
        g_settings.step_hz = kDialSteps[code - DIAL_STEP0];
        touched();
      }
      break;
  }
  build_state();
}

// Fewest spectrum frames that still feed the waterfall at the rate the screen
// wants: v2's poll divisor, from the measured full rate.
static int s_div = 1;
static void pick_divisor() {
  int &div = s_div;
  static uint32_t at = 0;
  static int settle = 0;  // windows to skip after a change (they mix both rates)
  if (millis() - at < 10000) return;
  at = millis();
  float fps = sdr_frames_per_s();
  if (fps <= 0) return;
  if (settle > 0) {
    settle--;
    return;
  }
  float base = fps * div;
  int d = (int)floorf(base / s_wf_rate);
  d = d < 1 ? 1 : (d > 8 ? 8 : d);
  if (d != div) {
    Serial.printf("[radio] full rate ~%.1f frames/s, want %d lines/s: divisor %d\n", base, s_wf_rate, d);
    div = d;
    sdr_set_rate(d);
    settle = 1;
  }
}

// The spectrum socket is open only while something draws it: the waterfall is
// on, and the screen saver isn't pausing it (screen off or "Spectrum off").
static bool want_spectrum() { return g_settings.ctrl_bg != BG_OFF && !saver_spectrum_paused(); }

static void lr_loop() {
  if (!s_running) return;
  uint32_t now = millis();
  // Receiver coverage arrives with the first config: re-apply zoom limits.
  if (sdr_max_hz() != s_last_max) {
    s_last_max = sdr_max_hz();
    s_span = span_for(s_zoom_idx);
    view_changed();
  }
  // Audio follows the spectrum session (same user_session_id).
  bool spec = want_spectrum();
  sdr_set_spectrum(spec);  // change-only inside
  SdrSession ss{};  // gen 0 until there is one
  extern bool g_led_disabled;  // debug quiet mode also keeps audio off
  if (sdr_session(&ss) && !g_led_disabled && (ss.gen != s_audio_gen || !audio_has_session())) {
    s_audio_gen = ss.gen;
    int lo, hi;
    cur_edges(&lo, &hi);
    audio_tune(s_tune, cur_mode().id, lo, hi);
    audio_set_squelch(g_settings.squelch_db);
    audio_set_session(ss.host, ss.port, ss.tls, ss.path_prefix, ss.uuid, ss.gen);
  }
  // Without the spectrum socket nothing else notices a session the server has
  // dropped (its own watchdog restarts on 10 s without frames): audio silent
  // for 20 s on a session -> register a fresh one.
  {
    static uint32_t ok_at = 0, ok_gen = 0;
    if (audio_live() || ss.gen != ok_gen || g_led_disabled || spec) {
      ok_at = now;
      ok_gen = ss.gen;
    } else if (now - ok_at > 20000) {
      Serial.println("[radio] no audio for 20 s without a spectrum socket: new session");
      ok_at = now;
      sdr_restart_session();
    }
  }
  // SNR for the audio coming out of the speaker now, published ~20x a second.
  if (now - s_snr_pub_at >= 50) {
    s_snr_pub_at = now;
    s_snr_pub = audio_snr_db_playing();
    s_dbfs_pub = audio_dbfs_playing();
  }
  if (now - s_snr_at >= 200 && !isnan(s_snr_pub)) {
    s_snr_at = now;
    if (s_snr_n < 5) s_snr_hist[s_snr_n++] = s_snr_pub;
    else {
      memmove(s_snr_hist, s_snr_hist + 1, 4 * sizeof(float));
      s_snr_hist[4] = s_snr_pub;
    }
  }
  pick_divisor();
  // Persist the view a few seconds after the user stops changing things.
  if (!s_saved && now - s_changed_at > 5000) {
    g_settings.freq_hz = s_tune;
    g_settings.span_hz = s_span;
    settings_save_view();
    s_saved = true;
  }
  build_state();
}

static bool lr_connected() { return s_running && sdr_state() == SdrState::Connected; }
static DialState lr_state() { return s_state; }

// Each screen column of the *requested* view takes the strongest bin under
// it, so tuning feels immediate even before the server has re-centred.
static bool lr_take_line(float *db, int *n, uint32_t *center_hz, uint32_t *span_hz) {
  SpectrumFrame f;
  if (!s_running || !sdr_take_frame(&f) || !f.bins || f.bin_bw_hz <= 0) return false;
  uint32_t now = millis();
  if (now - s_last_line + 15 < 1000u / s_wf_rate) return false;  // faster than wanted
  s_last_line = now;
  const int W = DIAL_LINE_MAX;
  double frame_lo = (double)f.center_hz - f.bins * f.bin_bw_hz / 2;
  double view_lo = (double)s_center - s_span / 2.0, per = (double)s_span / W;
  double bins_per_col = per / f.bin_bw_hz;
  static float raw[DIAL_LINE_MAX];
  for (int x = 0; x < W; x++) {
    double b0 = (view_lo + x * per - frame_lo) / f.bin_bw_hz;
    if (bins_per_col < 1) {
      // Zoomed in past the FFT: interpolate between bin centres rather than
      // repeating each bin as a block.
      double c = b0 + bins_per_col / 2 - 0.5;
      int i0 = (int)floor(c);
      if (i0 < 0 || i0 + 1 >= f.bins) {
        raw[x] = -200;
        continue;
      }
      double t = c - i0;
      raw[x] = f.ref_db + (f.codes[i0] * (1 - t) + f.codes[i0 + 1] * t) * f.step_db;
      continue;
    }
    // Several bins per column: the strongest, so narrow signals don't vanish.
    int i0 = (int)floor(b0), i1 = (int)ceil(b0 + bins_per_col);
    if (i1 <= i0) i1 = i0 + 1;
    if (i1 <= 0 || i0 >= f.bins) {
      raw[x] = -200;
      continue;
    }
    if (i0 < 0) i0 = 0;
    if (i1 > f.bins) i1 = f.bins;
    uint8_t m = 0;
    for (int i = i0; i < i1; i++) m = f.codes[i] > m ? f.codes[i] : m;
    raw[x] = f.ref_db + m * f.step_db;
  }
  // Light 1-2-1 smoothing across columns: the raw max-of-bins is grainy (noise
  // peaks pixel to pixel), which showed through the glassy UI as speckle.
  for (int x = 0; x < W; x++) {
    float c = raw[x];
    if (c <= -200) {
      db[x] = -200;
      continue;
    }
    float l = x > 0 && raw[x - 1] > -200 ? raw[x - 1] : c, r = x + 1 < W && raw[x + 1] > -200 ? raw[x + 1] : c;
    db[x] = (l + 2 * c + r) * 0.25f;
  }
  *n = W;
  *center_hz = s_center;
  *span_hz = s_span;
  return true;
}

static void lr_status(String *text, uint32_t *dot) {
  SdrState st = sdr_state();
  String who = g_settings.inst_call.length() ? g_settings.inst_call : g_settings.inst_name;
  *text = String(LV_SYMBOL_LEFT " ") + who;
  if (st == SdrState::Connected) {
    *dot = audio_live() ? COL_OK : COL_WARN;
  } else {
    *text += "  " + sdr_status_text();
    *dot = st == SdrState::Error ? COL_BAD : COL_WARN;
  }
}

static void lr_zoom_range(uint32_t *min_hz, uint32_t *max_hz) {
  *min_hz = span_for(kNumSpans - 1);
  *max_hz = span_for(0);
}

const DialLink kLocalLink = {true, lr_loop, lr_connected, lr_state, lr_send, lr_take_line, lr_status, lr_zoom_range};

void local_radio_start() {
  s_tune = s_center = g_settings.freq_hz;
  s_follow = true;
  s_span = g_settings.span_hz;
  int best = 0;
  for (int i = 0; i < kNumSpans; i++)
    if (kSpans[i] && labs((long)kSpans[i] - (long)s_span) < labs((long)kSpans[best] - (long)s_span)) best = i;
  s_zoom_idx = best;
  s_span = span_for(best);
  s_last_max = sdr_max_hz();
  s_audio_gen = 0;
  s_snr_n = 0;
  s_snr_pub = s_dbfs_pub = NAN;
  s_state = DialState();
  s_running = true;
  apply_volume();
  view_changed();
  s_saved = true;
  s_div = 1;
  sdr_set_rate(1);  // measure the full rate first; pick_divisor() slows it
  // No spectrum socket while the waterfall is off: audio needs only the session.
  sdr_set_spectrum(want_spectrum());
  sdr_connect(g_settings.inst_url);
  build_state();
}

void local_radio_stop() {
  if (!s_running) return;
  s_running = false;
  audio_stop();
  led_ring_off();
  sdr_disconnect();
  if (!s_saved) {
    g_settings.freq_hz = s_tune;
    g_settings.span_hz = s_span;
    settings_save_view();
    s_saved = true;
  }
}
