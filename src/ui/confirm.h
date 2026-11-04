#pragma once
#include <lvgl.h>

// "Restart the dial?" with Cancel / Restart, full screen over `screen`.
// Restarting goes back to the start-up choice (Receiver / Bluetooth / USB),
// which is only asked at boot. `slot` holds the overlay while it's open (any
// overlay already there is replaced); Cancel deletes it and nulls the slot.
void confirm_restart_open(lv_obj_t *screen, lv_obj_t **slot);
// The same with its own wording, e.g. "Skip Wi-Fi setup?" (still restarts).
void confirm_restart_open(lv_obj_t *screen, lv_obj_t **slot, const char *title, const char *sub, const char *yes);
