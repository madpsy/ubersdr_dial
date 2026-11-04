#include "battery_icon.h"

#include "../hw/battery.h"
#include "theme.h"

// Layout (children of the returned container):
//   [0] body outline  [1] fill (inside the body)  [2] nub  [3] bolt
static constexpr int BW = 22, BH = 11, INNER = BW - 4;

lv_obj_t *battery_icon_create(lv_obj_t *parent) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, BW + 3, BH);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *body = lv_obj_create(c);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, BW, BH);
  lv_obj_set_style_radius(body, 2, 0);
  lv_obj_set_style_border_width(body, 1, 0);
  lv_obj_set_style_border_color(body, C(COL_DIM), 0);
  lv_obj_align(body, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *fill = lv_obj_create(c);
  lv_obj_remove_style_all(fill);
  lv_obj_set_size(fill, INNER, BH - 4);
  lv_obj_set_style_radius(fill, 1, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(fill, C(COL_OK), 0);
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 2, 0);

  lv_obj_t *nub = lv_obj_create(c);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, 2, 5);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nub, C(COL_DIM), 0);
  lv_obj_align(nub, LV_ALIGN_LEFT_MID, BW, 0);

  lv_obj_t *bolt = theme_label(c, &lv_font_montserrat_12, COL_TEXT, LV_SYMBOL_CHARGE);
  lv_obj_align(bolt, LV_ALIGN_LEFT_MID, BW / 2 - 4, 0);
  lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);

  battery_icon_update(c);
  return c;
}

void battery_icon_update(lv_obj_t *c) {
  lv_obj_t *fill = lv_obj_get_child(c, 1);
  lv_obj_t *bolt = lv_obj_get_child(c, 3);
  int pct = battery_percent();
  bool chg = battery_charging();
  if (pct < 0) {
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);  // no reading yet
    return;
  }
  lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
  int w = INNER * pct / 100;
  lv_obj_set_width(fill, w < 2 ? 2 : w);
  uint32_t col = chg ? COL_ACCENT : (pct < 15 ? COL_BAD : (pct < 30 ? COL_WARN : COL_OK));
  lv_obj_set_style_bg_color(fill, C(col), 0);
  if (chg) lv_obj_remove_flag(bolt, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);
}
