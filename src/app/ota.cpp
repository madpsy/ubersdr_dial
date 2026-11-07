#include "ota.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "ota_certs.h"
#include "settings.h"
#include "wifi_setup.h"

#ifndef DIAL_VERSION
#define DIAL_VERSION "dev"
#endif

// Where releases are published. OTA_TEST_BASE (a build flag) points a test
// build at a local server instead; it may be plain http.
#ifdef OTA_TEST_BASE
static const char *kBase = OTA_TEST_BASE;
#else
static const char *kBase = "https://madpsy.github.io/ubersdr_dial/flash/";
#endif

static volatile OtaState s_state = OtaState::Idle;
static String s_error, s_latest, s_sha;
static uint32_t s_size = 0;
static volatile float s_progress = 0;
static bool s_join_wifi = false;
static bool s_trial = false;     // running on trial after an update (bootloader may roll back)
static bool s_marked = false;
static String s_bad;             // version that was installed and rolled back

// Arduino marks a freshly updated app valid before setup() unless told not
// to; ota_mark_good() does it once the dial has shown it works. C linkage:
// it overrides a weak C function in the core (esp32-hal-misc.c), and a C++
// definition would silently not.
extern "C" bool verifyRollbackLater() { return true; }

// ------------------------------------------------------------------ versions
static bool parse_ver(const String &s, int v[3]) {
  int n = sscanf(s.c_str(), "%d.%d.%d", &v[0], &v[1], &v[2]);
  return n == 3 && v[0] >= 0 && v[1] >= 0 && v[2] >= 0;
}

bool ota_version_known() {
  int v[3];
  return parse_ver(DIAL_VERSION, v);
}

// Is `remote` newer than what's running? A build without a release version
// (dev) takes any release.
static bool is_newer(const String &remote) {
  int r[3], c[3];
  if (!parse_ver(remote, r)) return false;
  if (!parse_ver(DIAL_VERSION, c)) return true;
  for (int i = 0; i < 3; i++)
    if (r[i] != c[i]) return r[i] > c[i];
  return false;
}

// ------------------------------------------------------------------ trial / rollback
void ota_boot() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  bool known = run && esp_ota_get_state_partition(run, &st) == ESP_OK;
  s_trial = known && st == ESP_OTA_IMG_PENDING_VERIFY;
  Preferences p;
  p.begin("ota", false);
  String tried = p.isKey("try") ? p.getString("try") : String();  // (getString logs an error when absent)
  if (!tried.isEmpty() && tried != DIAL_VERSION) {
    // We installed `tried` and restarted into it, but this is still the old
    // version: the bootloader rolled it back (or it never got going).
    Serial.printf("[ota] v%s was rolled back, running v%s\n", tried.c_str(), DIAL_VERSION);
    p.putString("bad", tried);
    p.remove("try");
  }
  s_bad = p.isKey("bad") ? p.getString("bad") : String();
  p.end();
  Serial.printf("[ota] v%s on %s (state %d)%s\n", DIAL_VERSION, run ? run->label : "?", known ? (int)st : -1,
                s_trial ? ", on trial" : "");
}

void ota_mark_good() {
  if (s_marked) return;
  s_marked = true;
  if (s_trial) {
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[ota] v%s confirmed (%s)\n", DIAL_VERSION, esp_err_to_name(e));
  }
  Preferences p;
  p.begin("ota", false);
  if (p.isKey("try")) p.remove("try");
  if (s_bad == DIAL_VERSION) {  // installed by hand since: it works after all
    p.remove("bad");
    s_bad = "";
  }
  p.end();
}

void ota_tick() {
  // Up and running this long without a reset: the display, input and main
  // loop work. (Picking a mode confirms sooner; see main.cpp.)
  if (!s_marked && millis() > 20000) ota_mark_good();
}

// ------------------------------------------------------------------ HTTP
// A minimal HTTP/1.1 GET with a hard deadline, headers parsed here, body read
// through body_read() (plain or chunked).
struct Http {
  WiFiClient plain;
  WiFiClientSecure tls;
  WiFiClient *c = nullptr;
  bool chunked = false;
  long chunk_left = 0;
  bool chunk_end = false;
  long length = -1;
  uint32_t deadline = 0;
  uint32_t stall_ms = 15000;

  bool expired() { return (int32_t)(millis() - deadline) >= 0; }

