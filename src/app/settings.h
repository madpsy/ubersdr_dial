#pragma once
#include <Arduino.h>

// Persistent settings (NVS via Preferences).
// Background behind the controller screen's widgets. Stored in NVS: append only.
enum CtrlBg : uint8_t { BG_OFF = 0, BG_WATERFALL = 1, BG_SPECTRUM = 2, BG_BOTH = 3 };

struct Settings {
  String wifi_ssid;
  String wifi_pass;
  // Last receiver used
  String inst_url;   // e.g. https://n2epe.tunnel.ubersdr.org/
  String inst_call;
  String inst_name;
  String favs;  // favourite receivers: UUIDs, comma separated
  uint32_t freq_hz = 14200000;
  uint32_t span_hz = 500000;
  uint8_t brightness = 80;
  String mode = "";      // "" = auto (LSB below 10 MHz, USB above)
  uint8_t volume = 60;   // 0..100
  bool muted = false;
  int16_t squelch_db = -999;  // audio gate min SNR (dB); -999 = off
  uint32_t step_hz = 0;       // tuning step; 0 = auto (from span)
  uint16_t bw_hz[8] = {0};
  bool haptics = true;
  uint8_t start_mode = 0;
  uint8_t ctrl_bg = BG_BOTH;  // background: off / waterfall / spectrum / both (CtrlBg)
  bool ctrl_wf_smooth = false; // its rate: false = battery (5 lines/s), true = smooth (10/s)
  uint8_t ctrl_meter = 0;     // controller screen's big readout: 0 off, 1 SNR, 2 dBFS, 3 S-meter (MeterMode)
  bool tune_locked = false;   // receiver mode: tuning lock (as v2's padlock)
  uint8_t saver_mode = 0;     // SaverMode: dim (default) / off / spectrum off / never / dim then off
  uint16_t saver_off_secs = 0;  // "dim, then off": idle time before the screen goes off; 0 = 3x saver_secs
  uint16_t sleep_mins = 30;  // deep sleep after this long idle (when nothing would be cut off); 0 = never
  uint16_t saver_secs = 120;  // idle time before the saver starts     // last startup choice: 0 receiver, 1 controller    // filter width per mode (index into the mode table); 0 = default
};

extern Settings g_settings;

void settings_load();
void settings_save();
void settings_save_view();  // just freq/span (called often, debounced by caller)
