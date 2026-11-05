// The dial screen, in both modes. It sends codes to a DialLink and shows the
// state and waterfall that come back: from the UberSDR v2 page over Bluetooth
// (controller mode) or from the dial's own receiver (local_radio, Wi-Fi).
//
//   knob        : sent to the page on the current page (TUNE/VOL/ZOOM/SQL/BW)
//   tap chip    : next knob page          hold chip: menu
//   tap/hold mode, frequency, mute: sent to the page (mapped in SDR Control)
#include <math.h>
#include <stdarg.h>

#include <src/core/lv_obj_event_private.h>  // lv_hit_test_info_t (LV_EVENT_HIT_TEST)

#include "../app/ctrl_link.h"
#include "../app/dial_link.h"
#include "../app/local_radio.h"
#include "../app/sdr.h"
#include "../app/saver.h"
#include "../app/settings.h"
#include "../app/wifi_setup.h"
#include "../hw/display.h"
#include "../hw/haptic.h"
#include "../hw/input.h"
#include "../hw/led_ring.h"
#include "battery_icon.h"
#include "confirm.h"
#include "device_menu.h"
#include "menu.h"
#include "padlock.h"
#include "rim.h"
#include "fonts/fonts.h"
#include "theme.h"
#include "ui.h"

void ui_load(lv_obj_t *scr);

// LVGL redraws a label (or anything restyled) whenever it's set, even to the
// same value — and state arrives 10x a second. Only touch what changed.
static void set_text(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}
static void set_textf(lv_obj_t *l, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  set_text(l, buf);
}
static void set_tcol(lv_obj_t *o, uint32_t hex) {
  lv_color_t c = lv_color_hex(hex);
  if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}

enum Page { P_TUNE, P_VOL, P_ZOOM, P_SQL, P_BW, P_STEP, P_COUNT };
static const uint8_t kPageCode[P_COUNT] = {DIAL_PAGE_TUNE, DIAL_PAGE_VOL, DIAL_PAGE_ZOOM,
                                           DIAL_PAGE_SQL,  DIAL_PAGE_BW,  DIAL_PAGE_STEP};
static const uint32_t kPageCol[P_COUNT] = {COL_ACCENT, COL_OK, COL_ACCENT2, COL_WARN, 0xf472b6, 0x60a5fa};
static constexpr int SQL_LO = -11, SQL_HI = 46;  // v2 squelch slider range

static lv_obj_t *s_scr, *s_dot, *s_status, *s_freq, *s_info, *s_snr, *s_snr_cap, *s_sql, *s_mode_lbl, *s_mute_lbl,
    *s_chip, *s_chip_lbl, *s_rim, *s_mark, *s_live, *s_wait, *s_wait_name, *s_overlay;
static Page s_page = P_TUNE;
static lv_obj_t *s_page_btn[P_COUNT];
static const char *kPageName[P_COUNT] = {"TUNE", "VOL", "ZOOM", "SQL", "BW", "STEP"};
static const DialLink *s_link = &kCtrlLink;
static DialState s_st;
static float s_meter = RIM_SNR_MIN;
static int s_rim_val = INT32_MIN;
static int s_accel = 1;
static int s_last_live = -1;  // waiting panel state shown (-1 = not yet)
static uint32_t s_dot_col = 0;

// ---------------------------------------------------------------- waterfall
// Full-screen, dimmed spectrum + waterfall behind the UI, fed ~10 lines/s over
// Bluetooth, with the tuned marker and receive passband drawn over it.
static constexpr int WF = 360, WF_LINE_H = 1;  // 1 px per line at ~10 lines/s: smooth scroll
// Split background: spectrum trace on top, waterfall below.
// Between them: a 2 px divider, then the frequency scale strip.
static constexpr int SPEC_TOP_H = 150, SCALE_Y0 = SPEC_TOP_H + 2, SCALE_H = 20, WF_Y0 = SCALE_Y0 + SCALE_H;
// Nothing holds a finished picture: the display asks for each row as it sends
// it (bg_row), and the rim, marker and passband are added to the row then.
// The waterfall is a ring of rows, so a new line moves a pointer, not 150 KB.
static constexpr int WF_ROWS = WF - WF_Y0;
static bool s_wf_on = false;      // waterfall background shown (layered display only)
static uint16_t *s_spec_px;       // SPEC_TOP_H x WF: the spectrum trace (PSRAM)
static uint16_t *s_ring;          // WF_ROWS x WF waterfall rows (PSRAM)
static int s_head = 0;            // ring row shown at the top of the waterfall
alignas(4) static uint16_t s_div_row[WF];  // the divider between the two
static uint16_t *s_scale_px;      // SCALE_H x WF: frequency scale under the spectrum (PSRAM)
static lv_obj_t *s_rim_host;      // hidden parent of the LVGL arcs when the software rim is in use
static bool s_rim_dirty = false;
static void wf_rim_update();
static uint32_t s_last_compose = 0;
static uint16_t s_wf_pal[256];
static float s_wf_floor = -999;
static uint32_t s_wf_center = 0, s_wf_span = 0;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void wf_palette() {
  // Same ramp as receiver mode, at ~45% so the UI on top stays readable.
  struct Stop { float p; uint8_t r, g, b; };
  static const Stop st[] = {{0, 2, 4, 10},     {0.25f, 10, 30, 110}, {0.5f, 20, 190, 230},
                            {0.7f, 250, 220, 60}, {0.88f, 240, 70, 40}, {1, 255, 255, 255}};
  for (int i = 0; i < 256; i++) {
    float p = i / 255.0f;
    int k = 0;
    while (k < 4 && p > st[k + 1].p) k++;
    float t = (p - st[k].p) / (st[k + 1].p - st[k].p);
    const float dim = 0.45f;
    s_wf_pal[i] = rgb565(dim * (st[k].r + t * (st[k + 1].r - st[k].r)), dim * (st[k].g + t * (st[k + 1].g - st[k].g)),
                         dim * (st[k].b + t * (st[k + 1].b - st[k].b)));
  }
}

// Spectrum trace animation (Smooth): the trace morphs between lines.
static float s_spec_from[WF], s_spec_to[WF], s_spec_now[WF];
static uint32_t s_anim_t0 = 0, s_anim_dur = 200, s_last_line_ms = 0, s_last_anim = 0;
static bool s_have_spec = false;

static void wf_draw_spec(const float *db_px) {
  uint8_t *d = (uint8_t *)s_spec_px;
  const uint32_t stride = WF * 2;
  // Spectrum (upper part): dim gradient fill under a brighter trace.
  const float spec_lo = s_wf_floor - 6, spec_range = 60;
  const uint16_t bg = rgb565(3, 5, 10), line = rgb565(90, 112, 120);
  static uint16_t fill[SPEC_TOP_H];  // gradient by row, computed once
  static bool fill_ok = false;
  if (!fill_ok) {
    for (int yy = 0; yy < SPEC_TOP_H; yy++) {
      float t = (float)yy / SPEC_TOP_H;  // 0 top .. 1 bottom
      fill[yy] = rgb565(5 + 8 * (1 - t), 40 + 50 * (1 - t), 62 + 40 * (1 - t));
    }
    fill_ok = true;
  }
  for (int y = 0; y < SPEC_TOP_H; y++) {
    uint16_t *r = (uint16_t *)(d + y * stride);
    for (int x = 0; x < WF; x++) r[x] = bg;
  }
  int prev_y = -1;
  for (int x = 0; x < WF; x++) {
    float db = db_px[x];
    int h = (int)((db - spec_lo) / spec_range * SPEC_TOP_H);
    h = h < 1 ? 1 : (h > SPEC_TOP_H ? SPEC_TOP_H : h);
    int y = SPEC_TOP_H - h;
    for (int yy = y + 1; yy < SPEC_TOP_H; yy++) ((uint16_t *)(d + yy * stride))[x] = fill[yy];
    int ya = prev_y < 0 ? y : (prev_y < y ? prev_y : y);
    int yb = prev_y < 0 ? y : (prev_y > y ? prev_y : y);
    for (int yy = ya; yy <= yb; yy++) ((uint16_t *)(d + yy * stride))[x] = line;
    prev_y = y;
  }
}

