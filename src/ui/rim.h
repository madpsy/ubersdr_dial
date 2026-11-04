#pragma once
#include <lvgl.h>

// The rim arc's SNR gradient (red -> amber -> green along the 270-degree
// arc), built once and shared by the radio and controller screens. Scale is
// the UberSDR clients': -5..30 dB.
static constexpr float RIM_SNR_MIN = -5, RIM_SNR_MAX = 30;
lv_draw_buf_t *rim_gradient();

// Rim arc styled like the radio screen's (352 px, 270 degrees from 7:30).
lv_obj_t *rim_arc_create(lv_obj_t *parent);
// Short thick marker arc (squelch threshold), hidden until placed.
lv_obj_t *rim_marker_create(lv_obj_t *parent);
// Put the marker at a fraction 0..1 along the rim arc.
void rim_marker_place(lv_obj_t *marker, float frac);
