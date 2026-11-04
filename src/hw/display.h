#pragma once
#include <stdint.h>

// Minimal ST77916 QSPI driver (ESP-IDF spi_master) wired into LVGL v9.
void display_init();
void display_set_brightness(uint8_t pct);  // 0..100
// Bigger LVGL draw buffer (fewer render passes per frame). Only where internal
// RAM is free — controller mode, Wi-Fi off. Returns false if it couldn't.
bool display_use_buffer_lines(int lines);

// Layered mode (controller screen): LVGL draws the UI into a transparent
// full-screen layer and only redraws what changed in it; a background image
// (RGB565, LCD_W x LCD_H) is blended underneath by display_present(), which
// is all a moving waterfall costs — no LVGL redraw of the widgets on top.
// chunk_lines: height of each of the two composing DMA buffers (internal
// RAM). 30 when Wi-Fi is off; smaller in receiver mode, where the TLS sockets
// need the RAM (it frees the 30-line LVGL buffer, so 12 is a net saving).
bool display_layered_begin(int chunk_lines = 30);
bool display_layered();
// Background to show under the UI (nullptr = black). `stride_px` in pixels.
void display_set_background(const uint16_t *rgb565, int stride_px);
// Or: a function that returns background row y (LCD_W pixels, RGB565,
// 4-byte aligned) when it's being sent. It may build the row in `scratch`
// (internal RAM, LCD_W pixels) and return that. nullptr = none.
typedef const uint16_t *(*display_row_fn)(int y, uint16_t *scratch);
void display_set_background_rows(display_row_fn fn);
// Recomposite rows y1..y2 (background + UI) and send them to the panel.
void display_present(int y1, int y2);
// Same for just a rectangle (e.g. the bit of the rim meter that changed).
void display_present_rect(int x1, int y1, int x2, int y2);
// Preferred: mark the background changed here; display_flush_dirty() sends the
// union of everything marked, at most ~30 times a second.
void display_mark_dirty(int x1, int y1, int x2, int y2);
void display_flush_dirty();  // call once per loop
void display_debug_row(int y, int x0, int x1);  // debug dump (serial)
void display_debug_fill_red();  // debug: is the panel taking pixels at all?
void display_debug_fill(const uint16_t *cols, int n);  // debug: solid colour bands
void display_debug_reinit();    // debug: panel start-up commands again + full redraw
