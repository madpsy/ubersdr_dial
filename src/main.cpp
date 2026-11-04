// UberSDR Dial — standalone UberSDR receiver front-end for the Guition
// JC3636K718 round knob display.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <mbedtls/platform.h>

#include "app/audio.h"
#include "app/ble_ctrl.h"
#include "app/instances.h"
#include "app/saver.h"
#include "app/sdr.h"
#include "app/settings.h"
#include "app/wifi_setup.h"
#include "hw/battery.h"
#include "hw/display.h"
#include "hw/haptic.h"
#include "hw/input.h"
#include "hw/led_ring.h"
#include "ui/ui.h"

// The loop task only runs LVGL and the UI now (networking is on core 0).
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

static void pump_ui() { lv_timer_handler(); }

// mbedTLS's buffers in PSRAM. The framework is built to put every TLS
// allocation in internal RAM, ~40 KB a session; with Wi-Fi, the display's DMA
// buffers and audio there too, a second session (spectrum socket, /connection)
// failed with "SSL - Memory allocation failed" once that RAM had fragmented.
// Small allocations (bignums, contexts) stay internal for speed.
static void *tls_calloc(size_t n, size_t size) {
  void *p = nullptr;
  if (n * size >= 1024) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  return p;
}

static volatile int s_start_mode = -1;  // set by the startup chooser

static void enter_setup(const char *reason) {
  ui_boot_status("Starting setup hotspot...");
  pump_ui();
  wifi_setup_start();
  ui_show_setup(reason);
}

void setup() {
  Serial.begin(115200);
  // USB serial writes otherwise block up to 100 ms when no host is reading,
  // which stalls whichever task logs (audio/network) -> dropouts. Drop logs instead.
  Serial.setTxTimeoutMs(0);
  mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free);
  settings_load();

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  display_init();
  input_init();
  haptic_init();
  battery_init();
  haptic_enable(g_settings.haptics);
  led_ring_init();
  audio_init();
  sdr_init();

  ui_show_boot("");
  for (int i = 0; i < 10; i++) {  // let the first frame render before the backlight
    pump_ui();
    delay(5);
  }
  display_set_brightness(g_settings.brightness);

  // Asked every boot: be the radio (Wi-Fi) or drive the web UI (Bluetooth).
  ui_show_mode_select([](int mode) { s_start_mode = mode; });
}

// Receiver mode: join Wi-Fi, then resume the last receiver or pick one.
static void start_receiver() {
  // Same layered display as controller mode (see display.h), with small DMA
  // chunks: the two TLS sockets need internal RAM. Before Wi-Fi, so it gets
  // contiguous blocks.
  display_layered_begin(12);
  ui_show_boot("");
  pump_ui();
  if (g_settings.wifi_ssid.isEmpty()) {
    enter_setup("");
    return;
  }
  String msg = "Joining " + g_settings.wifi_ssid + "...";
  ui_boot_status(msg.c_str());
  if (!wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass, 20000, pump_ui)) {
    String why = "Couldn't join " + g_settings.wifi_ssid;
    enter_setup(why.c_str());
    return;
  }
  Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
  // The picker fetches the receiver list itself when shown; don't run a
  // second TLS session alongside the spectrum socket (internal RAM is tight).
  if (!g_settings.inst_url.isEmpty()) ui_show_radio();  // resume the last receiver
  else ui_show_picker();
}

// Controller mode: Bluetooth only. Wi-Fi stays off, so the two never share
// the radio.
static void start_controller() {
  WiFi.mode(WIFI_OFF);
  // Wi-Fi's off, so internal RAM is free: a 120-line draw buffer means a
  // quarter of the render passes over the UI for every waterfall line.
  // Layered UI (see display.h): the waterfall moves without LVGL redrawing
  // the widgets over it. Falls back to a bigger draw buffer if it can't.
  if (!display_layered_begin() && !display_use_buffer_lines(120)) display_use_buffer_lines(60);
  ble_ctrl_start();
  ui_show_controller();
}

// Serial debug commands: t = 2 s test tone, i = Wi-Fi/heap info.
static uint32_t s_render_pause_until = 0;
static bool s_quiet = false;  // debug: 'q' = no audio socket, no I2S clocks, LEDs off  // debug: 'p' pauses LVGL rendering 30 s

