#include "padlock.h"

#include "theme.h"

// Children of the button: [0] shackle (arc), [1] body.
lv_obj_t *padlock_create(lv_obj_t *parent, int size) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, size / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_bg_color(b, C(COL_PANEL), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(b, 0, 0);

  lv_obj_t *shackle = lv_arc_create(b);
  lv_obj_remove_style(shackle, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(shackle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(shackle, 12, 12);
  lv_obj_set_style_arc_opa(shackle, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(shackle, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(shackle, false, LV_PART_INDICATOR);
  lv_arc_set_bg_angles(shackle, 0, 360);

  lv_obj_t *body = lv_obj_create(b);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 16, 11);
  lv_obj_set_style_radius(body, 2, 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 4);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

  padlock_set(b, false);
  return b;
}

void padlock_set(lv_obj_t *b, bool locked) {
  lv_obj_t *shackle = lv_obj_get_child(b, 0);
  lv_obj_t *body = lv_obj_get_child(b, 1);
  uint32_t col = locked ? COL_WARN : COL_DIM;
  lv_obj_set_style_border_color(b, C(locked ? COL_WARN : COL_EDGE), 0);
  lv_obj_set_style_bg_color(b, C(locked ? COL_WARN : COL_PANEL), 0);
  lv_obj_set_style_bg_opa(b, locked ? LV_OPA_20 : LV_OPA_COVER, 0);
  lv_obj_set_style_arc_color(shackle, C(col), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(body, C(col), 0);
  if (locked) {
    // Closed: a full top half-loop sitting on the body.
    lv_arc_set_angles(shackle, 180, 360);
    lv_obj_align(shackle, LV_ALIGN_CENTER, 0, -3);
  } else {
    // Open: lifted, with the left leg free.
    lv_arc_set_angles(shackle, 200, 360);
    lv_obj_align(shackle, LV_ALIGN_CENTER, 2, -6);
  }
}
