// LVGL v9 configuration for UberSDR Dial. Anything not set here takes the
// LVGL default from lv_conf_internal.h.
#pragma once

#define LV_COLOR_DEPTH 16

// Use the C library allocator; Arduino-ESP32 malloc() spills into PSRAM for
// large blocks, so LVGL objects/styles stay fast and canvases can be big.
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 200

#define LV_USE_OS LV_OS_NONE
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_DRAW_SW 1

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1  // frequency with the waterfall off
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_USE_CANVAS 1
#define LV_USE_ARC 1
#define LV_USE_ROLLER 1
#define LV_USE_LABEL 1
#define LV_USE_SPINNER 1
#define LV_USE_QRCODE 1
#define LV_USE_BUTTON 1
#define LV_USE_LIST 1

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