  bool line(String &out) {
    out = "";
    uint32_t last = millis();
    while (!expired() && millis() - last < stall_ms) {
      int ch = c->read();
      if (ch < 0) {
        if (!c->connected()) return false;
        delay(2);
        continue;
      }
      last = millis();
      if (ch == '\n') return true;
      if (ch != '\r' && out.length() < 512) out += (char)ch;
    }
    return false;
  }

  // Up to n bytes of body; 0 at the end, -1 on error or timeout.
  int raw(uint8_t *dst, size_t n) {
    uint32_t last = millis();
    while (!expired() && millis() - last < stall_ms) {
      int r = c->read(dst, n);
      if (r > 0) return r;
      if (!c->connected() && !c->available()) return 0;
      delay(2);
    }
    return -1;
  }

  int body_read(uint8_t *dst, size_t n) {
    if (!chunked) return raw(dst, n);
    if (chunk_end) return 0;
    if (chunk_left == 0) {
      String l;
      if (!line(l)) return -1;
      if (l.isEmpty() && !line(l)) return -1;  // CRLF after the previous chunk
      chunk_left = strtol(l.c_str(), nullptr, 16);
      if (chunk_left <= 0) {
        chunk_end = true;
        return 0;
      }
    }
    int r = raw(dst, min((long)n, chunk_left));
    if (r > 0) chunk_left -= r;
    return r == 0 ? -1 : r;  // closed mid-chunk
  }

  bool get(const String &url, uint32_t timeout_ms, String *err) {
    deadline = millis() + timeout_ms;
    bool secure = url.startsWith("https://");
    if (!secure && !url.startsWith("http://")) {
      *err = "bad URL";
      return false;
    }
    String rest = url.substring(secure ? 8 : 7);
    int slash = rest.indexOf('/');
    String host = slash < 0 ? rest : rest.substring(0, slash);
    String path = slash < 0 ? "/" : rest.substring(slash);
    uint16_t port = secure ? 443 : 80;
    int colon = host.indexOf(':');
    if (colon > 0) {
      port = host.substring(colon + 1).toInt();
      host = host.substring(0, colon);
    }
    if (secure) {
      tls.setCACert(kOtaRootCAs);  // verified: no setInsecure() here
      tls.setHandshakeTimeout(15);
      c = &tls;
    } else {
      c = &plain;
    }
    c->setTimeout(10);
    if (!c->connect(host.c_str(), port)) {
      *err = secure ? "couldn't connect securely" : "couldn't connect";
      return false;
    }
    c->printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: UberSDR-Dial/%s\r\nAccept-Encoding: identity\r\n"
              "Connection: close\r\n\r\n",
              path.c_str(), host.c_str(), DIAL_VERSION);
    String l;
    if (!line(l)) {
      *err = "no response";
      return false;
    }
    int sp = l.indexOf(' ');
    int code = sp > 0 ? l.substring(sp + 1).toInt() : 0;
    while (line(l) && l.length()) {
      l.toLowerCase();
      if (l.startsWith("transfer-encoding:") && l.indexOf("chunked") > 0) chunked = true;
      if (l.startsWith("content-length:")) length = l.substring(15).toInt();
    }
    if (code != 200) {
      *err = code ? "server said " + String(code) : "bad response";
      return false;
    }
    return true;
  }

  void stop() {
    if (c) c->stop();
  }
};

// ------------------------------------------------------------------ check
static bool fetch_info(String *err) {
  Http h;
  // A query string the CDN hasn't cached: GitHub Pages caches for 10 minutes.
  String url = String(kBase) + "ota.json?t=" + String(esp_random(), HEX);
  if (!h.get(url, 10000, err)) return false;  // inside the 12 s receiver start-up waits
  char buf[1024];
  size_t len = 0;
  while (len < sizeof(buf) - 1) {
    int r = h.body_read((uint8_t *)buf + len, sizeof(buf) - 1 - len);
    if (r < 0) {
      *err = "download failed";
      h.stop();
      return false;
    }
    if (r == 0) break;
    len += r;
  }
  h.stop();
  buf[len] = 0;
  JsonDocument doc;
  if (deserializeJson(doc, buf)) {
    *err = "bad ota.json";
    return false;
  }
  String ver = doc["version"] | "";
  String sha = doc["sha256"] | "";
  uint32_t size = doc["size"] | 0;
  int v[3];
  sha.toLowerCase();
  // Must fit the app slot this is going into, and the hash must be a hash.
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!parse_ver(ver, v) || sha.length() != 64 || size < 65536 || !next || size > next->size) {
    *err = "bad ota.json";
    return false;
  }
  for (char ch : sha)
    if (!isxdigit((unsigned char)ch)) {
      *err = "bad ota.json";
      return false;
    }
  s_latest = ver;
  s_sha = sha;
  s_size = size;
  return true;
}

