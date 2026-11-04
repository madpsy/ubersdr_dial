#pragma once
#include <stdint.h>

// Deep sleep: after `sleep_mins` with no knob turn or touch, and only when
// nothing would be cut off (see the `ok` callback), the dial plays a short
// "going to sleep" animation and powers down to microamps. A knob turn (or a
// touch) wakes it; waking is a restart, which goes straight back into the mode
// it was in. A turn or touch during the animation cancels it.
void sleep_boot();                     // first thing in setup(): release pins held through sleep
int sleep_woke_mode();                 // mode to resume after waking (0/1/2), or -1 for the start-up choice
void sleep_set_ok(bool (*ok)());       // may we sleep now? (e.g. no page connected)
void sleep_set_resume(int (*mode)());  // mode to come back to (-1 = start-up choice)
void sleep_tick();                     // call every loop, after saver_tick()
void sleep_start();                    // debug / "now": run the animation, then sleep
bool sleep_pending();                  // animation running
const char *sleep_wake_note();         // what woke us last (debug)
void sleep_debug_touch_wake(bool on);  // debug: leave touch out of the wake pins
