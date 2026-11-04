#include "saver.h"

#include <Arduino.h>

#include "../hw/display.h"
#include "../hw/input.h"
#include "../hw/led_ring.h"
#include "settings.h"

// Where the saver is: awake, dimmed, or dark. Spectrum-off mode uses DIMMED as
// its one "active" stage (the screen stays lit, drawing pauses).
enum Stage : uint8_t { AWAKE, DIMMED, DARK };
static Stage s_stage = AWAKE;
static uint32_t s_last_activity = 0;

const char *saver_mode_name(uint8_t mode) {
  switch (mode) {
    case SAVER_DIM: return "Dim";
    case SAVER_OFF: return "Screen off";
    case SAVER_SPECTRUM: return "Spectrum off";
    case SAVER_DIM_OFF: return "Dim, then off";
    default: return "Never";
  }
}

static void wake() {
  if (s_stage == AWAKE) return;
  s_stage = AWAKE;
  display_set_brightness(g_settings.brightness);
  g_led_saver_off = false;
  Serial.println("[saver] wake");
}

static void dim() {
  s_stage = DIMMED;
  if (g_settings.saver_mode != SAVER_SPECTRUM)
    display_set_brightness(g_settings.brightness > 10 ? 10 : g_settings.brightness / 2);
}

static void dark() {
  s_stage = DARK;
  display_set_brightness(0);
  led_ring_off();
  g_led_saver_off = true;  // the ring is light too; audio carries on regardless
}

void saver_activity() {
  s_last_activity = millis();
  wake();
}

void saver_tick() {
  // Knob turns count as activity wherever they're consumed.
  if (knob_idle_ms() < 50 || touch_idle_ms() < 50) saver_activity();
  const uint8_t mode = g_settings.saver_mode;
  if (mode == SAVER_NEVER || s_stage == DARK) return;
  const uint32_t idle = millis() - s_last_activity;
  const uint32_t first = (uint32_t)g_settings.saver_secs * 1000;

  if (mode == SAVER_DIM_OFF) {
    // Off is counted from the last activity, not from dimming; an off time
    // no longer than the dim time just goes straight to off.
    const uint32_t off = g_settings.saver_off_secs ? (uint32_t)g_settings.saver_off_secs * 1000 : first * 3;
    if (idle >= off && idle >= first) {
      dark();
      Serial.printf("[saver] off after %u s idle\n", (unsigned)(idle / 1000));
    } else if (s_stage == AWAKE && idle >= first) {
      dim();
      Serial.printf("[saver] dim after %u s idle\n", (unsigned)(idle / 1000));
    }
    return;
  }

  if (s_stage != AWAKE || idle < first) return;
  if (mode == SAVER_OFF) dark();
  else dim();  // SAVER_DIM, or SAVER_SPECTRUM (brightness untouched, drawing pauses)
  Serial.printf("[saver] %s after %u s idle\n", saver_mode_name(mode), g_settings.saver_secs);
}

uint32_t saver_idle_ms() { return millis() - s_last_activity; }
bool saver_active() { return s_stage != AWAKE; }
bool saver_spectrum_paused() {
  return s_stage == DARK || (s_stage == DIMMED && g_settings.saver_mode == SAVER_SPECTRUM);
}
bool saver_screen_off() { return s_stage == DARK; }
