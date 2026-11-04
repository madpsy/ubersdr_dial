#include "saver.h"

#include <Arduino.h>

#include "../hw/display.h"
#include "../hw/input.h"
#include "../hw/led_ring.h"
#include "settings.h"

static bool s_active = false;
static uint32_t s_last_activity = 0;

const char *saver_mode_name(uint8_t mode) {
  switch (mode) {
    case SAVER_DIM: return "Dim";
    case SAVER_OFF: return "Screen off";
    case SAVER_SPECTRUM: return "Spectrum off";
    default: return "Never";
  }
}

static void wake() {
  if (!s_active) return;
  s_active = false;
  display_set_brightness(g_settings.brightness);
  g_led_saver_off = false;
  Serial.println("[saver] wake");
}

void saver_activity() {
  s_last_activity = millis();
  wake();
}

void saver_tick() {
  // Knob turns count as activity wherever they're consumed.
  if (knob_idle_ms() < 50 || touch_idle_ms() < 50) saver_activity();
  if (s_active || g_settings.saver_mode == SAVER_NEVER) return;
  if (millis() - s_last_activity < (uint32_t)g_settings.saver_secs * 1000) return;
  s_active = true;
  switch (g_settings.saver_mode) {
    case SAVER_DIM:
      display_set_brightness(g_settings.brightness > 10 ? 10 : g_settings.brightness / 2);
      break;
    case SAVER_OFF:
      display_set_brightness(0);
      led_ring_off();
      g_led_saver_off = true;  // the ring is light too; audio carries on regardless
      break;
    default:
      break;  // SAVER_SPECTRUM: screen stays as it is, drawing pauses
  }
  Serial.printf("[saver] %s after %u s idle\n", saver_mode_name(g_settings.saver_mode), g_settings.saver_secs);
}

bool saver_active() { return s_active; }
bool saver_spectrum_paused() {
  return s_active && (g_settings.saver_mode == SAVER_OFF || g_settings.saver_mode == SAVER_SPECTRUM);
}
bool saver_screen_off() { return s_active && g_settings.saver_mode == SAVER_OFF; }
