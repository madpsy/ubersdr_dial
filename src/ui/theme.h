#pragma once
#include <lvgl.h>

// Dark "instrument" palette.
#define COL_BG 0x05070d
#define COL_PANEL 0x0d1322
#define COL_EDGE 0x1c2740
#define COL_TEXT 0xe6ebf5
#define COL_DIM 0x7a8599
#define COL_ACCENT 0x22d3ee   // cyan: tune
#define COL_ACCENT2 0xa78bfa  // violet: zoom
#define COL_OK 0x34d399
#define COL_WARN 0xfbbf24
#define COL_BAD 0xf87171

static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// Fresh black screen with the theme background.
static inline lv_obj_t *theme_screen() {
  lv_obj_t *s = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static inline lv_obj_t *theme_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, C(color), 0);
  lv_label_set_text(l, txt);
  return l;
}

// Rounded pill button with a label; returns the button.
static inline lv_obj_t *theme_pill(lv_obj_t *parent, const char *txt, uint32_t color, int w, int h) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_bg_color(b, C(color), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
  lv_obj_set_style_border_color(b, C(color), 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
  lv_obj_t *l = theme_label(b, &lv_font_montserrat_16, color, txt);
  lv_obj_center(l);
  return b;
}