static void wf_shift_history(int dx);
static void wf_line(const float *pts, int n, uint32_t center, uint32_t span) {
  if (!s_wf_on || n < 2) return;
  // Same span, new centre (tuned along, or the page moved its view): slide
  // the history with it so old signals stay under their frequencies.
  static double shift_acc = 0;
  if (s_wf_span == span && s_wf_center && center != s_wf_center) {
    shift_acc += ((double)s_wf_center - (double)center) * WF / span;
    int dx = (int)shift_acc;
    if (dx) {
      shift_acc -= dx;
      wf_shift_history(dx);
    }
  } else if (s_wf_span != span) {
    shift_acc = 0;
  }
  s_wf_center = center;
  s_wf_span = span;
  // Noise floor: 20th percentile of the line (0.5 dB bins), smoothed.
  uint16_t hist[256] = {0};
  int valid = 0;
  for (int i = 0; i < n; i++) {
    if (pts[i] <= -200) continue;
    int b = (int)lroundf((pts[i] + 160) * 2);
    hist[b < 0 ? 0 : (b > 255 ? 255 : b)]++;
    valid++;
  }
  if (!valid) return;
  int acc = 0, pc = 0;
  for (; pc < 256; pc++) {
    acc += hist[pc];
    if (acc * 5 >= valid) break;
  }
  float floor_now = pc / 2.0f - 160;
  s_wf_floor = (s_wf_floor < -900 || fabsf(floor_now - s_wf_floor) > 20) ? floor_now
                                                                         : s_wf_floor * 0.9f + floor_now * 0.1f;
  const float lo = s_wf_floor - 3, range = 45;
  // Linear interpolation of the points across 360 pixels (Bluetooth sends
  // 160), so the trace and the waterfall are smooth rather than steps.
  static float px_db[WF];
  if (n == WF) {
    memcpy(px_db, pts, sizeof(px_db));
  } else {
    for (int x = 0; x < WF; x++) {
      float pos = (x + 0.5f) * n / WF - 0.5f;
      int i0 = (int)floorf(pos);
      float f = pos - i0;
      int a = i0 < 0 ? 0 : (i0 >= n ? n - 1 : i0);
      int b = i0 + 1 >= n ? n - 1 : (i0 + 1 < 0 ? 0 : i0 + 1);
      px_db[x] = (pts[a] <= -200 || pts[b] <= -200) ? -200 : pts[a] * (1 - f) + pts[b] * f;
    }
  }
  // Waterfall (lower part): the ring's top moves up one row, which becomes
  // the new line; everything older is now one row further down.
  s_head = (s_head + WF_ROWS - 1) % WF_ROWS;
  uint16_t *row = s_ring + (size_t)s_head * WF;
  for (int x = 0; x < WF; x++) {
    float db = px_db[x];
    int v = (int)((db - lo) / range * 255);
    row[x] = s_wf_pal[v < 0 ? 0 : (v > 255 ? 255 : v)];
  }

  // Spectrum (upper part): morph from what's shown to this line (Smooth),
  // or jump straight to it (Battery).
  memcpy(s_spec_from, s_spec_now, sizeof(s_spec_now));
  memcpy(s_spec_to, px_db, sizeof(s_spec_to));
  uint32_t now = millis();
  s_anim_dur = s_last_line_ms ? constrain(now - s_last_line_ms, 60u, 400u) : 200;
  s_last_line_ms = now;
  s_anim_t0 = now;
  bool animate = g_settings.ctrl_wf_smooth && s_have_spec;
  if (!animate) memcpy(s_spec_now, px_db, sizeof(s_spec_now));
  s_have_spec = true;
  wf_draw_spec(s_spec_now);
  display_mark_dirty(0, 0, WF - 1, WF - 1);  // everything moved
  static uint32_t lines = 0, t0 = 0;
  if (!t0) t0 = millis();
  if (++lines % 100 == 0) {
    Serial.printf("[wf] %.1f lines/s\n", 100000.0f / (millis() - t0));
    t0 = millis();
  }
}

static int wf_x(int64_t hz) {
  return (int)((hz - ((int64_t)s_wf_center - (int64_t)s_wf_span / 2)) * WF / (int64_t)s_wf_span);
}

// Drag to pan (the strip across the middle): shift what's on screen at once,
// and send the movement to the page, which moves its view.
static int s_drag_last_x, s_drag_moved;
static int s_pan_pending = 0;
static uint32_t s_pan_sent_at = 0;

// Slide the picture (spectrum and waterfall) sideways by dx px.
static void wf_shift_history(int dx) {
  if (!s_wf_on || !dx) return;
  int n = dx > 0 ? dx : -dx;
  if (n >= WF) n = WF - 1;
  for (int y = 0; y < SPEC_TOP_H + WF_ROWS; y++) {
    // Spectrum rows, then the ring's (order doesn't matter: all of them move)
    bool spec = y < SPEC_TOP_H;
    uint16_t *row = spec ? s_spec_px + (size_t)y * WF : s_ring + (size_t)(y - SPEC_TOP_H) * WF;
    uint16_t edge = spec ? rgb565(3, 5, 10) : 0;
    if (dx > 0) {
      memmove(row + n, row, (WF - n) * 2);
      for (int x = 0; x < n; x++) row[x] = edge;
    } else {
      memmove(row, row + n, (WF - n) * 2);
      for (int x = WF - n; x < WF; x++) row[x] = edge;
    }
  }
  float *arrs[3] = {s_spec_from, s_spec_to, s_spec_now};
  for (float *a : arrs) {
    if (dx > 0) {
      memmove(a + n, a, (WF - n) * sizeof(float));
      for (int x = 0; x < n; x++) a[x] = -200;
    } else {
      memmove(a, a + n, (WF - n) * sizeof(float));
      for (int x = WF - n; x < WF; x++) a[x] = -200;
    }
  }
}

// A drag: move the picture now, ahead of the lines from the new view.
static void wf_shift(int dx) {
  if (!s_wf_on || !dx) return;
  wf_shift_history(dx);
  // Keep the trace morph and the markers lined up with the moved picture.
  if (s_wf_span) s_wf_center -= (int64_t)dx * s_wf_span / WF;
  display_mark_dirty(0, 0, WF - 1, WF - 1);
}

static void pan_flush(bool force) {
  if (!s_pan_pending) return;
  // At most ~12 a second: each one becomes a view change on the server.
  if (!force && millis() - s_pan_sent_at < 80) return;
  int v = s_pan_pending > 127 ? 127 : (s_pan_pending < -127 ? -127 : s_pan_pending);
  s_link->send(DIAL_PAN, (int8_t)v);
  s_pan_pending -= v;
  s_pan_sent_at = millis();
}

static void wf_place_marker();
static void on_drag(lv_event_t *e) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    s_drag_last_x = p.x;
    s_drag_moved = 0;
    return;
  }
  int dx = p.x - s_drag_last_x;
  // Small wobbles under a resting finger aren't a drag: only move once it's
  // clearly travelling, so a tap stays a tap.
  if (dx && (s_drag_moved + abs(dx) >= 6 || s_drag_moved >= 6)) {
    s_drag_last_x = p.x;
    s_drag_moved += abs(dx);
    wf_shift(dx);
    wf_place_marker();
    s_pan_pending += dx;
  } else if (dx) {
    s_drag_moved += 0;  // wobble: ignore, keep the reference point
  }
  if (code == LV_EVENT_RELEASED && s_drag_moved < 6) {
    // A tap: tune to that point of the view, snapped to the step (by the page).
    int v = (int)lroundf((p.x - WF / 2) * 256.0f / WF);
    s_link->send(DIAL_TAP_TUNE, (int8_t)(v > 127 ? 127 : (v < -127 ? -127 : v)));
    return;
  }
  pan_flush(code == LV_EVENT_RELEASED);
}

// The tuning marker and passband are added to each background row as it's
// sent (bg_row), not LVGL objects that redraw full-height strips as they move.
static int s_mark_x = -1, s_pb_x0 = 0, s_pb_x1 = 0;

static void wf_place_marker() {
  if (!s_wf_on) return;
  bool have = s_wf_span && s_st.freq_hz;
  // Receive passband: the edges the far end sent, either side of the tuned freq.
  int c0 = 0, c1 = 0, mx = -1;
  if (have) {
    int x0 = wf_x((int64_t)s_st.freq_hz + s_st.bw_lo), x1 = wf_x((int64_t)s_st.freq_hz + s_st.bw_hi);
    if (x1 - x0 < 2) x1 = x0 + 2;
    c0 = x0 < 0 ? 0 : (x0 > WF ? WF : x0);
    c1 = x1 > WF ? WF : (x1 < 0 ? 0 : x1);
    int64_t x = wf_x(s_st.freq_hz);
    mx = (x < 0 || x >= WF - 1) ? -1 : (int)x;
  }
  if (mx == s_mark_x && c0 == s_pb_x0 && c1 == s_pb_x1) return;
  // Resend the columns that change: old and new marker and passband.
  int lo = WF, hi = -1;
  auto span = [&](int a, int b) {
    if (b <= a) return;
    lo = a < lo ? a : lo;
    hi = b - 1 > hi ? b - 1 : hi;
  };
  if (s_mark_x >= 0) span(s_mark_x, s_mark_x + 2);
  if (mx >= 0) span(mx, mx + 2);
  span(s_pb_x0, s_pb_x1);
  span(c0, c1);
  s_mark_x = mx;
  s_pb_x0 = c0;
  s_pb_x1 = c1;
  if (hi >= lo) display_mark_dirty(lo, 0, hi, WF - 1);
}

// ---------------------------------------------------------------- software rim
// LVGL's arcs over the waterfall cost ~125 ms a redraw (per-pixel angle masks
// over the whole screen). Instead the ring's ~10k pixels are listed once, by
// row, and painted into each row as it's sent. The LVGL arcs still hold the
// values (range, value, colours, marker angles) — they're just never drawn.
struct RimPx { uint16_t x; uint16_t frac; uint8_t band; uint8_t aa; uint16_t grad; };
// band: 0 = inner marker-only part (radius 163..169), 1 = the meter's own 7 px
static RimPx *s_rim_px;
static int s_rim_n;
static uint16_t s_rim_row[WF + 1];  // row y's pixels: s_rim_px[s_rim_row[y] .. s_rim_row[y+1])
static constexpr float RIM_CX = 179.5f, RIM_R_OUT = 176.0f, RIM_R_IN = 169.0f, RIM_R_MARK = 163.0f;

