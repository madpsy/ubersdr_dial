#include "menu.h"

#include <Arduino.h>

#include <math.h>

#include "theme.h"

static lv_obj_t *s_screen, **s_slot, *s_list;
static const char *s_title;
static uint32_t s_title_col;
static const MenuItem *s_items;
static int s_n;
static int32_t s_scroll = 0;  // main list scroll, restored when coming back
static bool s_on_main = false;  // the main list is showing (else a setting's sheet)
// Open, as far as the caller knows. A setting may close the menu itself (and
// even rebuild the screen under it): then picking it mustn't reopen the list,
// which would land on the old screen and leave the new one's overlay set.
static bool s_open = false;

static void show_main();
void menu_close();

static lv_obj_t *new_overlay() {
  if (*s_slot) lv_obj_delete(*s_slot);
  lv_obj_t *o = lv_obj_create(s_screen);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 360, 360);
  lv_obj_center(o);
  lv_obj_set_style_bg_color(o, C(COL_BG), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  // A tap on the background, outside the list and buttons: closes the menu,
  // or from a setting's sheet goes back to the list (as its close button).
  // Presses that start on the list or a button are theirs, so scrolling the
  // list never lands here.
  lv_obj_add_event_cb(
      o,
      [](lv_event_t *e) {
        if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
        if (s_on_main) menu_close();
        else show_main();
      },
      LV_EVENT_SHORT_CLICKED, nullptr);
  *s_slot = o;
  s_list = nullptr;
  return o;
}

