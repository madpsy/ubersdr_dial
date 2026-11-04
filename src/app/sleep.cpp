#include "sleep.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "../hw/board.h"
#include "../hw/display.h"
#include "../hw/input.h"
#include "../hw/led_ring.h"
#include "../ui/sleep_anim.h"
#include "saver.h"
#include "settings.h"

// Survives deep sleep (not a power cut): which mode to resume on waking.
static RTC_DATA_ATTR int s_resume = -1;
static int s_woke_mode = -1;
static String s_wake_note = "cold boot";
static bool s_touch_wake = true;  // debug: knob-only wake to compare
static bool (*s_ok)() = nullptr;
static int (*s_mode)() = nullptr;
static uint32_t s_anim_at = 0;  // 0 = not animating
static constexpr uint32_t kAnimMs = 3400;

// Pins held at a level through sleep: backlight off, DAC muted, touch out of reset.
static const gpio_num_t kHeld[] = {(gpio_num_t)PIN_LCD_BL, (gpio_num_t)PIN_DAC_MUTE, (gpio_num_t)PIN_TP_RST};
// Wake: either knob pin (each detent pulses one LOW) or the touch interrupt.
static const gpio_num_t kWake[] = {(gpio_num_t)PIN_KNOB_CW, (gpio_num_t)PIN_KNOB_CCW, (gpio_num_t)PIN_TP_INT};

void sleep_boot() {
  for (gpio_num_t p : kHeld) gpio_hold_dis(p);
  gpio_deep_sleep_hold_dis();
  for (gpio_num_t p : kWake) rtc_gpio_deinit(p);
  esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
  if (why == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    const char *by = (pins >> PIN_TP_INT) & 1 ? "touch" : ((pins >> PIN_KNOB_CW) & 1 || (pins >> PIN_KNOB_CCW) & 1) ? "knob" : "?";
    char b[96];
    snprintf(b, sizeof(b), "woke by %s (pins 0x%llx), resuming mode %d", by, (unsigned long long)pins, s_resume);
    s_wake_note = b;
    Serial.printf("[sleep] %s\n", b);
    s_woke_mode = s_resume;
  }
  s_resume = -1;
}

int sleep_woke_mode() { return s_woke_mode; }
const char *sleep_wake_note() { return s_wake_note.c_str(); }
void sleep_debug_touch_wake(bool on) { s_touch_wake = on; }
void sleep_set_ok(bool (*ok)()) { s_ok = ok; }
void sleep_set_resume(int (*mode)()) { s_mode = mode; }
bool sleep_pending() { return s_anim_at != 0; }

static void sleep_now() {
  s_resume = s_mode ? s_mode() : -1;
  Serial.printf("[sleep] going to sleep (resume mode %d)\n", s_resume);
  led_ring_off();
  digitalWrite(PIN_DAC_MUTE, LOW);
  display_sleep();  // panel off, backlight pin LOW
  touch_sleep();    // touch chip to its low-power scan
  delay(20);        // let the LED ring's last frame and the panel commands land

  for (gpio_num_t p : kHeld) gpio_hold_en(p);
  gpio_deep_sleep_hold_en();
  uint64_t mask = 0;
  for (gpio_num_t p : kWake) {
    if (p == (gpio_num_t)PIN_TP_INT && !s_touch_wake) continue;
    Serial.printf("[sleep] wake pin %d reads %d\n", (int)p, digitalRead(p));
    rtc_gpio_init(p);
    rtc_gpio_set_direction(p, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(p);
    rtc_gpio_pullup_en(p);
    mask |= 1ULL << p;
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // keeps those pull-ups on
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  Serial.flush();
  esp_deep_sleep_start();
}

void sleep_start() {
  if (s_anim_at) return;
  Serial.println("[sleep] animation");
  display_set_brightness(g_settings.brightness < 40 ? 40 : g_settings.brightness);  // even if the saver had it dark
  sleep_anim_start();
  s_anim_at = millis();
  if (!s_anim_at) s_anim_at = 1;
}

void sleep_tick() {
  if (s_anim_at) {
    uint32_t t = millis() - s_anim_at;
    if (saver_idle_ms() < t && t > 150) {  // turned or touched since it started: stay awake
      Serial.println("[sleep] cancelled");
      sleep_anim_stop();
      s_anim_at = 0;
      saver_activity();  // restores the brightness
      return;
    }
    if (t >= kAnimMs) sleep_now();
    return;
  }
  if (!g_settings.sleep_mins) return;
  if (saver_idle_ms() < (uint32_t)g_settings.sleep_mins * 60000UL) return;
  if (s_ok && !s_ok()) return;
  sleep_start();
}
