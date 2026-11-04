#include "battery.h"

#include <Arduino.h>
#include <math.h>

static constexpr int PIN_BAT_ADC = 6;
static constexpr float DIVIDER = 2.0f;

static float s_v = NAN;            // smoothed volts
static float s_trend_ref = NAN;    // for "rising" detection
static uint32_t s_trend_at = 0, s_last = 0, s_last_log = 0;
static int s_rising = 0;           // >0 while the voltage keeps climbing

// Rough Li-ion discharge curve (resting volts -> %), as the ESPHome project
// calibrated it for this board: ~4.10 V full, ~3.20 V empty.
static const float kV[] = {3.20f, 3.45f, 3.60f, 3.68f, 3.75f, 3.82f, 3.90f, 3.98f, 4.05f, 4.10f};
static const int kP[] = {0, 5, 15, 25, 40, 55, 70, 82, 93, 100};

void battery_init() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
  pinMode(PIN_BAT_ADC, INPUT);
}

void battery_tick() {
  uint32_t now = millis();
  if (now - s_last < 250) return;
  s_last = now;
  // 64-sample oversample, calibrated millivolts from the ADC driver.
  uint32_t mv = 0;
  for (int i = 0; i < 64; i++) mv += analogReadMilliVolts(PIN_BAT_ADC);
  float v = (mv / 64.0f) / 1000.0f * DIVIDER;
  s_v = isnan(s_v) ? v : s_v * 0.9f + v * 0.1f;

  // Rising over a 20 s window -> charging.
  if (isnan(s_trend_ref)) {
    s_trend_ref = s_v;
    s_trend_at = now;
  } else if (now - s_trend_at > 20000) {
    float d = s_v - s_trend_ref;
    s_rising = d > 0.006f ? 3 : (d < -0.004f ? 0 : (s_rising > 0 ? s_rising - 1 : 0));
    s_trend_ref = s_v;
    s_trend_at = now;
  }
  if (now - s_last_log > 30000) {
    s_last_log = now;
    Serial.printf("[bat] %.3f V  %d%%  %s\n", s_v, battery_percent(), battery_charging() ? "charging/USB" : "battery");
  }
}

float battery_volts() { return s_v; }

int battery_percent() {
  if (isnan(s_v)) return -1;
  if (s_v <= kV[0]) return 0;
  const int n = sizeof(kV) / sizeof(kV[0]);
  for (int i = 1; i < n; i++) {
    if (s_v <= kV[i]) return kP[i - 1] + (int)((s_v - kV[i - 1]) / (kV[i] - kV[i - 1]) * (kP[i] - kP[i - 1]));
  }
  return 100;
}

// Charger float voltage (or USB with no cell) reads >= ~4.13 V; otherwise a
// voltage that keeps climbing means it's charging.
bool battery_charging() { return !isnan(s_v) && (s_v >= 4.13f || s_rising > 0); }