static void style_row(lv_obj_t *b) {
  lv_obj_set_style_bg_color(b, C(COL_PANEL), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(b, C(COL_TEXT), 0);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
  lv_obj_set_style_border_color(b, C(COL_EDGE), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_pad_ver(b, 10, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, C(COL_ACCENT), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
  lv_obj_set_style_outline_width(b, 0, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(b, 0, LV_STATE_FOCUS_KEY);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

static const char *value_label(const MenuItem &it) {
  if (!it.choices || !it.get) return nullptr;
  int v = it.get();
  for (int i = 0; i < it.n_choices; i++)
    if (it.choices[i].value == v) return it.choices[i].label;
  return "";
}

static lv_obj_t *close_button(lv_obj_t *o, int y, lv_event_cb_t cb) {
  lv_obj_t *x = lv_button_create(o);
  lv_obj_set_size(x, 44, 44);
  lv_obj_set_style_radius(x, 22, 0);
  lv_obj_set_style_bg_color(x, C(COL_PANEL), 0);
  lv_obj_set_style_border_color(x, C(COL_EDGE), 0);
  lv_obj_set_style_border_width(x, 2, 0);
  lv_obj_set_style_shadow_width(x, 0, 0);
  lv_obj_t *xl = theme_label(x, &lv_font_montserrat_16, COL_DIM, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);
  lv_obj_align(x, LV_ALIGN_CENTER, 0, y);
  lv_obj_add_event_cb(x, cb, LV_EVENT_CLICKED, nullptr);
  return x;
}

// A setting's own sheet.
static void show_sheet(int idx) {
  const MenuItem &it = s_items[idx];
  lv_obj_t *o = new_overlay();
  s_on_main = false;
  int cur = it.get ? it.get() : 0;
  if (it.n_choices <= 6) {
    // Round choices on the rim, title and back in the middle.
    // Three bands that can't collide: title on top, choices in the middle,
    // back at the bottom. 2-3 choices sit in one row, 4-6 in two rows; sizes
    // are picked so each row fits the round screen at its height.
    lv_obj_t *t = theme_label(o, &lv_font_montserrat_16, s_title_col, it.title);
    lv_obj_set_width(t, 220);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t *back = close_button(o, 0, [](lv_event_t *) { show_main(); });
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -28);

    int n = it.n_choices;
    int rows = n <= 3 ? 1 : 2;
    int per_row[2] = {rows == 1 ? n : (n + 1) / 2, rows == 1 ? 0 : n / 2};
    int size = n <= 2 ? 104 : (n == 3 ? 88 : (n == 4 ? 86 : 76));
    int gap = n <= 2 ? 24 : 12;
    int row_y[2] = {rows == 1 ? 4 : -46, 50};
    int k = 0;
    for (int r = 0; r < rows; r++) {
      int m = per_row[r];
      int total = m * size + (m - 1) * gap;
      for (int c = 0; c < m; c++, k++) {
        int i = k;
        bool on = it.choices[i].value == cur;
        lv_obj_t *b = lv_button_create(o);
        lv_obj_set_size(b, size, size);
        lv_obj_set_style_radius(b, size / 2, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_align(b, LV_ALIGN_CENTER, -total / 2 + size / 2 + c * (size + gap), row_y[r]);
        lv_obj_set_style_border_color(b, C(COL_ACCENT), 0);
        lv_obj_set_style_bg_color(b, C(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_10, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_t *l = theme_label(b, &lv_font_montserrat_14, on ? COL_BG : COL_TEXT, it.choices[i].label);
        lv_obj_set_width(l, size - 10);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_center(l);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
              intptr_t packed = (intptr_t)lv_event_get_user_data(e);
              const MenuItem &mi = s_items[packed >> 8];
              mi.set(mi.choices[packed & 0xFF].value);
              if (s_open) show_main();
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)((idx << 8) | i));
      }
    }
  } else {
    // A list, current one ticked.
    lv_obj_t *t = theme_label(o, &lv_font_montserrat_14, s_title_col, it.title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *list = lv_list_create(o);
    s_list = list;
    lv_obj_set_size(list, 230, 222);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    for (int i = 0; i < it.n_choices; i++) {
      bool on = it.choices[i].value == cur;
      lv_obj_t *b = lv_list_add_button(list, on ? LV_SYMBOL_OK : nullptr, it.choices[i].label);
      style_row(b);
      if (on) lv_obj_set_style_text_color(b, C(COL_ACCENT), 0);
      lv_obj_add_event_cb(
          b,
          [](lv_event_t *e) {
            intptr_t packed = (intptr_t)lv_event_get_user_data(e);
            const MenuItem &mi = s_items[packed >> 8];
            mi.set(mi.choices[packed & 0xFF].value);
            if (s_open) show_main();
          },
          LV_EVENT_CLICKED, (void *)(intptr_t)((idx << 8) | i));
      if (on) lv_obj_scroll_to_view(b, LV_ANIM_OFF);
    }
    close_button(o, 146, [](lv_event_t *) { show_main(); });
  }
}

static void show_main() {
  lv_obj_t *o = new_overlay();
  s_on_main = true;
  lv_obj_t *t = theme_label(o, &lv_font_montserrat_14, s_title_col, s_title);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_t *list = lv_list_create(o);
  s_list = list;
  lv_obj_set_size(list, 252, 250);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 6);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_row(list, 6, 0);
  for (int i = 0; i < s_n; i++) {
    const MenuItem &it = s_items[i];
    if (it.visible && !it.visible()) continue;
    lv_obj_t *b = lv_list_add_button(list, it.icon, it.title);
    style_row(b);
    // Current value under the title, dimmer.
    const char *val = value_label(it);
    if (val) {
      lv_obj_t *txt = lv_obj_get_child(b, -1);  // the title label
      lv_label_set_text(txt, (String(it.title) + "\n" + val).c_str());
      lv_obj_set_style_text_line_space(txt, 2, 0);
    }
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
          s_scroll = lv_obj_get_scroll_y(s_list);
          int idx = (int)(intptr_t)lv_event_get_user_data(e);
          const MenuItem &mi = s_items[idx];
          if (mi.choices) show_sheet(idx);
          else mi.set(0);  // an action
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
  lv_obj_t *b = lv_list_add_button(list, LV_SYMBOL_CLOSE, "Close");
  style_row(b);
  lv_obj_add_event_cb(b, [](lv_event_t *) { menu_close(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_update_layout(list);
  lv_obj_scroll_to_y(list, s_scroll, LV_ANIM_OFF);
}

void menu_open(lv_obj_t *screen, const char *title, uint32_t title_col, const MenuItem *items, int n,
               lv_obj_t **slot) {
  s_screen = screen;
  s_slot = slot;
  s_title = title;
  s_title_col = title_col;
  s_items = items;
  s_n = n;
  s_scroll = 0;
  s_open = true;
  show_main();
}

void menu_close() {
  s_open = false;
  if (s_slot && *s_slot) {
    lv_obj_delete(*s_slot);
    *s_slot = nullptr;
  }
  s_list = nullptr;
}

void menu_knob(int detents) {
  if (s_list && detents) lv_obj_scroll_by_bounded(s_list, 0, -detents * 50, LV_ANIM_ON);
}