static void rim_table() {
  if (s_rim_px) return;
  lv_draw_buf_t *g = rim_gradient();  // 352x352, centred 4 px in
  s_rim_px = (RimPx *)ps_malloc(sizeof(RimPx) * 16000);
  s_rim_n = 0;
  for (int y = 0; y < WF; y++) {
    s_rim_row[y] = s_rim_n;
    for (int x = 0; x < WF; x++) {
      float dx = x - RIM_CX, dy = y - RIM_CX;
      float d = sqrtf(dx * dx + dy * dy);
      if (d < RIM_R_MARK - 1 || d > RIM_R_OUT + 1) continue;
      float ang = atan2f(dy, dx) * 57.29578f;
      if (ang < 0) ang += 360;
      float rel = ang - 135;
      if (rel < 0) rel += 360;
      if (rel > 270 || s_rim_n >= 16000) continue;
      // Edge anti-aliasing: coverage of the outer edge (and the inner edge of
      // whichever band the pixel is in).
      float cov = fminf(1, fmaxf(0, RIM_R_OUT + 0.5f - d));
      uint8_t band = d >= RIM_R_IN ? 1 : 0;
      float inner = band ? RIM_R_IN : RIM_R_MARK;
      cov = fminf(cov, fminf(1, fmaxf(0, d - inner + 0.5f)));
      if (cov <= 0) cov = 0;  // just outside an edge: kept, as backing
      RimPx &p = s_rim_px[s_rim_n++];
      p.x = x;
      p.frac = (uint16_t)(rel / 270.0f * 65535);
      p.band = band;
      p.aa = (uint8_t)(cov * 255);
      int gx = x - 4, gy = y - 4;
      p.grad = (gx >= 0 && gy >= 0 && gx < 352 && gy < 352)
                   ? ((uint16_t *)((uint8_t *)g->data + gy * g->header.stride))[gx]
                   : 0;
    }
  }
  s_rim_row[WF] = s_rim_n;
}

