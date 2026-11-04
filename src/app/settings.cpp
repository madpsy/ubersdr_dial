#include "settings.h"

#include <Preferences.h>

Settings g_settings;
static Preferences s_prefs;

void settings_load() {
  s_prefs.begin("dial", true);
  g_settings.wifi_ssid = s_prefs.getString("ssid", "");
  g_settings.wifi_pass = s_prefs.getString("pass", "");
  g_settings.inst_url = s_prefs.getString("i_url", "");
  g_settings.inst_call = s_prefs.getString("i_call", "");
  g_settings.inst_name = s_prefs.getString("i_name", "");
  g_settings.freq_hz = s_prefs.getUInt("freq", g_settings.freq_hz);
  g_settings.span_hz = s_prefs.getUInt("span", g_settings.span_hz);
  g_settings.brightness = s_prefs.getUChar("bl", g_settings.brightness);
  g_settings.mode = s_prefs.getString("mode", "");
  g_settings.volume = s_prefs.getUChar("vol", g_settings.volume);
  g_settings.muted = s_prefs.getBool("mute", false);
  g_settings.squelch_db = s_prefs.getShort("sql", -999);
  g_settings.step_hz = s_prefs.isKey("stepu") ? s_prefs.getUInt("stepu", 0) : s_prefs.getUShort("step", 0);
  s_prefs.getBytes("bw", g_settings.bw_hz, sizeof(g_settings.bw_hz));
  g_settings.haptics = s_prefs.getBool("hap", true);
  g_settings.start_mode = s_prefs.getUChar("smode", 0);
  g_settings.ctrl_waterfall = s_prefs.getBool("cwf", true);
  g_settings.ctrl_wf_smooth = s_prefs.getBool("cwfs", false);
  g_settings.tune_locked = s_prefs.getBool("lock", false);
  g_settings.saver_mode = s_prefs.getUChar("svm", 0);
  g_settings.saver_secs = s_prefs.getUShort("svs", 120);
  s_prefs.end();
}

void settings_save() {
  s_prefs.begin("dial", false);
  s_prefs.putString("ssid", g_settings.wifi_ssid);
  s_prefs.putString("pass", g_settings.wifi_pass);
  s_prefs.putString("i_url", g_settings.inst_url);
  s_prefs.putString("i_call", g_settings.inst_call);
  s_prefs.putString("i_name", g_settings.inst_name);
  s_prefs.putUInt("freq", g_settings.freq_hz);
  s_prefs.putUInt("span", g_settings.span_hz);
  s_prefs.putUChar("bl", g_settings.brightness);
  s_prefs.putBool("hap", g_settings.haptics);
  s_prefs.putUChar("smode", g_settings.start_mode);
  s_prefs.putBool("cwf", g_settings.ctrl_waterfall);
  s_prefs.putBool("cwfs", g_settings.ctrl_wf_smooth);
  s_prefs.putBool("lock", g_settings.tune_locked);
  s_prefs.putUChar("svm", g_settings.saver_mode);
  s_prefs.putUShort("svs", g_settings.saver_secs);
  s_prefs.putString("mode", g_settings.mode);
  s_prefs.putUChar("vol", g_settings.volume);
  s_prefs.putBool("mute", g_settings.muted);
  s_prefs.putShort("sql", g_settings.squelch_db);
  s_prefs.putUInt("stepu", g_settings.step_hz);  // u32: 100 kHz steps
  s_prefs.putBytes("bw", g_settings.bw_hz, sizeof(g_settings.bw_hz));
  s_prefs.end();
}

void settings_save_view() {
  Serial.printf("[settings] saving view to flash at t=%lu\n", (unsigned long)millis());
  s_prefs.begin("dial", false);
  s_prefs.putUInt("freq", g_settings.freq_hz);
  s_prefs.putUInt("span", g_settings.span_hz);
  s_prefs.putString("mode", g_settings.mode);
  s_prefs.putUChar("vol", g_settings.volume);
  s_prefs.putBool("mute", g_settings.muted);
  s_prefs.putShort("sql", g_settings.squelch_db);
  s_prefs.putUInt("stepu", g_settings.step_hz);  // u32: 100 kHz steps
  s_prefs.putBytes("bw", g_settings.bw_hz, sizeof(g_settings.bw_hz));
  s_prefs.end();
}
