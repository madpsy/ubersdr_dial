#include "sleep_anim.h"

#include <lvgl.h>

#include "theme.h"

static lv_obj_t *s_root = nullptr;

static void set_h(void *o, int32_t v) {
  lv_obj_set_height((lv_obj_t *)o, v);
  lv_obj_align((lv_obj_t *)o, LV_ALIGN_CENTER, (int32_t)(intptr_t)lv_obj_get_user_data((lv_obj_t *)o), -20);
}
static void set_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
static void set_y(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void set_x(void *o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)o, v, 0); }

static void anim(lv_obj_t *o, lv_anim_exec_xcb_t f, int32_t a, int32_t b, uint32_t delay, uint32_t ms,
                 uint32_t playback = 0, lv_anim_path_cb_t path = lv_anim_path_ease_in_out) {
  lv_anim_t an;
  lv_anim_init(&an);
  lv_anim_set_var(&an, o);
  lv_anim_set_exec_cb(&an, f);
  lv_anim_set_values(&an, a, b);
  lv_anim_set_delay(&an, delay);
  lv_anim_set_duration(&an, ms);
  if (playback) lv_anim_set_playback_duration(&an, playback);
  lv_anim_set_path_cb(&an, path);
  lv_anim_start(&an);
}

static lv_obj_t *eye(int x) {
  lv_obj_t *e = lv_obj_create(s_root);
  lv_obj_remove_style_all(e);
  lv_obj_set_size(e, 34, 44);
  lv_obj_set_style_radius(e, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(e, C(COL_ACCENT), 0);
  lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
  lv_obj_set_user_data(e, (void *)(intptr_t)x);
  lv_obj_align(e, LV_ALIGN_CENTER, x, -20);
  // Blink, blink... then droop shut and stay shut.
  anim(e, set_h, 44, 6, 350, 120, 120);
  anim(e, set_h, 44, 6, 900, 120, 160);
  anim(e, set_h, 44, 4, 1500, 700);
  return e;
}

void sleep_anim_start() {
  sleep_anim_stop();
  s_root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, 360, 360);
  lv_obj_center(s_root);
  lv_obj_set_style_radius(s_root, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_root, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // a tap cancels it without pressing what is underneath
  set_opa(s_root, 0);
  anim(s_root, set_opa, 0, 255, 0, 250);

  eye(-34);
  eye(34);
  // A little smile that flattens into a sleepy line.
  lv_obj_t *m = lv_arc_create(s_root);
  lv_obj_remove_style(m, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(m, 56, 56);
  lv_arc_set_bg_angles(m, 30, 150);
  lv_obj_set_style_arc_width(m, 0, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(m, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(m, C(COL_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(m, true, LV_PART_MAIN);
  lv_obj_align(m, LV_ALIGN_CENTER, 0, 0);

  // Zzz: three, each bigger, drifting up and right and fading.
  static const lv_font_t *fonts[] = {&lv_font_montserrat_16, &lv_font_montserrat_20, &lv_font_montserrat_28};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *z = theme_label(s_root, fonts[i], COL_ACCENT2, "z");
    lv_obj_align(z, LV_ALIGN_CENTER, 56 + i * 14, -54 - i * 10);
    set_opa(z, 0);
    uint32_t d = 1700 + i * 380;
    anim(z, set_opa, 0, 255, d, 300, 700);
    anim(z, set_y, 0, -46, d, 1000, 0, lv_anim_path_ease_out);
    anim(z, set_x, 0, 14, d, 1000, 0, lv_anim_path_ease_out);
  }

  lv_obj_t *t = theme_label(s_root, &lv_font_montserrat_14, COL_DIM, "Night night...\ntap or turn to wake");
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 74);
  set_opa(t, 0);
  anim(t, set_opa, 0, 255, 600, 400);
}

void sleep_anim_stop() {
  if (!s_root) return;
  lv_obj_delete(s_root);  // takes its animations with it
  s_root = nullptr;
}
