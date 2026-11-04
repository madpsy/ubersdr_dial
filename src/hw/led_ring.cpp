#include "led_ring.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "board.h"

static Adafruit_NeoPixel s_ring(LED_RING_COUNT, PIN_LED_RING, NEO_GRB + NEO_KHZ800);
static float s_level = -99;  // smoothed SNR shown
static uint32_t s_last = 0;
static bool s_on = false;

// Mirrors SNR_MIN/SNR_MAX and the colour ramp in the UberSDR clients
// (clients/electron/monitor/multi_monitor.js, static/v2/src/lib/format.js).
static constexpr float kMin = -5, kMax = 30, kAmber = 6, kGreen = 15;
static constexpr uint8_t kBright = 40;
// Geometry (LVGL angle convention: 0 deg = 3 o'clock, clockwise).
static constexpr float kLed0Deg = 270;    // LED 0 sits at 12 o'clock
static constexpr float kArcStart = 90;    // meter starts at 6 o'clock...
static constexpr float kArcSweep = 360;   // ...and fills clockwise all the way round  // of 255: the ring is bright at night

void led_ring_init() {
  s_ring.begin();
  s_ring.setBrightness(kBright);
  s_ring.clear();
  s_ring.show();
}

static uint32_t s_calib_until = 0;

// Debug: LED 0 red, LED 1 green, LED 2 blue, LED 3 white for 15 s, so the
// physical order/orientation of the ring can be read off.
void led_ring_calibrate() {
  s_calib_until = millis() + 15000;
  s_ring.clear();
  s_ring.setPixelColor(0, 255, 0, 0);
  s_ring.setPixelColor(1, 0, 255, 0);
  s_ring.setPixelColor(2, 0, 0, 255);
  s_ring.setPixelColor(3, 255, 255, 255);
  s_ring.show();
}

void led_ring_off() {
  if ((int32_t)(s_calib_until - millis()) > 0) return;
  if (!s_on) return;
  s_on = false;
  s_level = -99;
  s_ring.clear();
  s_ring.show();
}

bool g_led_disabled = false;  // debug: keep the ring dark
bool g_led_saver_off = false;

void led_ring_snr(float snr) {
  if (g_led_disabled || g_led_saver_off) return;
  if ((int32_t)(s_calib_until - millis()) > 0) return;
  if (isnan(snr)) {
    led_ring_off();
    return;
  }
  // Meter ballistics: fast attack, slow release.
  s_level = snr > s_level ? snr : s_level + (snr - s_level) * 0.15f;
  uint32_t now = millis();
  if (now - s_last < 50) return;  // ~20 fps is plenty
  s_last = now;
  s_on = true;
  float frac = (s_level - kMin) / (kMax - kMin);
  frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
  // Full-circle meter from 6 o'clock, clockwise (LVGL angle convention:
  // 0 = 3 o'clock, clockwise). Physically LED 0 is at 12 o'clock and the ring
  // runs clockwise.
  const float spacing = 360.0f / LED_RING_COUNT;
  const float lit_deg = frac * kArcSweep;
  // Segments light up to the level; every lit segment takes the colour of the
  // current SNR (red < 6 dB, amber 6-14 dB, green >= 15 dB), like the clients.
  uint8_t r, g, b;
  if (s_level >= kGreen) r = 34, g = 197, b = 94;
  else if (s_level >= kAmber) r = 251, g = 191, b = 36;
  else r = 239, g = 68, b = 68;
  for (int i = 0; i < LED_RING_COUNT; i++) {
    float ang = fmodf(kLed0Deg + i * spacing, 360.0f);
    float rel = fmodf(ang - kArcStart + 360.0f, 360.0f);  // position along the arc
    if (kArcSweep < 360 && rel > kArcSweep + spacing / 2) {  // outside a partial arc
      s_ring.setPixelColor(i, 0);
      continue;
    }
    float on = (lit_deg - rel) / spacing + 0.5f;  // partial for the leading LED
    on = on < 0 ? 0 : (on > 1 ? 1 : on);
    if (on <= 0) s_ring.setPixelColor(i, 2, 3, 6);  // faint track
    else s_ring.setPixelColor(i, r * on, g * on, b * on);
  }
  s_ring.show();
}