// a over b, a weighted t/256 (t = 0..256).
// Channel by channel (the packed 0xF81F trick needs a 5-bit t: with 8 bits
// red runs into blue and speckles every blended pixel).
static inline uint16_t blend565(uint16_t a, uint16_t b, uint32_t t) {  // t: 0..256
  uint32_t nt = 256 - t;
  uint32_t r = ((a >> 11) * t + (b >> 11) * nt + 128) >> 8;
  uint32_t g = (((a >> 5) & 63) * t + ((b >> 5) & 63) * nt + 128) >> 8;
  uint32_t bl = ((a & 31) * t + (b & 31) * nt + 128) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static inline uint16_t to565(lv_color_t c) { return rgb565(c.red, c.green, c.blue); }

// What the rim shows, read off the hidden arcs once per change, so painting a
// row doesn't go through LVGL's style lookups for every pixel.
struct RimSnap {
  uint32_t lim = 0;  // meter filled up to this frac
  bool grad = false, mark = false;
  uint16_t ind = 0, track = 0, mcol = 0;
  uint8_t ind_opa = 255;
  uint32_t m0 = 0, m1 = 0;  // marker frac range
  bool operator==(const RimSnap &o) const {
    return lim == o.lim && grad == o.grad && mark == o.mark && ind == o.ind && track == o.track &&
           mcol == o.mcol && ind_opa == o.ind_opa && m0 == o.m0 && m1 == o.m1;
  }
};
static RimSnap s_rs;

static RimSnap rim_snap() {
  RimSnap r;
  int vmin = lv_arc_get_min_value(s_rim), vmax = lv_arc_get_max_value(s_rim), val = lv_arc_get_value(s_rim);
  r.lim = vmax > vmin ? (uint32_t)((int64_t)(val - vmin) * 65535 / (vmax - vmin)) : 0;
  r.grad = lv_obj_get_style_arc_image_src(s_rim, LV_PART_INDICATOR) != nullptr;
  r.ind = to565(lv_obj_get_style_arc_color(s_rim, LV_PART_INDICATOR));
  r.ind_opa = lv_obj_get_style_arc_opa(s_rim, LV_PART_INDICATOR);
  r.track = to565(C(COL_EDGE));
  r.mark = !lv_obj_has_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
  r.mcol = to565(lv_obj_get_style_arc_color(s_mark, LV_PART_INDICATOR));
  if (r.mark) {
    float a0 = lv_arc_get_angle_start(s_mark) - 135.0f, a1 = lv_arc_get_angle_end(s_mark) - 135.0f;
    if (a0 < 0) a0 += 360;
    if (a1 < 0) a1 += 360;
    r.m0 = (uint32_t)(fmaxf(0, a0) / 270.0f * 65535);
    r.m1 = (uint32_t)(fminf(270, a1) / 270.0f * 65535);
  }
  return r;
}

// Colour of one rim pixel; false = nothing there (leave the background).
static inline bool rim_colour(const RimPx &p, const RimSnap &r, uint16_t *c) {
  if (r.mark && p.frac >= r.m0 && p.frac <= r.m1) {
    *c = r.mcol;
  } else if (!p.band) {
    return false;  // marker-only band, no marker here
  } else if (p.frac <= r.lim) {
    *c = r.grad ? p.grad : r.ind;
    if (r.ind_opa < 250) *c = blend565(*c, r.track, r.ind_opa);
  } else {
    *c = r.track;
  }
  return true;
}

// ---------------------------------------------------------------- frequency scale
// Ticks hanging from the divider with MHz labels (3 decimals) under the
// spectrum. Painted into its own strip of background rows whenever the view
// (centre or span) moves; the step is the smallest 1/2/5 that keeps the
// labels apart. Glyphs are taken from the 16 px font once, as alpha maps.
static constexpr int SCALE_GW = 16, SCALE_GH = 20;  // largest glyph box kept
struct ScaleGlyph { uint8_t w, h, adv; int8_t ox, oy; uint8_t a[SCALE_GW * SCALE_GH]; };
static const lv_font_t *const kScaleFont = &lv_font_montserrat_16;
static ScaleGlyph s_glyph[11];  // '0'..'9', '.'
static bool s_glyph_ok = false;
static uint32_t s_scale_center = 0, s_scale_span = 0;
static constexpr int SCALE_BASE = 17;  // label baseline, row in the strip

static void scale_glyphs() {
  if (s_glyph_ok) return;
  lv_draw_buf_t *buf = lv_draw_buf_create(SCALE_GW, SCALE_GH, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
  for (int i = 0; i < 11; i++) {
    ScaleGlyph &g = s_glyph[i];
    memset(&g, 0, sizeof(g));
    lv_font_glyph_dsc_t d;
    if (!lv_font_get_glyph_dsc(kScaleFont, &d, i < 10 ? '0' + i : '.', 0)) continue;
    g.adv = d.adv_w;
    if (d.box_w > SCALE_GW || d.box_h > SCALE_GH || !buf) continue;
    g.w = d.box_w;
    g.h = d.box_h;
    g.ox = d.ofs_x;
    g.oy = d.ofs_y;
    memset(buf->data, 0, buf->data_size);
    // Built-in fonts return the draw buffer they filled (A8), not the pixels.
    const lv_draw_buf_t *out = (const lv_draw_buf_t *)lv_font_get_glyph_bitmap(&d, buf);
    if (!out) continue;
    const uint8_t *bm = out->data;
    uint32_t stride = lv_draw_buf_width_to_stride(d.box_w, LV_COLOR_FORMAT_A8);
    for (int y = 0; y < g.h; y++) memcpy(g.a + y * g.w, bm + y * stride, g.w);
  }
  if (buf) lv_draw_buf_destroy(buf);
  s_glyph_ok = true;
}

static inline int scale_glyph_idx(char c) { return c == '.' ? 10 : (c >= '0' && c <= '9' ? c - '0' : -1); }

static int scale_text_w(const char *t) {
  int w = 0;
  for (; *t; t++) {
    int i = scale_glyph_idx(*t);
    if (i >= 0) w += s_glyph[i].adv;
  }
  return w;
}

static void scale_text(int x, const char *t, uint16_t col) {
  for (; *t; t++) {
    int i = scale_glyph_idx(*t);
    if (i < 0) continue;
    const ScaleGlyph &g = s_glyph[i];
    int top = SCALE_BASE - g.oy - g.h;
    for (int y = 0; y < g.h; y++) {
      int ry = top + y;
      if (ry < 0 || ry >= SCALE_H) continue;
      uint16_t *row = s_scale_px + (size_t)ry * WF;
      for (int xx = 0; xx < g.w; xx++) {
        int px = x + g.ox + xx;
        uint8_t a = g.a[y * g.w + xx];
        if (a && px >= 0 && px < WF) row[px] = blend565(col, row[px], a + 1);
      }
    }
    x += g.adv;
  }
}

static void wf_scale_update() {
  if (!s_wf_on || !s_scale_px) return;
  if (s_wf_center == s_scale_center && s_wf_span == s_scale_span) return;
  s_scale_center = s_wf_center;
  s_scale_span = s_wf_span;
  static const uint16_t bg = rgb565(6, 9, 16), tick = rgb565(90, 112, 120), minor = rgb565(45, 58, 66);
  static const uint16_t txt = to565(C(COL_TEXT));
  for (int i = 0; i < SCALE_H * WF; i++) s_scale_px[i] = bg;
  if (s_wf_span) {
    scale_glyphs();
    const int64_t lo = (int64_t)s_wf_center - s_wf_span / 2, hi = lo + s_wf_span;
    char t[16];
    snprintf(t, sizeof t, "%.3f", (double)(hi > 0 ? hi : 0) / 1e6);
    const int need = scale_text_w(t) + 14;  // widest label plus a gap
    // Smallest 1/2/5 step (>= 1 kHz: 3 decimals of MHz) whose labels don't touch.
    static const int kMul[3] = {1, 2, 5};
    int k = 0;
    int64_t dec = 1000, step = 1000;
    while (step * WF / (int64_t)s_wf_span < need) {
      if (++k == 3) {
        k = 0;
        dec *= 10;
      }
      step = kMul[k] * dec;
    }
    const int64_t sub = step / (kMul[k] == 2 ? 4 : 5);  // minor ticks: 2s in quarters, 1s and 5s in fifths
    // Inside the rim on these rows, and clear of the SNR figure in the middle.
    const float dy = SCALE_Y0 + SCALE_H - 179.5f, half = sqrtf(160.0f * 160.0f - dy * dy);
    const int xmin = (int)(179.5f - half), xmax = (int)(179.5f + half);
    const int snr0 = 180 - 34, snr1 = 180 + 34;
    for (int64_t f = (lo / sub) * sub; f <= hi; f += sub) {
      if (f < 0) continue;
      int x = (int)((f - lo) * WF / (int64_t)s_wf_span);
      if (x < 0 || x >= WF) continue;
      bool major = f % step == 0;
      for (int y = 0; y < (major ? 4 : 2); y++) s_scale_px[(size_t)y * WF + x] = major ? tick : minor;
      if (!major) continue;
      snprintf(t, sizeof t, "%.3f", (double)f / 1e6);
      int w = scale_text_w(t), x0 = x - w / 2;
      // Just clipping the SNR gap: slide the label off it, away from the
      // centre, by up to 10 px (less than the gap between labels).
      if (x0 + w > snr0 && x0 < snr1) {
        int nx = x < 180 ? snr0 - w : snr1;
        if (abs(nx - x0) > 10) continue;
        x0 = nx;
      }
      if (x0 < xmin || x0 + w > xmax) continue;
      scale_text(x0, t, txt);
    }
  }
  display_mark_dirty(0, SCALE_Y0, WF - 1, WF_Y0 - 1);
}

// The display's background, a row at a time: spectrum, divider or ring row,
// plus the passband, marker and rim where they fall in it.
static const uint16_t *bg_row(int y, uint16_t *scratch) {
  const uint16_t *src = y < SPEC_TOP_H ? s_spec_px + (size_t)y * WF
                        : y < SCALE_Y0 ? s_div_row
                        : y < WF_Y0    ? s_scale_px + (size_t)(y - SCALE_Y0) * WF
                                       : s_ring + (size_t)((s_head + y - WF_Y0) % WF_ROWS) * WF;
  int r0 = s_rim_row[y], r1 = s_rim_row[y + 1];
  bool pb = s_pb_x1 > s_pb_x0, mk = s_mark_x >= 0;
  if (!pb && !mk && r0 == r1) return src;
  memcpy(scratch, src, WF * 2);
  if (pb) {
    static const uint16_t pbc = to565(C(COL_ACCENT));
    for (int x = s_pb_x0; x < s_pb_x1; x++) scratch[x] = blend565(pbc, scratch[x], 51);  // ~20%
  }
  if (mk) {
    static const uint16_t mkc = to565(C(COL_BAD));
    scratch[s_mark_x] = blend565(mkc, scratch[s_mark_x], 102);  // ~40%
    scratch[s_mark_x + 1] = blend565(mkc, scratch[s_mark_x + 1], 102);
  }
  // The ring sits on a solid dark backing: its anti-aliased edges blend into
  // that, not into the waterfall's noise (which made the track look
  // speckled), and the waterfall stops cleanly inside the ring.
  static const uint16_t backing = to565(C(COL_BG));
  for (int i = r0; i < r1; i++) {
    const RimPx &p = s_rim_px[i];
    uint16_t c;
    if (!rim_colour(p, s_rs, &c)) c = backing;  // marker band with no marker
    scratch[p.x] = p.aa == 255 ? c : blend565(c, backing, p.aa + 1);
  }
  return scratch;
}

// The rim's values changed: resend just the pixels whose colour changed.
static void wf_rim_update() {
  if (!s_wf_on) return;
  RimSnap n = rim_snap();
  s_rim_dirty = false;
  s_last_compose = millis();
  if (n == s_rs) return;
  int x0 = WF, y0 = WF, x1 = -1, y1 = -1;
  for (int y = 0; y < WF; y++) {
    for (int i = s_rim_row[y]; i < s_rim_row[y + 1]; i++) {
      uint16_t a = 0, b = 0;
      bool ha = rim_colour(s_rim_px[i], s_rs, &a), hb = rim_colour(s_rim_px[i], n, &b);
      if (ha == hb && a == b) continue;
      int x = s_rim_px[i].x;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      y1 = y;
    }
  }
  s_rs = n;
  if (x1 >= 0) display_mark_dirty(x0, y0, x1, y1);
}

// One in-between frame of the trace morph: only the spectrum band is resent.
static void wf_animate() {
  if (!s_wf_on || !g_settings.ctrl_wf_smooth || !s_have_spec) return;
  uint32_t now = millis();
  float t = (now - s_anim_t0) / (float)s_anim_dur;
  if (t >= 1.0f && memcmp(s_spec_now, s_spec_to, sizeof(s_spec_now)) == 0) return;  // settled
  if (now - s_last_anim < 50) return;  // ~20 fps
  s_last_anim = now;
  if (t > 1) t = 1;
  float e = t * t * (3 - 2 * t);  // ease in-out
  for (int x = 0; x < WF; x++) s_spec_now[x] = s_spec_from[x] + (s_spec_to[x] - s_spec_from[x]) * e;
  wf_draw_spec(s_spec_now);
  display_mark_dirty(0, 0, WF - 1, SPEC_TOP_H - 1);
}

static void set_rim(int v) {
  if (v == s_rim_val) return;
  s_rim_val = v;
  lv_arc_set_value(s_rim, v);
  s_rim_dirty = true;
}

static const char *mode_label(uint8_t m) {
  static const char *labels[] = {"LSB", "USB", "AM", "SAM", "NFM", "FM", "CW-L", "CW-U", "IQ"};
  return m < sizeof(labels) / sizeof(labels[0]) ? labels[m] : "--";
}

static void fmt_hz(char *out, size_t n, uint32_t hz) {
  if (hz >= 1000000) snprintf(out, n, "%g MHz", hz / 1e6);
  else if (hz >= 1000) snprintf(out, n, "%g kHz", hz / 1e3);
  else snprintf(out, n, "%u Hz", (unsigned)hz);
}

static void update_chip() {
  char v[32];
  int w = abs(s_st.bw_hi - s_st.bw_lo);
  switch (s_page) {
    case P_TUNE: set_text(s_chip_lbl, LV_SYMBOL_SHUFFLE "  TUNE"); break;
    case P_ZOOM: set_text(s_chip_lbl, LV_SYMBOL_PLUS "  ZOOM"); break;
    case P_VOL: set_textf(s_chip_lbl, LV_SYMBOL_VOLUME_MAX "  %d", s_st.muted ? 0 : s_st.volume); break;
    case P_SQL:
      if (isnan(s_st.sql_db)) set_text(s_chip_lbl, "SQL  off");
      else set_textf(s_chip_lbl, "SQL  %d dB", (int)s_st.sql_db);
      break;
    case P_BW:
      if (w >= 1000) snprintf(v, sizeof(v), "BW  %d.%02dk", w / 1000, (w % 1000) / 10);
      else snprintf(v, sizeof(v), "BW  %d", w);
      set_text(s_chip_lbl, v);
      break;
    case P_STEP: {
      char h[20];
      fmt_hz(h, sizeof(h), s_st.step_hz);
      set_textf(s_chip_lbl, "STEP  %s", h);
      break;
    }
    default: break;
  }
}

// Configure the rim for the page: SNR gradient for TUNE/SQL, solid otherwise.
static void rim_mode() {
  s_rim_val = INT32_MIN;
  uint32_t col = kPageCol[s_page];
  lv_obj_set_style_arc_color(s_rim, C(col), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(s_rim, LV_OPA_COVER, LV_PART_INDICATOR);
  if (s_page == P_TUNE) {
    lv_arc_set_range(s_rim, (int)(RIM_SNR_MIN * 10), (int)(RIM_SNR_MAX * 10));
    lv_obj_set_style_arc_image_src(s_rim, rim_gradient(), LV_PART_INDICATOR);
  } else {
    lv_obj_set_style_arc_image_src(s_rim, nullptr, LV_PART_INDICATOR);
    if (s_page == P_SQL) lv_arc_set_range(s_rim, SQL_LO, SQL_HI);
    else if (s_page == P_BW) lv_arc_set_range(s_rim, 0, 12000);
    else if (s_page == P_STEP) lv_arc_set_range(s_rim, 0, 50);  // log10(Hz) x 10: 1 Hz .. 100 kHz
    else lv_arc_set_range(s_rim, 0, 100);
  }
  lv_obj_set_style_border_color(s_chip, C(col), 0);
  lv_obj_set_style_bg_color(s_chip, C(col), 0);
  lv_obj_set_style_text_color(s_chip_lbl, C(col), 0);
  // Direct page buttons: the active one filled in its colour.
  for (int i = 0; i < P_COUNT; i++) {
    if (!s_page_btn[i]) continue;
    bool on = i == s_page;
    lv_obj_set_style_bg_opa(s_page_btn[i], on ? LV_OPA_COVER : LV_OPA_10, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_page_btn[i], 0), C(on ? COL_BG : kPageCol[i]), 0);
  }
  update_chip();
}

static void send(uint8_t code) {
  s_link->send(code, 1);
}

// Tell the page how many waterfall lines a second to send (code 96): none
// is drawn while the background is off, so ask for the minimum then.
static void send_wf_rate() {
  int rate = !g_settings.ctrl_waterfall ? 1 : (g_settings.ctrl_wf_smooth ? 10 : 5);
  s_link->send(DIAL_WF_RATE, (int8_t)rate);
}

// ---------------------------------------------------------------- menu
static void close_overlay() {
  if (s_overlay) lv_obj_delete(s_overlay);
  s_overlay = nullptr;
}

// "Are you sure?" before restarting (back to the start-up choice).
static void confirm_restart() {
  menu_close();  // if it came from the menu (forgets the menu's list too)
  confirm_restart_open(s_scr, &s_overlay);
}

static void send_wf_rate();
static const MenuChoice kOnOff[] = {{"On", 1}, {"Off", 0}};
static const MenuChoice kWfRate[] = {{"Battery\n5/s", 0}, {"Smooth\n10/s", 1}};

static const MenuChoice kStepChoices[] = {{"Auto", 0},      {"10 Hz", 10},   {"100 Hz", 100}, {"500 Hz", 500},
                                          {"1 kHz", 1000},  {"5 kHz", 5000}, {"9 kHz", 9000}, {"10 kHz", 10000},
                                          {"100 kHz", 100000}};
static bool is_local() { return s_link->local; }

static const MenuItem kCtrlMenu[] = {
    // Receiver mode: the dial is the radio, so its own step (with Auto, which
    // follows the zoom) and where to listen live here.
    {LV_SYMBOL_SHUFFLE, "Tuning step", [] { return (int)g_settings.step_hz; },
     [](int v) {
       g_settings.step_hz = v;
       settings_save();
     },
     kStepChoices, 9, is_local},
    {LV_SYMBOL_HOME, "Receivers", nullptr,
     [](int) {
       menu_close();
       ui_show_picker();
     },
     nullptr, 0, is_local},
    {LV_SYMBOL_WIFI, "Wi-Fi setup", nullptr,
     [](int) {
       menu_close();
       wifi_setup_start();
       ui_show_setup("");
     },
     nullptr, 0, is_local},
    {LV_SYMBOL_IMAGE, "Waterfall background", [] { return (int)g_settings.ctrl_waterfall; },
     [](int v) {
       g_settings.ctrl_waterfall = v;
       settings_save();
       send_wf_rate();
       menu_close();
       controller_create(s_link);  // rebuild with/without it (the link carries on)
     },
     kOnOff, 2},
    {LV_SYMBOL_CHARGE, "Waterfall speed", [] { return (int)g_settings.ctrl_wf_smooth; },
     [](int v) {
       g_settings.ctrl_wf_smooth = v;
       settings_save();
       send_wf_rate();
     },
     kWfRate, 2, [] { return g_settings.ctrl_waterfall; }},
    {LV_SYMBOL_REFRESH, "Restart (choose mode)", nullptr, [](int) { confirm_restart(); }, nullptr, 0},
};

static void open_menu() {
  // This screen's own items, then the dial-wide ones, then Restart last.
  static constexpr int kNumCtrl = sizeof(kCtrlMenu) / sizeof(kCtrlMenu[0]);
  static MenuItem items[kNumCtrl + 8];
  static int n = 0;
  if (!n) {
    for (int i = 0; i < kNumCtrl - 1; i++) items[n++] = kCtrlMenu[i];
    for (int i = 0; i < kNumDeviceMenu && n < (int)(sizeof(items) / sizeof(items[0])) - 1; i++)
      items[n++] = kDeviceMenu[i];
    items[n++] = kCtrlMenu[kNumCtrl - 1];
  }
  menu_open(s_scr, s_link->local ? "R E C E I V E R" : "C O N T R O L L E R", s_link->local ? COL_ACCENT : COL_ACCENT2,
            items, n, &s_overlay);
}

// ---------------------------------------------------------------- pickers
// Full-screen ring of choices, as in receiver mode. Each choice sends a
// "set exactly this" code; the page does it and the dial shows the result.
static constexpr uint8_t CODE_MODE0 = DIAL_MODE0, CODE_BAND0 = DIAL_BAND0, CODE_STEP0 = DIAL_STEP0;
static constexpr uint8_t CODE_LOCK = DIAL_LOCK, CODE_SQL_TOGGLE = DIAL_SQL_TOGGLE, CODE_SQL_AUTO = DIAL_SQL_AUTO,
                         CODE_ZOOM_IN = DIAL_ZOOM_IN, CODE_ZOOM_OUT = DIAL_ZOOM_OUT, CODE_ZOOM_RESET = DIAL_ZOOM_RESET;
#define kSteps kDialSteps
#define kNumSteps kDialNumSteps

static lv_obj_t *open_sheet(const char *title) {
  close_overlay();
  s_overlay = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_overlay);
  lv_obj_set_size(s_overlay, 360, 360);
  lv_obj_center(s_overlay);
  lv_obj_set_style_bg_color(s_overlay, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = theme_label(s_overlay, &lv_font_montserrat_14, COL_DIM, title);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -16);
  lv_obj_t *x = lv_button_create(s_overlay);
  lv_obj_set_size(x, 44, 44);
  lv_obj_set_style_radius(x, 22, 0);
  lv_obj_set_style_bg_color(x, C(COL_PANEL), 0);
  lv_obj_set_style_border_color(x, C(COL_EDGE), 0);
  lv_obj_set_style_border_width(x, 2, 0);
  lv_obj_set_style_shadow_width(x, 0, 0);
  lv_obj_t *xl = theme_label(x, &lv_font_montserrat_16, COL_DIM, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);
  lv_obj_align(x, LV_ALIGN_CENTER, 0, 22);
  lv_obj_add_event_cb(x, [](lv_event_t *) { close_overlay(); }, LV_EVENT_CLICKED, nullptr);
  return s_overlay;
}

// A round choice on a circle; sends `code` and closes when tapped.
static lv_obj_t *sheet_button(int i, int n, int radius, int size, const char *txt, bool active, uint32_t col,
                              uint8_t code) {
  float a = (-90.0f + 360.0f * i / n) * (float)M_PI / 180.0f;
  lv_obj_t *b = lv_button_create(s_overlay);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, size / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_border_color(b, C(col), 0);
  lv_obj_set_style_bg_color(b, C(col), 0);
  lv_obj_set_style_bg_opa(b, active ? LV_OPA_COVER : LV_OPA_10, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_t *l = theme_label(b, &lv_font_montserrat_14, active ? COL_BG : COL_TEXT, txt);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(l);
  lv_obj_align(b, LV_ALIGN_CENTER, (int)(radius * cosf(a)), (int)(radius * sinf(a)));
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        send((uint8_t)(intptr_t)lv_event_get_user_data(e));
        close_overlay();
      },
      LV_EVENT_CLICKED, (void *)(intptr_t)code);
  return b;
}

