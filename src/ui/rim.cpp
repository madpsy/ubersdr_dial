#include "rim.h"

#include <math.h>

#include "theme.h"

static constexpr int RING = 352;
static lv_draw_buf_t *s_grad;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void lerp_rgb(float t, const uint8_t a[3], const uint8_t b[3], uint8_t o[3]) {
  for (int i = 0; i < 3; i++) o[i] = a[i] + t * (b[i] - a[i]);
}

// SNR -> colour: red (<6 dB) -> amber -> green (>=15 dB), blended.
static void snr_rgb(float snr, uint8_t o[3]) {
  static const uint8_t red[3] = {239, 68, 68}, amber[3] = {251, 191, 36}, green[3] = {34, 197, 94},
                       bright[3] = {74, 222, 128};
  if (snr < 3) lerp_rgb(0, red, red, o);
  else if (snr < 8) lerp_rgb((snr - 3) / 5, red, amber, o);
  else if (snr < 12) lerp_rgb(0, amber, amber, o);
  else if (snr < 16) lerp_rgb((snr - 12) / 4, amber, green, o);
  else lerp_rgb((snr - 16) / (RIM_SNR_MAX - 16) > 1 ? 1 : (snr - 16) / (RIM_SNR_MAX - 16), green, bright, o);
}

lv_draw_buf_t *rim_gradient() {
  if (s_grad) return s_grad;
  s_grad = lv_draw_buf_create(RING, RING, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
  uint8_t *d = (uint8_t *)s_grad->data;
  const float c = (RING - 1) / 2.0f;
  for (int y = 0; y < RING; y++) {
    uint16_t *row = (uint16_t *)(d + y * s_grad->header.stride);
    for (int x = 0; x < RING; x++) {
      float dx = x - c, dy = y - c;
      if (dx * dx + dy * dy < 150 * 150) {  // only the rim matters
        row[x] = 0;
        continue;
      }
      // LVGL arc angles: 0 deg = 3 o'clock, clockwise; the arc starts at 135.
      float ang = atan2f(dy, dx) * 57.29578f;
      if (ang < 0) ang += 360;
      float rel = ang - 135;
      if (rel < 0) rel += 360;
      float t = rel > 270 ? (rel > 315 ? 0 : 1) : rel / 270;
      uint8_t rgb[3];
      snr_rgb(RIM_SNR_MIN + t * (RIM_SNR_MAX - RIM_SNR_MIN), rgb);
      row[x] = rgb565(rgb[0], rgb[1], rgb[2]);
    }
  }
  return s_grad;
}

lv_obj_t *rim_arc_create(lv_obj_t *parent) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, RING, RING);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(a, 7, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, C(COL_EDGE), LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 7, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  return a;
}

lv_obj_t *rim_marker_create(lv_obj_t *parent) {
  lv_obj_t *m = lv_arc_create(parent);
  lv_obj_set_size(m, RING, RING);
  lv_obj_center(m);
  lv_obj_remove_style(m, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(m, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(m, 13, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(m, C(COL_WARN), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(m, true, LV_PART_INDICATOR);
  lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
  return m;
}

void rim_marker_place(lv_obj_t *marker, float frac) {
  frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
  float a = 135.0f + 270.0f * frac;
  lv_arc_set_angles(marker, (int)a - 2, (int)a + 2);
  lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
}
