// Receiver picker: knob scrolls the public instance list, tap LISTEN.
#include "../app/instances.h"
#include "../app/settings.h"
#include "../app/wifi_setup.h"
#include "fonts/fonts.h"
#include "theme.h"
#include "ui.h"

void ui_load(lv_obj_t *scr);

static lv_obj_t *s_roller, *s_detail, *s_avail, *s_listen, *s_spinner, *s_msg;
static FetchState s_seen = FetchState::Idle;

static String short_loc(const String &loc) {
  int c = loc.indexOf(',');
  return c > 0 ? loc.substring(0, c) : loc;
}

// Roller rows: the local receivers, then (only when there are both) a
// divider row, then the internet list. The divider can't be selected.
static int s_sep = -1;     // roller row of the divider, -1 = none
static int s_last_row = 0;  // to step over the divider in the direction of travel

static int row_to_index(int row) { return s_sep >= 0 && row > s_sep ? row - 1 : row; }
static int index_to_row(int i) { return s_sep >= 0 && i >= s_sep ? i + 1 : i; }
static int row_count() { return (int)instances_list().size() + (s_sep >= 0 ? 1 : 0); }

static void select_row(int row, lv_anim_enable_t anim) {
  int n = row_count();
  row = row < 0 ? 0 : (row >= n ? n - 1 : row);
  if (row == s_sep) row += row >= s_last_row ? 1 : -1;  // keep going the way we were
  if (row < 0) row = s_sep + 1;
  s_last_row = row;
  if ((int)lv_roller_get_selected(s_roller) != row) lv_roller_set_selected(s_roller, row, anim);
}

static void update_detail() {
  const auto &list = instances_list();
  uint32_t i = row_to_index(lv_roller_get_selected(s_roller));
  if (i >= list.size()) return;
  const Instance &in = list[i];
  String t = in.name + "\n" + in.location;
  lv_label_set_text(s_detail, t.c_str());
  if (in.local)
    lv_label_set_text_fmt(s_avail, "%d/%d free   " LV_SYMBOL_BULLET "   on this network", in.available,
                          in.max_clients);
  else
    lv_label_set_text_fmt(s_avail, "%d/%d free   " LV_SYMBOL_BULLET "   SNR %d dB", in.available, in.max_clients,
                          in.snr);
  lv_obj_set_style_text_color(s_avail, C(in.available > 0 ? COL_OK : COL_BAD), 0);
}