static void open_mode_picker() {
  open_sheet("MODE");
  // IQ is left out, as v2's own mode cycle does: no audio, and a confirmation
  // dialog a knob can't answer.
  static const char *labels[] = {"LSB", "USB", "AM", "SAM", "NFM", "FM", "CW-L", "CW-U"};
  for (int i = 0; i < 8; i++) sheet_button(i, 8, 124, 68, labels[i], s_st.mode == i, COL_ACCENT, CODE_MODE0 + i);
}

static void open_band_picker() {
  open_sheet("BAND");
  // Receiver mode knows its coverage: only bands it fully covers (as v2's
  // bandsInRange). Over Bluetooth the page sorts that out itself.
  uint32_t top = s_link->local && sdr_max_hz() ? sdr_max_hz() : UINT32_MAX;
  int idx[16], n = 0;
  for (int i = 0; i < kDialNumBands && n < 16; i++)
    if (kDialBands[i].hi <= top) idx[n++] = i;
  for (int k = 0; k < n; k++) {
    const DialBand &b = kDialBands[idx[k]];
    bool here = s_st.freq_hz >= b.lo && s_st.freq_hz <= b.hi;
    sheet_button(k, n, 128, 58, b.name, here, COL_ACCENT2, CODE_BAND0 + idx[k]);
  }
}

static void open_step_picker() {
  open_sheet("STEP");
  for (int i = 0; i < kNumSteps; i++) {
    char t[16];
    uint32_t hz = kSteps[i];
    if (hz >= 1000) snprintf(t, sizeof(t), "%g\nkHz", hz / 1000.0);
    else snprintf(t, sizeof(t), "%u\nHz", (unsigned)hz);
    sheet_button(i, kNumSteps, 126, 62, t, s_st.step_hz == hz, 0x60a5fa, CODE_STEP0 + i);
  }
}

static void open_squelch_sheet() {
  open_sheet("SQUELCH");
  bool on = s_st.sql_on && !isnan(s_st.sql_db);
  sheet_button(0, 2, 104, 92, on ? "Turn\noff" : "Turn\non", false, COL_WARN, CODE_SQL_TOGGLE);
  sheet_button(1, 2, 104, 92, "Auto", false, COL_OK, CODE_SQL_AUTO);
}

static void open_zoom_sheet() {
  open_sheet("ZOOM");
  sheet_button(0, 3, 104, 84, LV_SYMBOL_PLUS "\nIn", false, COL_ACCENT2, CODE_ZOOM_IN);
  sheet_button(1, 3, 104, 84, LV_SYMBOL_MINUS "\nOut", false, COL_ACCENT2, CODE_ZOOM_OUT);
  sheet_button(2, 3, 104, 84, LV_SYMBOL_REFRESH "\nReset", false, COL_ACCENT2, CODE_ZOOM_RESET);
}

