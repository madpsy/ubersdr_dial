#include "ui.h"

#include <lvgl.h>

#include "../app/dial_link.h"
#include "../app/local_radio.h"
#include "../hw/display.h"
#include "../hw/haptic.h"
#include "../hw/input.h"

static Screen s_cur = Screen::None;

static void leaving() {
  if (s_cur == Screen::Radio) local_radio_stop();
  // The dial screen's background rows belong to it; other screens are opaque
  // and shouldn't pay for building them.
  if (s_cur == Screen::Radio || s_cur == Screen::Controller) display_set_background(nullptr, 0);
}

void ui_show_boot(const char *status) {
  leaving();
  s_cur = Screen::Boot;
  boot_create(status);
}

void ui_boot_status(const char *status) {
  if (s_cur == Screen::Boot) boot_set_status(status);
}

void ui_show_setup(const char *reason) {
  leaving();
  s_cur = Screen::Setup;
  setup_create(reason);
}

void ui_show_picker() {
  leaving();
  s_cur = Screen::Picker;
  picker_create();
}

void ui_show_radio() {
  leaving();
  s_cur = Screen::Radio;
  // Receiver mode is the controller's screen with the dial as the receiver.
  local_radio_start();
  controller_create(&kLocalLink);
}

void ui_show_mode_select(void (*on_pick)(int)) {
  leaving();
  s_cur = Screen::ModeSelect;
  modesel_create(on_pick);
}

void ui_show_controller() {
  leaving();
  s_cur = Screen::Controller;
  controller_create(&kBleLink);
}

Screen ui_current() { return s_cur; }

void ui_tick() {
  int k = knob_take();
  if (k) haptic_tick();
  switch (s_cur) {
    case Screen::Picker: picker_tick(k); break;
    case Screen::ModeSelect: modesel_tick(k); break;
    case Screen::Radio:  // same screen, local link
    case Screen::Controller: controller_tick(k); break;
    default: break;
  }
}

// Load a new screen with a quick fade and free the old one.
void ui_load(lv_obj_t *scr) { lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, true); }