static void fill_list() {
  const auto &list = instances_list();
  int nlocal = instances_local_count();
  s_sep = nlocal > 0 && nlocal < (int)list.size() ? nlocal : -1;
  String opts;
  int sel = 0;
  for (size_t i = 0; i < list.size(); i++) {
    if (i) opts += "\n";
    if ((int)i == s_sep) opts += "\xE2\x80\x94\xE2\x80\x94  INTERNET  \xE2\x80\x94\xE2\x80\x94\n";  // em dashes
    if (list[i].local) opts += LV_SYMBOL_HOME "  ";
    opts += list[i].callsign + "  " + short_loc(list[i].location);
    if (list[i].url == g_settings.inst_url) sel = i;
  }
  lv_roller_set_options(s_roller, opts.c_str(), LV_ROLLER_MODE_NORMAL);
  s_last_row = 0;
  select_row(index_to_row(sel), LV_ANIM_OFF);
  lv_obj_remove_flag(s_roller, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_listen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_avail, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
  update_detail();
}

static void on_listen(lv_event_t *) {
  const auto &list = instances_list();
  uint32_t i = row_to_index(lv_roller_get_selected(s_roller));
  if (i >= list.size()) return;
  g_settings.inst_url = list[i].url;
  g_settings.inst_call = list[i].callsign;
  g_settings.inst_name = list[i].name;
  settings_save();
  ui_show_radio();
}

static void on_refresh(lv_event_t *) {
  instances_fetch_async();
  s_seen = FetchState::Idle;
}

static void on_wifi(lv_event_t *) {
  wifi_setup_start();
  ui_show_setup("");
}

static lv_obj_t *round_btn(lv_obj_t *scr, const char *sym, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(scr);
  lv_obj_set_size(b, 44, 44);
  lv_obj_set_style_radius(b, 22, 0);
  lv_obj_set_style_bg_color(b, C(COL_PANEL), 0);
  lv_obj_set_style_border_color(b, C(COL_EDGE), 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *l = theme_label(b, &lv_font_montserrat_16, COL_DIM, sym);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

void picker_create() {
  lv_obj_t *scr = theme_screen();

  lv_obj_t *t = theme_label(scr, &lv_font_montserrat_14, COL_ACCENT, "R E C E I V E R S");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);

  // Laid out from the bottom up: LISTEN, then the slots/SNR line over it, the
  // description over that, and the list takes whatever height is left. Nothing
  // depends on guessing how tall fonts or a row count come out.
  s_listen = theme_pill(scr, LV_SYMBOL_PLAY "  LISTEN", COL_ACCENT, 140, 44);
  lv_obj_align(s_listen, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_add_event_cb(s_listen, on_listen, LV_EVENT_CLICKED, nullptr);

  s_avail = theme_label(scr, &lv_font_montserrat_12, COL_OK, "");
  lv_obj_set_width(s_avail, 250);  // fixed, centred text: its contents change after it's placed
  lv_obj_set_style_text_align(s_avail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(s_avail, s_listen, LV_ALIGN_OUT_TOP_MID, 0, -8);

  s_detail = lv_label_create(scr);
  lv_obj_set_width(s_detail, 250);
  lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_detail, &font_latin_12, 0);
  lv_obj_set_height(s_detail, 2 * lv_font_get_line_height(&font_latin_12) + 2);  // name, location
  lv_obj_set_style_text_color(s_detail, C(COL_DIM), 0);
  lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(s_detail, s_avail, LV_ALIGN_OUT_TOP_MID, 0, -2);

  s_roller = lv_roller_create(scr);
  lv_obj_set_width(s_roller, 290);
  lv_obj_set_style_bg_opa(s_roller, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_roller, 0, 0);
  lv_obj_set_style_text_font(s_roller, &font_latin_16, 0);
  lv_obj_set_style_text_color(s_roller, C(COL_DIM), 0);
  lv_obj_set_style_text_align(s_roller, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(s_roller, 8, 0);
  lv_obj_set_style_bg_color(s_roller, C(COL_ACCENT), LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(s_roller, LV_OPA_20, LV_PART_SELECTED);
  lv_obj_set_style_radius(s_roller, 12, LV_PART_SELECTED);
  lv_obj_set_style_text_color(s_roller, C(COL_TEXT), LV_PART_SELECTED);
  lv_obj_set_style_text_font(s_roller, &font_latin_20, LV_PART_SELECTED);
  lv_obj_update_layout(scr);
  const int top = 50;  // under the title
  lv_obj_set_height(s_roller, lv_obj_get_y(s_detail) - 6 - top);
  lv_obj_align(s_roller, LV_ALIGN_TOP_MID, 0, top);
  lv_obj_add_event_cb(
      s_roller,
      [](lv_event_t *) {
        select_row(lv_roller_get_selected(s_roller), LV_ANIM_ON);  // a swipe that lands on the divider
        update_detail();
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_update_layout(scr);
  Serial.printf("[picker] roller y %d..%d, detail %d..%d, avail %d..%d, listen %d..%d\n", (int)lv_obj_get_y(s_roller),
                (int)lv_obj_get_y2(s_roller), (int)lv_obj_get_y(s_detail), (int)lv_obj_get_y2(s_detail),
                (int)lv_obj_get_y(s_avail), (int)lv_obj_get_y2(s_avail), (int)lv_obj_get_y(s_listen),
                (int)lv_obj_get_y2(s_listen));

  lv_obj_t *r = round_btn(scr, LV_SYMBOL_REFRESH, on_refresh);
  lv_obj_align(r, LV_ALIGN_BOTTOM_MID, -100, -50);
  lv_obj_t *w = round_btn(scr, LV_SYMBOL_WIFI, on_wifi);
  lv_obj_align(w, LV_ALIGN_BOTTOM_MID, 100, -50);

  s_spinner = lv_spinner_create(scr);
  lv_obj_set_size(s_spinner, 60, 60);
  lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, -20);
  lv_obj_set_style_arc_color(s_spinner, C(COL_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_spinner, C(COL_EDGE), LV_PART_MAIN);

  s_msg = theme_label(scr, &lv_font_montserrat_14, COL_DIM, "Fetching receivers...");
  lv_obj_set_width(s_msg, 240);
  lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, 40);

  lv_obj_add_flag(s_roller, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_listen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_avail, LV_OBJ_FLAG_HIDDEN);
  s_seen = FetchState::Idle;
  instances_fetch_async();  // always refresh: slot counts change minute to minute
  ui_load(scr);
}

void picker_tick(int knob) {
  FetchState st = instances_state();
  if (st != s_seen) {
    s_seen = st;
    if (st == FetchState::Done) {
      fill_list();
    } else if (st == FetchState::Error) {
      lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(s_msg, "Couldn't fetch receivers\n%s\nTap " LV_SYMBOL_REFRESH " to retry",
                            instances_error().c_str());
    } else if (st == FetchState::Loading) {
      lv_obj_add_flag(s_roller, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_listen, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_avail, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(s_msg, "Fetching receivers...");
    }
  }
  if (knob && st == FetchState::Done) {
    select_row((int)lv_roller_get_selected(s_roller) + knob, LV_ANIM_ON);
    update_detail();
  }
}
