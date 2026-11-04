#pragma once
#include <math.h>

// 13-LED WS2812 ring around the knob.
void led_ring_init();
// SNR meter, same scale as the UberSDR clients: -5..30 dB, red < 6 dB,
// amber 6-14 dB, green >= 15 dB. NAN = no reading (ring off).
void led_ring_snr(float snr_db);
void led_ring_off();
extern bool g_led_saver_off;  // screen saver "off": keep the ring dark
void led_ring_calibrate();  // debug: LEDs 0..3 = red, green, blue, white for 15 s
void led_ring_test_one(int idx);  // debug: just LED idx, white, for 15 s
