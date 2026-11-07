#pragma once

// Screen manager. Each screen owns its widgets and a tick() for knob input.
enum class Screen { None, Boot, Setup, Picker, Radio, ModeSelect, Controller, Update };

void ui_show_boot(const char *status);
void ui_boot_status(const char *status);
void ui_show_setup(const char *reason);
void ui_show_picker();
void ui_show_radio();
void ui_show_mode_select(void (*on_pick)(int mode));  // 0 receiver, 1 controller
void ui_show_controller();
// Firmware update (state from app/ota). `done` runs on Later / OK.
void ui_show_update(void (*done)());
void ui_tick();  // call every loop
Screen ui_current();

// Implemented per screen
void boot_create(const char *status);
void boot_set_status(const char *status);
void setup_create(const char *reason);
void picker_create();
void picker_tick(int knob);
void modesel_create(void (*on_pick)(int));
void modesel_tick(int knob);
// The dial screen (both modes): Radio = receiver mode (kLocalLink), Controller
// = Bluetooth or USB (kCtrlLink).
struct DialLink;
void controller_create(const DialLink *link);
void controller_tick(int knob);
void update_create(void (*done)());
void update_tick(int knob);
void update_debug_install();
