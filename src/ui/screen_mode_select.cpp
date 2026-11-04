// Startup chooser: Receiver (Wi-Fi, the dial is the radio) or Controller
// (Bluetooth, the dial drives the UberSDR web UI). Asked on every boot; the
// last choice is pre-highlighted so a tap is all it takes.
#include "../app/settings.h"
#include "theme.h"
#include "ui.h"

void ui_load(lv_obj_t *scr);

static lv_obj_t *s_btn[2];
static int s_sel = 0;
static void (*s_on_pick)(int) = nullptr;

static void highlight() {
  for (int i = 0; i < 2; i++) {
    uint32_t col = i == 0 ? COL_ACCENT : COL_ACCENT2;
    bool on = i == s_sel;
    lv_obj_set_style_bg_opa(s_btn[i], on ? LV_OPA_30 : LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_btn[i], on ? 4 : 2, 0);
    lv_obj_set_style_border_color(s_btn[i], C(on ? col : COL_EDGE), 0);
    lv_obj_set_style_transform_scale(s_btn[i], on ? 270 : 240, 0);  // 256 = 1.0
  }
}

static lv_obj_t *choice(lv_obj_t *scr, int idx, const char *sym, const char *title, const char *sub, int x) {
  uint32_t col = idx == 0 ? COL_ACCENT : COL_ACCENT2;
  lv_obj_t *b = lv_button_create(scr);
  lv_obj_set_size(b, 132, 132);
  lv_obj_set_style_radius(b, 66, 0);
  lv_obj_set_style_bg_color(b, C(col), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_transform_pivot_x(b, 66, 0);
  lv_obj_set_style_transform_pivot_y(b, 66, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, x, 6);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(b, 4, 0);
  theme_label(b, &lv_font_montserrat_28, col, sym);
  theme_label(b, &lv_font_montserrat_14, COL_TEXT, title);
  theme_label(b, &lv_font_montserrat_12, COL_DIM, sub);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        s_sel = (int)(intptr_t)lv_event_get_user_data(e);
        g_settings.start_mode = s_sel;
        settings_save();
        if (s_on_pick) s_on_pick(s_sel);
      },
      LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  return b;
}

void modesel_create(void (*on_pick)(int)) {
  s_on_pick = on_pick;
  s_sel = g_settings.start_mode == 1 ? 1 : 0;
  lv_obj_t *scr = theme_screen();
  lv_obj_t *t = theme_label(scr, &lv_font_montserrat_20, COL_TEXT, "UberSDR Dial");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_t *q = theme_label(scr, &lv_font_montserrat_14, COL_DIM, "How shall we start?");
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 78);
  s_btn[0] = choice(scr, 0, LV_SYMBOL_WIFI, "RECEIVER", "Wi-Fi", -74);
  s_btn[1] = choice(scr, 1, LV_SYMBOL_BLUETOOTH, "CONTROLLER", "Bluetooth", 74);
  lv_obj_t *h = theme_label(scr, &lv_font_montserrat_12, COL_DIM, "Turn to choose  " LV_SYMBOL_BULLET "  tap to start");
  lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -58);
  highlight();
  ui_load(scr);
}

void modesel_tick(int knob) {
  if (!knob) return;
  s_sel = knob > 0 ? 1 : 0;
  highlight();
}