// Debug commands are whole lines of the form "!x" (send with printf '!t\n').
// A bare key used to be enough, but a host terminal with echo on bounced our
// own log text back ("[dbg] test tone" contains a 't'), retriggering commands
// in a loop - it played the test tone over the radio audio forever.
static int serial_command_key() {
  static char line[8];
  static int len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      int key = (!overflow && len == 2 && line[0] == '!') ? line[1] : -1;
      len = 0;
      overflow = false;
      if (key >= 0) return key;
      continue;
    }
    if (len < (int)sizeof(line)) line[len++] = (char)c;
    else overflow = true;
  }
  return -1;
}

static void serial_commands() {
  int c;
  while ((c = serial_command_key()) >= 0) {
    if (c == 't') {
      Serial.println("[dbg] test tone 2 s");
      audio_test_tone(2000);
    } else if (c == 'h') {
      extern bool g_dbg_hide_ui;
      g_dbg_hide_ui = !g_dbg_hide_ui;
      Serial.printf("[dbg] controller widgets %s\n", g_dbg_hide_ui ? "hidden" : "shown");
    } else if (c == 'q') {
      s_quiet = !s_quiet;
      Serial.printf("[dbg] quiet mode %s\n", s_quiet ? "ON (audio/I2S/LEDs off)" : "OFF");
      extern bool g_led_disabled;
      g_led_disabled = s_quiet;
      if (s_quiet) {
        audio_stop();
        led_ring_off();
      }
      audio_i2s_enable(!s_quiet);
    } else if (c == 'p') {
      Serial.println("[dbg] display updates paused for 30 s");
      s_render_pause_until = millis() + 30000;
    } else if (c == 'l') {
      Serial.println("[dbg] LED calibration: 0=red 1=green 2=blue 3=white");
      led_ring_calibrate();
    } else if (c == 'r') {
      // Row 180 crosses the rim's track on the left (x ~2..18).
      display_debug_row(180, 0, 22);
      display_debug_row(60, 20, 60);
    } else if (c == 'x') {
      display_debug_fill_red();
    } else if (c == 'c') {
      // Ring track (COL_EDGE), panel (COL_PANEL), background (COL_BG), mid grey.
      static const uint16_t cols[] = {0x1928, 0x0884, 0x0021, 0x8410};
      s_render_pause_until = millis() + 30000;
      display_debug_fill(cols, 4);
      Serial.println("[dbg] colour bands for 30 s: track, panel, background, grey");
    } else if (c == 'z') {
      display_debug_reinit();
    } else if (c == 'i') {
      Serial.printf("[dbg] rssi %d dBm, ch %d, internal heap %u, psram %u\n", (int)WiFi.RSSI(), WiFi.channel(),
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    }
  }
}

void loop() {
  serial_commands();
  if (s_start_mode >= 0) {
    int m = s_start_mode;
    s_start_mode = -1;
    if (m == 1) start_controller();
    else start_receiver();
  }
  static uint32_t m_ui = 0, m_other = 0, m_n = 0, m_at = 0;
  uint32_t m0 = micros();
  wifi_setup_loop();
  sdr_loop();
  battery_tick();
  uint32_t m1 = micros();
  ui_tick();
  uint32_t m2 = micros();
  saver_tick();
  m_other += (micros() - m2) + (m1 - m0);
  m_ui += m2 - m1;
  if (++m_n && millis() - m_at > 5000) {
    Serial.printf("[loop] ui_tick avg %lu us, other %lu us over %lu\n", (unsigned long)(m_ui / m_n),
                  (unsigned long)(m_other / m_n), (unsigned long)m_n);
    m_ui = m_other = m_n = 0;
    m_at = millis();
  }
  if ((int32_t)(s_render_pause_until - millis()) <= 0) {
    uint32_t t0 = micros();
    lv_timer_handler();
    uint32_t dt = micros() - t0;  // LVGL only; presents are timed separately
    display_flush_dirty();
    static uint32_t sum = 0, mx = 0, n = 0, last = 0;
    sum += dt;
    n++;
    if (dt > mx) mx = dt;
    if (millis() - last > 5000) {
      Serial.printf("[render] avg %lu us, max %lu us over %lu loops\n", (unsigned long)(sum / n), (unsigned long)mx,
                    (unsigned long)n);
      sum = mx = n = 0;
      last = millis();
    }
  }
  delay(2);
}
