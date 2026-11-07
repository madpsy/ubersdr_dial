// Startup chooser: Receiver (Wi-Fi, the dial is the radio) or Controller (the
// dial drives the UberSDR web UI, over Bluetooth or a USB cable). Asked on
// every boot; the last choice is pre-highlighted so a tap is all it takes.
#include "../app/ota.h"
#include "../app/settings.h"
#include "../app/wifi_setup.h"

#ifndef DIAL_VERSION
#define DIAL_VERSION "dev"
#endif
#include "device_menu.h"
#include "menu.h"
#include "theme.h"
#include "ui.h"

void ui_load(lv_obj_t *scr);

static constexpr int kChoices = 3;  // 0 receiver, 1 controller BLE, 2 controller USB
static lv_obj_t *s_btn[kChoices];
static int s_sel = 0;
static void (*s_on_pick)(int) = nullptr;
static lv_obj_t *s_scr, *s_overlay;  // the settings menu while open

static uint32_t choice_col(int i) { return i == 0 ? COL_ACCENT : COL_ACCENT2; }

static void highlight() {
  for (int i = 0; i < kChoices; i++) {
    bool on = i == s_sel;
    lv_obj_set_style_bg_opa(s_btn[i], on ? LV_OPA_30 : LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_btn[i], on ? 4 : 2, 0);
    lv_obj_set_style_border_color(s_btn[i], C(on ? choice_col(i) : COL_EDGE), 0);
    lv_obj_set_style_transform_scale(s_btn[i], on ? 272 : 236, 0);  // 256 = 1.0
  }
}

static lv_obj_t *choice(lv_obj_t *scr, int idx, const char *sym, const char *title, const char *sub, int x) {
  static constexpr int kSize = 100;
  uint32_t col = choice_col(idx);
  lv_obj_t *b = lv_button_create(scr);
  lv_obj_set_size(b, kSize, kSize);
  lv_obj_set_style_radius(b, kSize / 2, 0);
  lv_obj_set_style_bg_color(b, C(col), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_transform_pivot_x(b, kSize / 2, 0);
  lv_obj_set_style_transform_pivot_y(b, kSize / 2, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, x, 6);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(b, 3, 0);
  theme_label(b, &lv_font_montserrat_28, col, sym);
  theme_label(b, &lv_font_montserrat_12, COL_TEXT, title);
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

// Settings from here: Wi-Fi setup (for Receiver mode), then the dial-wide ones.
static void open_settings() {
  static MenuItem items[16];
  static int n = 0;
  if (!n) {
    items[n++] = {LV_SYMBOL_WIFI, "Wi-Fi setup", nullptr,
                  [](int) {
                    menu_close();
                    ui_show_boot("Starting setup hotspot...");
                    for (int i = 0; i < 25; i++) {  // let it show: the scan below blocks a few seconds
                      lv_timer_handler();
                      delay(10);
                    }
                    wifi_setup_start();
                    ui_show_setup("");
                  },
                  nullptr, 0};
    // Firmware update: joins the saved Wi-Fi, checks, offers what it finds.
    // Back here afterwards is a restart (Wi-Fi off again, as at start-up).
    items[n++] = {LV_SYMBOL_DOWNLOAD, "Firmware update", nullptr,
                  [](int) {
                    menu_close();
                    ota_check_async(true);
                    ui_show_update([] {
                      ota_mark_good();
                      ESP.restart();
                    });
                  },
                  nullptr, 0};
    static const MenuChoice kOnOff[] = {{"On", 1}, {"Off", 0}};
    items[n++] = {LV_SYMBOL_REFRESH, "Update check", [] { return (int)g_settings.ota_auto; },
                  [](int v) {
                    g_settings.ota_auto = v;
                    settings_save();
                  },
                  kOnOff, 2};
    for (int i = 0; i < kNumDeviceMenu && n < 16; i++) items[n++] = kDeviceMenu[i];
  }
  menu_open(s_scr, "S E T T I N G S", COL_TEXT, items, n, &s_overlay);
}

void modesel_create(void (*on_pick)(int)) {
  s_on_pick = on_pick;
  s_sel = g_settings.start_mode < kChoices ? g_settings.start_mode : 0;
  lv_obj_t *scr = theme_screen();
  s_scr = scr;
  s_overlay = nullptr;
  lv_obj_t *t = theme_label(scr, &lv_font_montserrat_20, COL_TEXT, "UberSDR Dial");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_t *q = theme_label(scr, &lv_font_montserrat_14, COL_DIM, "How shall we start?");
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 86);
  s_btn[0] = choice(scr, 0, LV_SYMBOL_WIFI, "RECEIVER", "Wi-Fi", -110);
  s_btn[1] = choice(scr, 1, LV_SYMBOL_BLUETOOTH, "CONTROLLER", "Bluetooth", 0);
  s_btn[2] = choice(scr, 2, LV_SYMBOL_USB, "CONTROLLER", "USB", 110);
  lv_obj_t *h = theme_label(scr, &lv_font_montserrat_12, COL_DIM, "Turn to choose  " LV_SYMBOL_BULLET "  tap to start");
  lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -80);
  lv_obj_t *v = theme_label(scr, &lv_font_montserrat_12, COL_DIM, "v" DIAL_VERSION);
  lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -60);
  // Settings before choosing: Wi-Fi setup and the dial-wide ones.
  lv_obj_t *gear = theme_pill(scr, LV_SYMBOL_SETTINGS, COL_DIM, 52, 40);
  lv_obj_align(gear, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_add_event_cb(
gear, [](lv_event_t *) { open_settings(); }, LV_EVENT_CLICKED, nullptr);
  highlight();
  ui_load(scr);
}

void modesel_tick(int knob) {
  if (!knob) return;
  if (s_overlay) {  // settings open: the knob scrolls it
    menu_knob(knob);
    return;
  }
  int s = s_sel + (knob > 0 ? 1 : -1);
  if (s < 0 || s >= kChoices) return;
  s_sel = s;
  highlight();
}