// Long-press on a knob-page button: the page's quick action.
static void page_hold(Page p) {
  switch (p) {
    case P_TUNE: send(CODE_LOCK); break;            // tuning lock (shown as LOCK)
    case P_VOL: send(DIAL_MUTE_TAP); break;          // mute
    case P_ZOOM: open_zoom_sheet(); break;
    case P_SQL: open_squelch_sheet(); break;
    case P_STEP: open_step_picker(); break;
    default: break;
  }
}

// ---------------------------------------------------------------- squelch notch drag
// The squelch notch on the rim (TUNE with a squelch set, and the SQL page) can
// be grabbed and dragged round the arc. Grab within ~20 degrees of it, so a
// touch elsewhere on the edge (the header sits there) isn't taken. The far end
// gets ordinary SQL-page turns (1 dB each, as v2's squelch_set), so neither
// protocol changes; the notch follows the finger at once.
static bool s_sql_drag = false;
static uint32_t s_sql_drag_end = 0;  // shown value is ours for a moment after release
static int s_sql_drag_db = 0;        // threshold shown while dragging (dB)
static int s_sql_sent_db = 0;        // what the far end has been told so far
static constexpr int SQL_OFF_DB = -10;  // v2: the slider floor means "off"

static void notch_scale(float *lo, float *hi) {
  if (s_page == P_SQL) {
    *lo = SQL_LO;
    *hi = SQL_HI;
  } else {
    *lo = RIM_SNR_MIN;
    *hi = RIM_SNR_MAX;
  }
}

// Position on the rim arc (0..1) of a screen point; false if it's not on the
// ring band at all.
static bool rim_frac_at(lv_point_t p, float *frac, float *ang_out) {
  float dx = p.x - RIM_CX, dy = p.y - RIM_CX;
  float d = sqrtf(dx * dx + dy * dy);
  if (d < 148 || d > 186) return false;
  float ang = atan2f(dy, dx) * 57.29578f;  // 0 = 3 o'clock, clockwise (as LVGL)
  if (ang < 0) ang += 360;
  float rel = ang - 135;
  if (rel < 0) rel += 360;
  if (ang_out) *ang_out = ang;
  // In the gap at the bottom: the nearer end.
  *frac = rel <= 270 ? rel / 270 : (rel > 315 ? 0 : 1);
  return true;
}

static bool notch_grabbable() {
  return (s_page == P_TUNE || s_page == P_SQL) && !lv_obj_has_flag(s_mark, LV_OBJ_FLAG_HIDDEN) && !s_overlay;
}

static void on_rim_hit(lv_event_t *e) {
  lv_hit_test_info_t *info = (lv_hit_test_info_t *)lv_event_get_param(e);
  float frac, ang;
  if (s_sql_drag) {  // keep the drag once it's started, wherever the finger goes
    info->res = true;
    return;
  }
  if (!notch_grabbable() || !rim_frac_at(*info->point, &frac, &ang)) {
    info->res = false;
    return;
  }
  float mid = (lv_arc_get_angle_start(s_mark) + lv_arc_get_angle_end(s_mark)) / 2.0f;
  float diff = fabsf(fmodf(ang - mid + 540.0f, 360.0f) - 180.0f);
  info->res = diff <= 20;
}

static void on_rim_drag(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) {
    s_sql_drag = true;
    s_sql_sent_db = (s_st.sql_on && !isnan(s_st.sql_db)) ? (int)lroundf(s_st.sql_db) : SQL_OFF_DB;
    s_sql_drag_db = s_sql_sent_db;
  }
  if (!s_sql_drag) return;
  float frac;
  if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST && rim_frac_at(p, &frac, nullptr)) {
    float lo, hi;
    notch_scale(&lo, &hi);
    int db = (int)lroundf(lo + frac * (hi - lo));
    if (s_page == P_TUNE && db <= SQL_OFF_DB) db = SQL_OFF_DB + 1;  // dragging here doesn't switch it off
    if (db != s_sql_drag_db) {
      s_sql_drag_db = db;
      haptic_tick();
      int delta = db - s_sql_sent_db;
      if (delta) {
        s_link->send(DIAL_PAGE_SQL, (int8_t)(delta > 127 ? 127 : (delta < -127 ? -127 : delta)));
        s_sql_sent_db = db;
      }
    }
  }
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    s_sql_drag = false;
    s_sql_drag_end = millis();
  }
}

// The threshold to show: the finger's while dragging (and briefly after, until
// the far end's state catches up), else what the far end says.
static bool shown_sql(float *db) {
  if (s_sql_drag || millis() - s_sql_drag_end < 600) {
    *db = s_sql_drag_db;
    return s_sql_drag_db > SQL_OFF_DB;
  }
  *db = s_st.sql_db;
  return s_st.sql_on && !isnan(s_st.sql_db);
}

// ---------------------------------------------------------------- screen
static lv_obj_t *s_lock, *s_bat;

static lv_obj_t *pill(const char *txt, int x, uint8_t tap, uint8_t hold, lv_obj_t **lbl) {
  lv_obj_t *b = theme_pill(s_live, txt, COL_EDGE, 70, 30);
  *lbl = lv_obj_get_child(b, 0);
  lv_obj_set_style_text_color(*lbl, C(COL_TEXT), 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, x, 268);
  lv_obj_add_event_cb(b, [](lv_event_t *e) { send((uint8_t)(intptr_t)lv_event_get_user_data(e)); },
                      LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)tap);
  if (hold)
    lv_obj_add_event_cb(b, [](lv_event_t *e) { send((uint8_t)(intptr_t)lv_event_get_user_data(e)); },
                        LV_EVENT_LONG_PRESSED, (void *)(intptr_t)hold);
  return b;
}

