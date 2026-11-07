#include "confirm.h"

#include <Arduino.h>

#include "../app/ota.h"
#include "theme.h"

static lv_obj_t **s_slot;

void confirm_restart_open(lv_obj_t *screen, lv_obj_t **slot) {
  confirm_restart_open(screen, slot, "Restart the dial?", "Back to choosing Receiver,\nBluetooth or USB",
                       LV_SYMBOL_REFRESH " Restart");
}

void confirm_restart_open(lv_obj_t *screen, lv_obj_t **slot, const char *title, const char *sub_txt,
                          const char *yes_txt) {
  if (*slot) lv_obj_delete(*slot);
  s_slot = slot;
  lv_obj_t *o = lv_obj_create(screen);
  *slot = o;
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 360, 360);
  lv_obj_center(o);
  lv_obj_set_style_bg_color(o, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = theme_label(o, &lv_font_montserrat_20, COL_TEXT, title);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -52);
  lv_obj_t *sub = theme_label(o, &lv_font_montserrat_14, COL_DIM, sub_txt);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, -14);
  lv_obj_t *no = theme_pill(o, "Cancel", COL_DIM, 110, 44);
  lv_obj_align(no, LV_ALIGN_CENTER, -62, 44);
  lv_obj_add_event_cb(
      no,
      [](lv_event_t *) {
        if (*s_slot) lv_obj_delete(*s_slot);
        *s_slot = nullptr;
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t *yes = theme_pill(o, yes_txt, COL_WARN, 110, 44);
  lv_obj_align(yes, LV_ALIGN_CENTER, 62, 44);
  lv_obj_add_event_cb(yes, [](lv_event_t *) {
        ota_mark_good();  // a restart chosen from the UI isn't a failed update
        ESP.restart();
      }, LV_EVENT_CLICKED, nullptr);
}
