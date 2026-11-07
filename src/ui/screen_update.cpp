// Firmware update screen: checking, "update available" (Install / Later),
// progress round the rim, then a restart into the new version.
#include "../app/ota.h"
#include "theme.h"
#include "ui.h"

#ifndef DIAL_VERSION
#define DIAL_VERSION "dev"
#endif

void ui_load(lv_obj_t *scr);

static lv_obj_t *s_spin, *s_arc, *s_title, *s_big, *s_body, *s_left, *s_right;
static OtaState s_shown;
static bool s_first;
static void (*s_done)() = nullptr;
static uint32_t s_restart_at = 0;
static int s_pct = -1;

static void set_pill(lv_obj_t *b, const char *txt, bool show) {
  if (!show) {
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(lv_obj_get_child(b, 0), txt);
}

static void finish() {
  void (*d)() = s_done;
  s_done = nullptr;  // once
  if (d) d();
}

static void show(OtaState st) {
  s_shown = st;
  bool busy = st == OtaState::Checking || st == OtaState::Idle;
  bool installing = st == OtaState::Installing || st == OtaState::Done;
  if (busy) lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
  if (installing) lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
  // The left pill is "Later" / "OK" (finish), the right one "Install".
  set_pill(s_left, "", false);
  set_pill(s_right, "", false);
  lv_obj_set_style_text_color(s_big, C(COL_TEXT), 0);
  String big, body;
  switch (st) {
    case OtaState::Idle:
    case OtaState::Checking:
      big = "";
      body = "Checking for updates...";
      break;
    case OtaState::UpToDate:
      big = "v" DIAL_VERSION;
      body = "This is the latest version.";
      set_pill(s_left, "OK", true);
      break;
    case OtaState::Available:
      Serial.printf("[ota] offering v%s%s\n", ota_latest().c_str(), ota_latest_failed() ? " (rolled back before)" : "");
      big = "v" DIAL_VERSION "  " LV_SYMBOL_RIGHT "  v" + ota_latest();
      body = ota_latest_failed() ? "This version didn't start last time\nand was rolled back. Try again?"
                                 : "A new version is available.\nSettings are kept.";
      set_pill(s_left, "Later", true);
      set_pill(s_right, LV_SYMBOL_DOWNLOAD " Install", true);
      break;
    case OtaState::Installing:
      big = "0%";
      s_pct = 0;
      body = "Installing v" + ota_latest() + "\nThe dial keeps working if\nthis is interrupted.";
      break;
    case OtaState::Done:
      big = LV_SYMBOL_OK;
      lv_obj_set_style_text_color(s_big, C(COL_OK), 0);
      body = "Installed. Restarting...";
      lv_arc_set_value(s_arc, 1000);
      s_restart_at = millis() + 1500;
      break;
    case OtaState::Error:
      big = LV_SYMBOL_WARNING;
      lv_obj_set_style_text_color(s_big, C(COL_WARN), 0);
      body = "Update failed:\n" + ota_error();
      set_pill(s_left, "OK", true);
      break;
  }
  lv_label_set_text(s_big, big.c_str());
  lv_label_set_text(s_body, body.c_str());
  // One button: centred. Two: side by side.
  bool two = !lv_obj_has_flag(s_right, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(s_left, LV_ALIGN_CENTER, two ? -62 : 0, 92);
}

void update_create(void (*done)()) {
  s_done = done;
  s_restart_at = 0;
  s_pct = -1;
  lv_obj_t *scr = theme_screen();

  s_spin = lv_spinner_create(scr);
  lv_obj_set_size(s_spin, 344, 344);
  lv_obj_center(s_spin);
  lv_spinner_set_anim_params(s_spin, 1600, 70);
  lv_obj_set_style_arc_width(s_spin, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_spin, C(COL_EDGE), LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_spin, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_spin, C(COL_ACCENT), LV_PART_INDICATOR);

  s_arc = lv_arc_create(scr);
  lv_obj_set_size(s_arc, 344, 344);
  lv_obj_center(s_arc);
  lv_arc_set_rotation(s_arc, 270);
  lv_arc_set_bg_angles(s_arc, 0, 360);
  lv_arc_set_range(s_arc, 0, 1000);
  lv_arc_set_value(s_arc, 0);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_arc, C(COL_EDGE), LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, C(COL_ACCENT), LV_PART_INDICATOR);

  s_title = theme_label(scr, &lv_font_montserrat_20, COL_TEXT, LV_SYMBOL_DOWNLOAD "  Firmware update");
  lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -92);
  s_big = theme_label(scr, &lv_font_montserrat_28, COL_TEXT, "");
  lv_obj_align(s_big, LV_ALIGN_CENTER, 0, -42);
  s_body = theme_label(scr, &lv_font_montserrat_14, COL_DIM, "");
  lv_obj_set_width(s_body, 270);
  lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 18);

  s_left = theme_pill(scr, "", COL_DIM, 110, 44);
  lv_obj_add_event_cb(s_left, [](lv_event_t *) { finish(); }, LV_EVENT_CLICKED, nullptr);
  s_right = theme_pill(scr, "", COL_ACCENT, 120, 44);
  lv_obj_align(s_right, LV_ALIGN_CENTER, 66, 92);
  lv_obj_add_event_cb(
      s_right, [](lv_event_t *) { ota_install_async(); }, LV_EVENT_CLICKED, nullptr);

  show(ota_state());
  ui_load(scr);
}

// Debug (serial "!y"): press Install.
void update_debug_install() {
  if (s_shown == OtaState::Available) ota_install_async();
}

void update_tick(int) {
  OtaState st = ota_state();
  if (st != s_shown) show(st);
  if (st == OtaState::Installing) {
    int pct = (int)(ota_progress() * 100);
    lv_arc_set_value(s_arc, (int)(ota_progress() * 1000));
    if (pct != s_pct) {
      s_pct = pct;
      lv_label_set_text_fmt(s_big, "%d%%", pct);
    }
  }
  if (s_restart_at && (int32_t)(millis() - s_restart_at) >= 0) ESP.restart();
}