void controller_create(const DialLink *link) {
  s_link = link;
  s_overlay = nullptr;
  s_last_live = -1;
  s_dot_col = 0;
  for (auto &b : s_page_btn) b = nullptr;
  s_scr = theme_screen();
  if (display_layered()) {
    // Transparent UI layer: anything not drawn shows the background.
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_TRANSP, 0);
    display_set_background(nullptr, 0);
  }
  s_page = P_TUNE;
  s_st = DialState();

  // Waterfall background (bottom-most), optional
  s_wf_floor = -999;
  s_wf_span = 0;
  s_wf_center = 0;
  s_have_spec = false;
  s_last_line_ms = 0;
  s_mark_x = -1;
  s_pb_x0 = s_pb_x1 = 0;
  // Only on the layered display: the compositor pulls the rows (bg_row).
  s_wf_on = g_settings.ctrl_waterfall && display_layered();
  if (s_wf_on && !s_spec_px) {
    s_spec_px = (uint16_t *)ps_malloc((size_t)SPEC_TOP_H * WF * 2);
    s_ring = (uint16_t *)ps_malloc((size_t)WF_ROWS * WF * 2);
    s_scale_px = (uint16_t *)ps_malloc((size_t)SCALE_H * WF * 2);
    if (!s_spec_px || !s_ring || !s_scale_px) {
      free(s_spec_px);
      free(s_ring);
      free(s_scale_px);
      s_spec_px = s_ring = s_scale_px = nullptr;
    }
  }
  if (!s_spec_px) s_wf_on = false;
  if (s_wf_on) {
    wf_palette();
    rim_table();
    s_rs = RimSnap();
    const uint16_t sbg = rgb565(3, 5, 10);
    for (int i = 0; i < SPEC_TOP_H * WF; i++) s_spec_px[i] = sbg;
    memset(s_ring, 0, (size_t)WF_ROWS * WF * 2);
    for (int x = 0; x < WF; x++) s_div_row[x] = rgb565(14, 20, 34);
    s_scale_center = 1;  // force a repaint
    s_scale_span = 1;
    wf_scale_update();
    s_head = 0;
    display_set_background_rows(bg_row);
  }

  // With the waterfall on, the arcs live under a hidden host: they keep the
  // values, the software rim draws them (see bg_row).
  s_rim_host = nullptr;
  lv_obj_t *rim_parent = s_scr;
  if (s_wf_on) {
    s_rim_host = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_rim_host);
    lv_obj_set_size(s_rim_host, 360, 360);
    lv_obj_add_flag(s_rim_host, LV_OBJ_FLAG_HIDDEN);
    rim_parent = s_rim_host;
  }
  s_rim = rim_arc_create(rim_parent);
  s_mark = rim_marker_create(rim_parent);

  // Header: link dot + who we're connected to
  lv_obj_t *hdr = lv_obj_create(s_scr);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, 240, 24);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 6, 0);
  s_dot = lv_obj_create(hdr);
  lv_obj_remove_style_all(s_dot);
  lv_obj_set_size(s_dot, 8, 8);
  lv_obj_set_style_radius(s_dot, 4, 0);
  lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_dot, C(COL_WARN), 0);
  s_status = theme_label(hdr, &font_latin_14, COL_DIM, "");  // the page / receiver name
  if (s_link->local) {
    // Receiver mode: the header is where you are; tap it to pick another.
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hdr, 10);
    lv_obj_add_event_cb(hdr, [](lv_event_t *) { ui_show_picker(); }, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(hdr, [](lv_event_t *) { ui_show_picker(); }, LV_EVENT_LONG_PRESSED, nullptr);
  }
  s_bat = battery_icon_create(hdr);

  // Everything that needs a connected page lives in s_live.
  s_live = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_live);
  // Only a container: clickable (LVGL's default) it covered the header and
  // swallowed taps on the receiver name.
  lv_obj_remove_flag(s_live, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(s_live, 360, 360);
  lv_obj_center(s_live);
  lv_obj_remove_flag(s_live, LV_OBJ_FLAG_SCROLLABLE);

  s_freq = theme_label(s_live, &lv_font_montserrat_40, COL_TEXT, "-.---.---");
  lv_obj_align(s_freq, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_add_flag(s_freq, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s_freq, 8);
  lv_obj_add_event_cb(s_freq, [](lv_event_t *) { open_band_picker(); }, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_freq, [](lv_event_t *) { open_band_picker(); }, LV_EVENT_LONG_PRESSED, nullptr);
  s_info = theme_label(s_live, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 98);

  s_snr = theme_label(s_live, &lv_font_montserrat_40, COL_DIM, "--");
  lv_obj_align(s_snr, LV_ALIGN_CENTER, 0, -36);
  s_snr_cap = theme_label(s_live, &lv_font_montserrat_12, COL_DIM, "SNR dB");
  lv_obj_align(s_snr_cap, LV_ALIGN_CENTER, 0, -4);
  s_sql = theme_label(s_live, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_align(s_sql, LV_ALIGN_CENTER, 0, 14);

  // Drag strip: swipe across the middle to pan the waterfall (and the page's).
  if (s_wf_on) {
    lv_obj_t *drag = lv_obj_create(s_live);
    lv_obj_remove_style_all(drag);
    lv_obj_set_size(drag, 360, 96);
    lv_obj_align(drag, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_add_flag(drag, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(drag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(drag, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(drag, on_drag, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(drag, on_drag, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(drag, on_drag, LV_EVENT_RELEASED, nullptr);
  }

  // Direct knob-page buttons: no spectrum here, so the knob's functions get
  // one-tap access instead of cycling the chip.
  lv_obj_t *row = lv_obj_create(s_live);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 296, 44);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, 52);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  static const Page kRowOrder[P_COUNT] = {P_TUNE, P_ZOOM, P_SQL, P_BW, P_STEP, P_VOL};  // volume last (right)
  for (int k = 0; k < P_COUNT; k++) {
    int i = kRowOrder[k];
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, 46, 40);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, C(kPageCol[i]), 0);
    lv_obj_set_style_bg_color(b, C(kPageCol[i]), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_t *l = theme_label(b, &lv_font_montserrat_12, kPageCol[i], kPageName[i]);
    lv_obj_center(l);
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
          s_page = (Page)(intptr_t)lv_event_get_user_data(e);
          rim_mode();
        },
        LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(
        b, [](lv_event_t *e) { page_hold((Page)(intptr_t)lv_event_get_user_data(e)); }, LV_EVENT_LONG_PRESSED,
        (void *)(intptr_t)i);
    s_page_btn[i] = b;
  }

  lv_obj_t *mode_btn = pill("--", -102, DIAL_MODE_TAP, 0, &s_mode_lbl);
  lv_obj_add_event_cb(mode_btn, [](lv_event_t *) { open_mode_picker(); }, LV_EVENT_LONG_PRESSED, nullptr);
  pill("", 102, DIAL_MUTE_TAP, DIAL_MUTE_HOLD, &s_mute_lbl);

  // Tuning lock: v2's own padlock — the page refuses tuning and mode changes
  // while it's on, and the dial stops sending TUNE turns.
  s_lock = padlock_create(s_live, 40);
  lv_obj_align(s_lock, LV_ALIGN_TOP_MID, -36, 263);
  lv_obj_add_event_cb(s_lock, [](lv_event_t *) { send(80 /* lock toggle */); }, LV_EVENT_CLICKED, nullptr);

  // Centre: the waterfall toolbar's "centre on tuned" in the web UI.
  lv_obj_t *ctr = lv_button_create(s_live);
  lv_obj_set_size(ctr, 40, 40);
  lv_obj_set_style_radius(ctr, 20, 0);
  lv_obj_set_style_bg_color(ctr, C(COL_PANEL), 0);
  lv_obj_set_style_border_color(ctr, C(COL_ACCENT), 0);
  lv_obj_set_style_border_width(ctr, 2, 0);
  lv_obj_set_style_shadow_width(ctr, 0, 0);
  lv_obj_set_style_bg_color(ctr, C(COL_ACCENT), LV_STATE_PRESSED);
  lv_obj_t *cl = theme_label(ctr, &lv_font_montserrat_16, COL_ACCENT, LV_SYMBOL_GPS);
  lv_obj_center(cl);
  lv_obj_align(ctr, LV_ALIGN_TOP_MID, 36, 263);
  lv_obj_add_event_cb(ctr, [](lv_event_t *) { send(DIAL_CENTER_TAP); }, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(ctr, [](lv_event_t *) { send(CODE_ZOOM_RESET); }, LV_EVENT_LONG_PRESSED, nullptr);

  // Waiting panel (shown until a page connects and sends state)
  s_wait = lv_obj_create(s_scr);
  lv_obj_remove_style_all(s_wait);
  lv_obj_set_size(s_wait, 270, 240);
  lv_obj_align(s_wait, LV_ALIGN_CENTER, 0, -6);
  lv_obj_set_flex_flow(s_wait, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_wait, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_wait, 8, 0);
  theme_label(s_wait, &lv_font_montserrat_40, s_link->local ? COL_ACCENT : COL_ACCENT2,
              s_link->local ? LV_SYMBOL_WIFI : ctrl_transport()->symbol);
  s_wait_name = theme_label(s_wait, &font_latin_16, COL_TEXT,
                            s_link->local ? g_settings.inst_call.c_str() : ctrl_name().c_str());
  // Short lines: the round screen is narrow this far from the centre.
  lv_obj_t *how = theme_label(s_wait, &lv_font_montserrat_12, COL_DIM,
                              s_link->local ? "Connecting..." : ctrl_transport()->how);
  lv_obj_set_width(how, 240);
  lv_obj_set_style_text_align(how, LV_TEXT_ALIGN_CENTER, 0);
  // Ways out while nothing's connected: the menu (also on a held chip), and
  // a restart, back to the start-up choice (after an "are you sure?").
  lv_obj_t *outs = lv_obj_create(s_wait);
  lv_obj_remove_style_all(outs);
  lv_obj_set_size(outs, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(outs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(outs, 14, 0);
  lv_obj_set_style_pad_top(outs, 6, 0);
  lv_obj_t *set = theme_pill(outs, LV_SYMBOL_SETTINGS " Settings", COL_DIM, 116, 38);
  lv_obj_add_event_cb(set, [](lv_event_t *) { open_menu(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *rst = theme_pill(outs, LV_SYMBOL_REFRESH " Restart", COL_DIM, 116, 38);
  lv_obj_add_event_cb(rst, [](lv_event_t *) { confirm_restart(); }, LV_EVENT_CLICKED, nullptr);

  // Knob page chip
  s_chip = theme_pill(s_scr, "", COL_ACCENT, 124, 38);
  s_chip_lbl = lv_obj_get_child(s_chip, 0);
  lv_obj_align(s_chip, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_add_event_cb(
      s_chip,
      [](lv_event_t *) {
        s_page = (Page)((s_page + 1) % P_COUNT);
        rim_mode();
      },
      LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_chip, [](lv_event_t *) { open_menu(); }, LV_EVENT_LONG_PRESSED, nullptr);

  // Rim touch: full screen, but it only claims touches near the squelch notch
  // (custom hit test), so everything under it works as before.
  s_sql_drag = false;
  s_sql_drag_end = 0;
  lv_obj_t *rim_touch = lv_obj_create(s_scr);
  lv_obj_remove_style_all(rim_touch);
  lv_obj_set_size(rim_touch, 360, 360);
  lv_obj_center(rim_touch);
  lv_obj_add_flag(rim_touch, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(rim_touch, LV_OBJ_FLAG_ADV_HITTEST);
  lv_obj_remove_flag(rim_touch, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(rim_touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(rim_touch, on_rim_hit, LV_EVENT_HIT_TEST, nullptr);
  lv_obj_add_event_cb(rim_touch, on_rim_drag, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(rim_touch, on_rim_drag, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(rim_touch, on_rim_drag, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(rim_touch, on_rim_drag, LV_EVENT_PRESS_LOST, nullptr);

  rim_mode();
  lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);
  ui_load(s_scr);
}

static void render_state() {
  char f[24], a[24], b[24];
  uint32_t hz = s_st.freq_hz;
  snprintf(f, sizeof(f), "%u.%03u.%03u", (unsigned)(hz / 1000000), (unsigned)(hz / 1000 % 1000), (unsigned)(hz % 1000));
  set_text(s_freq, f);
  fmt_hz(a, sizeof(a), s_st.step_hz * s_accel);
  int w = abs(s_st.bw_hi - s_st.bw_lo);
  fmt_hz(b, sizeof(b), w);
  set_textf(s_info, "STEP %s%s   " LV_SYMBOL_BULLET "   BW %s%s", a, s_accel > 1 ? " " LV_SYMBOL_CHARGE : "", b,
                        s_st.locked ? "   " LV_SYMBOL_CLOSE " LOCK" : "");
  // Lit while accelerating, as in receiver mode.
  set_tcol(s_info, s_accel > 1 ? COL_ACCENT : COL_DIM);
  set_text(s_mode_lbl, mode_label(s_st.mode));
  static int lock_shown = -1;
  if ((int)s_st.locked != lock_shown) {
    lock_shown = s_st.locked;
    padlock_set(s_lock, s_st.locked);
  }
  set_textf(s_mute_lbl, "%s %d", s_st.muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX, s_st.volume);
  set_tcol(s_mute_lbl, s_st.muted ? COL_BAD : COL_TEXT);
  if (s_st.sql_on && !isnan(s_st.sql_db)) {
    set_textf(s_sql, "SQL %d dB  " LV_SYMBOL_BULLET "  %s", (int)s_st.sql_db, s_st.sql_open ? "OPEN" : "closed");
    set_tcol(s_sql, s_st.sql_open ? COL_OK : COL_DIM);
  } else {
    set_text(s_sql, "");
  }
  update_chip();
}

// Debug ('h' over serial): hide the controller's widgets, keep the background,
// to measure what the redraw cost is made of.
bool g_dbg_hide_ui = false;

void controller_tick(int knob) {
  static bool hid = false;
  if (g_dbg_hide_ui != hid && s_live) {
    hid = g_dbg_hide_ui;
    if (hid) lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_live, LV_OBJ_FLAG_HIDDEN);
    if (s_chip) {
      if (hid) lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // Debug timing: where a pass goes (printed every 5 s).
  static uint32_t t_loop = 0, t_status = 0, t_line = 0, t_draw = 0, t_rest = 0, t_n = 0, t_at = 0;
  uint32_t t0 = micros();
  pan_flush(false);
  s_link->loop();
  uint32_t t1 = micros();
  t_loop += t1 - t0;
  static uint32_t bat_at = 0;
  if (millis() - bat_at > 2000) {
    bat_at = millis();
    battery_icon_update(s_bat);
  }
  // Receiver mode is its own receiver: always live, even while the socket
  // (re)connects — the header says how that's going.
  bool conn = s_link->local || s_link->connected();
  DialState st = s_link->state();
  bool live = conn && st.valid;

  // Header: what we're connected to (change-only, it's polled every pass)
  {
    String text;
    uint32_t dot;
    uint32_t ts = micros();
    s_link->status(&text, &dot);
    t_status += micros() - ts;
    set_text(s_status, text.c_str());
    if (dot != s_dot_col) {
      s_dot_col = dot;
      lv_obj_set_style_bg_color(s_dot, C(dot), 0);
    }
  }

  // Waiting panel
  if ((int)live != s_last_live) {
    s_last_live = live;
    if (live) {
      lv_obj_remove_flag(s_live, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_wait, LV_OBJ_FLAG_HIDDEN);
      send_wf_rate();  // tell the far end how fast to feed the waterfall
    } else {
      lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_wait, LV_OBJ_FLAG_HIDDEN);
      led_ring_off();
      lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      set_rim(lv_arc_get_min_value(s_rim));
    }
  }

  // Knob -> page (the page does the work; the dial only shows the result)
  if (knob && !s_overlay && conn && !(s_page == P_TUNE && s_st.locked)) {
    int n = knob;
    if (s_page == P_TUNE) {
      // Spin faster -> bigger steps (same thresholds as receiver mode). Sent as
      // more detents: v2's stepBy lands them on the step grid.
      int a = knob_accel();
      if (a != s_accel) {
        s_accel = a;
        if (s_st.valid) render_state();
      }
      n *= a;
    }
    s_link->send(kPageCode[s_page], (int8_t)(n > 127 ? 127 : (n < -127 ? -127 : n)));
  }
  if (s_accel > 1 && knob_idle_ms() > 400) {
    s_accel = 1;
    if (s_st.valid) render_state();
  }
  if (s_overlay && knob) {
    menu_knob(knob);
  }

  // Waterfall lines: always take them (so the queue never goes stale), draw
  // only when visible.
  static float line[DIAL_LINE_MAX];
  int ln = 0;
  uint32_t lc, ls;
  static uint32_t d_taken = 0, d_drawn = 0, d_knob = 0, d_knob_lost = 0;
  if (knob) {
    d_knob++;
    if (s_overlay) d_knob_lost++;
  }
  uint32_t tl = micros();
  bool got_line = s_link->take_line(line, &ln, &lc, &ls);
  if (got_line) d_taken++;
  uint32_t tl2 = micros();
  t_line += tl2 - tl;
  t_n++;
  if (millis() - t_at > 5000) {
    Serial.printf("[tick] %lu passes: link %lu us, status %lu, take_line %lu, draw %lu, rest %lu (avg)\n",
                  (unsigned long)t_n, (unsigned long)(t_loop / t_n), (unsigned long)(t_status / t_n),
                  (unsigned long)(t_line / t_n), (unsigned long)(t_draw / t_n), (unsigned long)(t_rest / t_n));
    Serial.printf("[dial] live=%d overlay=%p wf_on=%d paused=%d lines taken %lu drawn %lu, knob %lu (%lu to overlay)\n",
                  (int)live, (void *)s_overlay, (int)s_wf_on, (int)saver_spectrum_paused(), (unsigned long)d_taken,
                  (unsigned long)d_drawn, (unsigned long)d_knob, (unsigned long)d_knob_lost);
    d_taken = d_drawn = d_knob = d_knob_lost = 0;
    t_loop = t_status = t_line = t_draw = t_rest = t_n = 0;
    t_at = millis();
  }
  if (!live || s_overlay) return;
  if (got_line && s_wf_on && !saver_spectrum_paused()) {
    wf_line(line, ln, lc, ls);
    d_drawn++;
  }
  uint32_t td = micros();
  t_draw += td - tl2;
  struct Rest { uint32_t t; uint32_t &acc; ~Rest() { acc += micros() - t; } } rest_timer{td, t_rest};
  // Advance the trace morph every pass, line or not — it used to run only on
  // passes without a line, and with lines as fast as the loop it never did.
  if (!saver_spectrum_paused()) wf_animate();
  bool state_changed = st.updated_ms != s_st.updated_ms;
  if (state_changed) {
    s_st = st;
    render_state();
  }
  // Tuning or bandwidth changed, or a new line: put marker/passband in place.
  if (got_line || state_changed) wf_place_marker();
  wf_scale_update();  // view moved (new line, or dragged)
  // The software rim follows the waterfall's own redraws (~5/s). If the meter
  // or marker changed and no line has come along to carry it, recomposite —
  // at most 5/s, and only when something actually changed.
  // The software rim: at most ~30 updates a second, each resending only the
  // pixels that changed (nothing at all when the values didn't).
  if (s_wf_on && millis() - s_last_compose >= 33 && !saver_spectrum_paused()) wf_rim_update();

  // SNR readout, rim and LEDs
  float snr = s_st.snr;
  static int last_snr = INT32_MIN;
  int si = isnan(snr) ? INT32_MIN + 1 : (int)lroundf(snr);
  if (si != last_snr) {
    last_snr = si;
    if (isnan(snr)) {
      lv_label_set_text(s_snr, "--");
      lv_obj_set_style_text_color(s_snr, C(COL_DIM), 0);
    } else {
      lv_label_set_text_fmt(s_snr, "%d", si);
      lv_obj_set_style_text_color(s_snr, C(si >= 15 ? COL_OK : (si >= 6 ? COL_WARN : COL_BAD)), 0);
    }
  }
  led_ring_snr(snr);
  float tgt = isnan(snr) ? RIM_SNR_MIN : snr;
  s_meter = tgt > s_meter ? tgt : s_meter + (tgt - s_meter) * 0.12f;
  float sql_db;
  bool sql = shown_sql(&sql_db);
  switch (s_page) {
    case P_TUNE:
      set_rim((int)(fmaxf(RIM_SNR_MIN, fminf(RIM_SNR_MAX, s_meter)) * 10));
      if (sql) {
        rim_marker_place(s_mark, (sql_db - RIM_SNR_MIN) / (RIM_SNR_MAX - RIM_SNR_MIN));
        // As receiver mode: white while the gate is open, amber while closed.
        lv_obj_set_style_arc_color(s_mark, C(s_st.sql_open ? 0xffffff : COL_WARN), LV_PART_INDICATOR);
      } else {
        lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_set_style_arc_opa(s_rim, sql && !s_st.sql_open ? LV_OPA_50 : LV_OPA_COVER, LV_PART_INDICATOR);
      break;
    case P_SQL: {
      // Live SNR against the threshold marker, as the web UI's squelch slider.
      set_rim((int)lroundf(fmaxf(SQL_LO, fminf(SQL_HI, s_meter))));
      lv_obj_set_style_arc_color(s_rim, C(!sql || s_st.sql_open ? COL_OK : COL_DIM), LV_PART_INDICATOR);
      rim_marker_place(s_mark, ((sql ? sql_db : SQL_LO) - SQL_LO) / (float)(SQL_HI - SQL_LO));
      lv_obj_set_style_arc_color(s_mark, C(COL_WARN), LV_PART_INDICATOR);
      break;
    }
    case P_VOL:
      set_rim(s_st.muted ? 0 : s_st.volume);
      lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      break;
    case P_BW:
      set_rim(abs(s_st.bw_hi - s_st.bw_lo));
      lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      break;
    case P_STEP:
      set_rim(s_st.step_hz ? (int)lroundf(log10f((float)s_st.step_hz) * 10) : 0);
      lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      break;
    case P_ZOOM: {
      // How far in, on a log scale: empty at the whole receiver, full at the
      // narrowest view (clockwise zooms in, so the rim fills that way).
      uint32_t lo, hi;
      s_link->zoom_range(&lo, &hi);
      int v = 0;
      if (s_wf_span && hi > lo) {
        float f = logf((float)hi / (float)s_wf_span) / logf((float)hi / (float)lo);
        v = (int)lroundf(fmaxf(0, fminf(1, f)) * 100);
      }
      set_rim(v);
      lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      break;
    }
    default:
      set_rim(0);
      lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
      break;
  }
}
