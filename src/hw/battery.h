#pragma once
#include <stdint.h>

// Li-ion battery on GPIO6 through a ~10k/10k divider (vendor demo pincfg.h).
// There is no charge-status pin, so "on charger" is inferred from the voltage:
// at the charger's float level, or climbing. Readings are heavily averaged —
// the charger's ripple is otherwise visible as noise.
void battery_init();
void battery_tick();      // call from the loop; samples ~every 250 ms
float battery_volts();    // smoothed, NAN until the first reading
int battery_percent();    // 0..100, -1 until known
bool battery_charging();  // on USB power / charging (heuristic)
