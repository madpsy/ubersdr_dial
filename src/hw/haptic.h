#pragma once

// DRV2605 haptic driver (I2C 0x5A on the touch bus). The original vendor
// firmware drives it ("task_haptic"); all calls are no-ops if it isn't found.
// Call only from the UI/loop task (shares the I2C bus with the touch chip).
void haptic_init();
bool haptic_present();
void haptic_enable(bool on);
void haptic_tick();   // knob detent: short, light
void haptic_click();  // screen touch: a little firmer
