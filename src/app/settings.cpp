#include "settings.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

#include <memory>

Settings g_settings;
static Preferences s_prefs;

// Settings baked in by the web flasher (docs/flash/), in the raw "usercfg"
// partition: 8-byte magic, uint32 LE JSON length, JSON. Imported into NVS
// once, then the sector is erased so the password doesn't linger in flash and
// later changes (setup hotspot) stick. Same format as ubersdr_cyd's flasher.
static const char kUserCfgMagic[8] = {'U', 'B', 'S', 'D', 'R', 'C', 'F', '1'};
static const size_t kUserCfgMaxJson = 3072;

static void import_flasher_config() {
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "usercfg");
  if (!part) return;
  struct __attribute__((packed)) {
    char magic[8];
    uint32_t length;
  } hdr;
  if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) return;
  if (memcmp(hdr.magic, kUserCfgMagic, sizeof(kUserCfgMagic)) != 0) return;  // nothing baked in
  if (hdr.length > 0 && hdr.length <= kUserCfgMaxJson) {
    std::unique_ptr<char[]> buf(new char[hdr.length + 1]);
    if (esp_partition_read(part, sizeof(hdr), buf.get(), hdr.length) == ESP_OK) {
      buf[hdr.length] = 0;
      JsonDocument doc;
      if (!deserializeJson(doc, buf.get())) {
        String ssid = doc["wifi_ssid"] | "";
        String pass = doc["wifi_password"] | "";
        if (ssid.length() && ssid.length() <= 32 && pass.length() <= 64) {
          s_prefs.begin("dial", false);
          s_prefs.putString("ssid", ssid);
          s_prefs.putString("pass", pass);
          s_prefs.end();
          Serial.printf("[settings] flasher: imported Wi-Fi \"%s\"\n", ssid.c_str());
        }
      } else {
        Serial.println("[settings] flasher: bad JSON, discarded");
      }
    }
  }
  // Consume it whether or not it parsed, so it's never imported twice.
  esp_partition_erase_range(part, 0, SPI_FLASH_SEC_SIZE);
  Serial.println("[settings] flasher: settings slot erased");
}

void settings_load() {
  import_flasher_config();
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
  g_settings.saver_off_secs = s_prefs.getUShort("svo", 0);
  g_settings.sleep_mins = s_prefs.getUShort("slp", 30);
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
  s_prefs.putUShort("svo", g_settings.saver_off_secs);
  s_prefs.putUShort("slp", g_settings.sleep_mins);
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
