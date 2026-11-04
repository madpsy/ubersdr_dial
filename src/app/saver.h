#pragma once
#include <stdint.h>

// Screen saver: after `timeout` with no knob turn or touch, dim the screen,
// turn it off, or stop drawing the spectrum/waterfall — the menu's choice.
// "Dim, then off" has two stages: dim after `saver_secs`, then off after
// `saver_off_secs` (both counted from the last activity; 0 = 3x the dim time).
// Any knob turn or touch wakes it; a touch that wakes a screen that was off
// is swallowed so it doesn't also press whatever was under the finger.
enum SaverMode : uint8_t { SAVER_DIM = 0, SAVER_OFF = 1, SAVER_SPECTRUM = 2, SAVER_NEVER = 3,
                         SAVER_DIM_OFF = 4 };  // values are stored in NVS: append only

void saver_tick();             // call every loop (after input has been read)
void saver_activity();         // something happened: wake / restart the timer
bool saver_active();
bool saver_spectrum_paused();  // screens skip spectrum/waterfall drawing
bool saver_screen_off();       // touch input should be swallowed to wake

const char *saver_mode_name(uint8_t mode);
static const uint16_t kSaverTimeouts[] = {30, 60, 120, 300, 600};
static constexpr int kNumSaverTimeouts = sizeof(kSaverTimeouts) / sizeof(kSaverTimeouts[0]);