static void check_task(void *) {
  String err;
  OtaState result = OtaState::Error;
  if (s_join_wifi && WiFi.status() != WL_CONNECTED) {
    if (g_settings.wifi_ssid.isEmpty()) err = "Set up Wi-Fi first";
    else if (!wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass, 20000)) err = "Couldn't join " + g_settings.wifi_ssid;
  }
  if (err.isEmpty()) {
    uint32_t t0 = millis();
    if (fetch_info(&err)) {
      result = is_newer(s_latest) ? OtaState::Available : OtaState::UpToDate;
      Serial.printf("[ota] latest v%s (%u bytes), running v%s: %s, in %lu ms\n", s_latest.c_str(), (unsigned)s_size,
                    DIAL_VERSION, result == OtaState::Available ? "update available" : "up to date",
                    (unsigned long)(millis() - t0));
    }
  }
  if (result == OtaState::Error) Serial.printf("[ota] check failed: %s\n", err.c_str());
  s_error = err;
  s_state = result;
  vTaskDelete(nullptr);
}

void ota_check_async(bool join_wifi) {
  if (s_state == OtaState::Checking || s_state == OtaState::Installing) return;
  s_join_wifi = join_wifi;
  s_error = "";
  s_state = OtaState::Checking;
  if (xTaskCreatePinnedToCore(check_task, "ota", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    s_error = "out of memory";
    s_state = OtaState::Error;
  }
}

// ------------------------------------------------------------------ install
static bool install(String *err) {
  ota_mark_good();  // installing over a trial version: keep it as the fallback
  Http h;
  h.stall_ms = 20000;
  // ?v= so the CDN can't hand out a cached older firmware.bin.
  String url = String(kBase) + "firmware.bin?v=" + s_latest;
  if (!h.get(url, 10 * 60 * 1000, err)) return false;
  if (h.length >= 0 && (uint32_t)h.length != s_size) {
    *err = "wrong size on server";
    h.stop();
    return false;
  }
  if (!Update.begin(s_size, U_FLASH)) {
    *err = String("can't start: ") + Update.errorString();
    h.stop();
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  static uint8_t buf[4096];
  uint32_t done = 0, t0 = millis();
  bool ok = true;
  while (done < s_size) {
    int r = h.body_read(buf, min((uint32_t)sizeof(buf), s_size - done));
    if (r <= 0) {
      *err = r == 0 ? "download cut short" : (h.expired() ? "download timed out" : "download stalled");
      ok = false;
      break;
    }
    mbedtls_sha256_update_ret(&sha, buf, r);
    if (Update.write(buf, r) != (size_t)r) {
      *err = String("write failed: ") + Update.errorString();
      ok = false;
      break;
    }
    done += r;
    s_progress = (float)done / s_size;
  }
  h.stop();
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (ok) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    if (s_sha != hex) {
      *err = "download didn't match (try again later)";
      Serial.printf("[ota] SHA-256 %s, expected %s\n", hex, s_sha.c_str());
      ok = false;
    }
  }
  if (!ok) {
    Update.abort();  // the other slot stays unbootable; nothing changes
    return false;
  }
  // Checks the image (header, its own SHA-256) and only then makes it the
  // one to boot.
  if (!Update.end()) {
    *err = String("image rejected: ") + Update.errorString();
    return false;
  }
  Preferences p;
  p.begin("ota", false);
  p.putString("try", s_latest);
  p.end();
  Serial.printf("[ota] v%s installed in %lu ms, restarting into it\n", s_latest.c_str(),
                (unsigned long)(millis() - t0));
  return true;
}

static void install_task(void *) {
  String err;
  bool ok = install(&err);
  if (!ok) Serial.printf("[ota] install failed: %s\n", err.c_str());
  s_error = err;
  s_state = ok ? OtaState::Done : OtaState::Error;
  vTaskDelete(nullptr);
}

void ota_install_async() {
  if (s_state != OtaState::Available) return;
  s_progress = 0;
  s_error = "";
  s_state = OtaState::Installing;
  if (xTaskCreatePinnedToCore(install_task, "ota", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    s_error = "out of memory";
    s_state = OtaState::Error;
  }
}

OtaState ota_state() { return s_state; }
const String &ota_error() { return s_error; }
const String &ota_latest() { return s_latest; }
bool ota_latest_failed() { return !s_latest.isEmpty() && s_latest == s_bad; }
float ota_progress() { return s_progress; }
