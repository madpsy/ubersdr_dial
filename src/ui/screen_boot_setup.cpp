// Boot splash and Wi-Fi setup screens.
#include <WiFi.h>

#include "../app/settings.h"
#include "../app/wifi_setup.h"
#include "theme.h"
#include "ui.h"

void ui_load(lv_obj_t *scr);

static lv_obj_t *s_boot_status;

static lv_obj_t *brand(lv_obj_t *scr, int y) {
  lv_obj_t *t = theme_label(scr, &lv_font_montserrat_40, COL_TEXT, "UberSDR");
  lv_obj_align(t, LV_ALIGN_CENTER, 0, y);
  lv_obj_t *d = theme_label(scr, &lv_font_montserrat_14, COL_ACCENT, "D  I  A  L");
  lv_obj_align_to(d, t, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  return t;
}

void boot_create(const char *status) {
  lv_obj_t *scr = theme_screen();

  // Slow-spinning accent ring around the rim.
  lv_obj_t *sp = lv_spinner_create(scr);
  lv_obj_set_size(sp, 344, 344);
  lv_obj_center(sp);
  lv_spinner_set_anim_params(sp, 1600, 70);
  lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(sp, C(COL_EDGE), LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sp, C(COL_ACCENT), LV_PART_INDICATOR);

  brand(scr, -20);
  s_boot_status = theme_label(scr, &lv_font_montserrat_14, COL_DIM, status);
  lv_obj_set_width(s_boot_status, 240);
  lv_obj_set_style_text_align(s_boot_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_boot_status, LV_ALIGN_CENTER, 0, 60);
  ui_load(scr);
}

void boot_set_status(const char *status) { lv_label_set_text(s_boot_status, status); }

void setup_create(const char *reason) {
  lv_obj_t *scr = theme_screen();
  String ap = wifi_setup_ap_name();

  lv_obj_t *ring = lv_arc_create(scr);
  lv_obj_set_size(ring, 350, 350);
  lv_obj_center(ring);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_value(ring, 0);
  lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ring, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, C(COL_ACCENT2), LV_PART_MAIN);

  lv_obj_t *t = theme_label(scr, &lv_font_montserrat_20, COL_TEXT, LV_SYMBOL_WIFI "  Wi-Fi setup");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 38);

  if (reason && *reason) {
    lv_obj_t *r = theme_label(scr, &lv_font_montserrat_12, COL_WARN, reason);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 66);
  }

  // QR code joins the setup hotspot directly.
  String qr = "WIFI:T:nopass;S:" + ap + ";;";
  lv_obj_t *q = lv_qrcode_create(scr);
  lv_qrcode_set_size(q, 130);
  lv_qrcode_set_dark_color(q, C(COL_BG));
  lv_qrcode_set_light_color(q, C(0xffffff));
  lv_qrcode_update(q, qr.c_str(), qr.length());
  lv_obj_set_style_border_color(q, C(0xffffff), 0);
  lv_obj_set_style_border_width(q, 6, 0);
  lv_obj_align(q, LV_ALIGN_CENTER, 0, -8);

  String msg = "Join  " + ap + "\nthen open 192.168.4.1";
  lv_obj_t *m = theme_label(scr, &lv_font_montserrat_14, COL_DIM, msg.c_str());
  lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(m, LV_ALIGN_CENTER, 0, 92);
  ui_load(scr);
}
