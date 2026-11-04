#pragma once
#include <lvgl.h>

// Phone-style battery indicator: outline + fill level + terminal nub, a bolt
// while on charge, red below 15%. Call battery_icon_update() every so often.
lv_obj_t *battery_icon_create(lv_obj_t *parent);
void battery_icon_update(lv_obj_t *icon);
