#pragma once
#include <stdint.h>

// CST816 touch as an LVGL pointer, plus the rotary knob.
void input_init();

// Detents turned since the last call (+ = clockwise). Consumes the count.
int knob_take();
// Milliseconds since the last detent.
uint32_t knob_idle_ms();
// Milliseconds between the two most recent detents (spin speed).
uint32_t knob_gap_ms();
// Tuning acceleration for the last click, shared by receiver and controller
// modes: x1 for normal turning, x2 for brisk, x4 for a real spin.
int knob_accel();
// Milliseconds since the screen was last touched.
uint32_t touch_idle_ms();
