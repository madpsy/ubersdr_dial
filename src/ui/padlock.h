#pragma once
#include <lvgl.h>

// Round padlock button (LVGL's symbol font has no padlock, so it is drawn:
// a rounded body and an arc shackle that opens when unlocked).
lv_obj_t *padlock_create(lv_obj_t *parent, int size);
void padlock_set(lv_obj_t *btn, bool locked);
