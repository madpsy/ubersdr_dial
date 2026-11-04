#include "wifi_setup.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "settings.h"

static WebServer *s_web = nullptr;
static DNSServer *s_dns = nullptr;
static String s_ap_name;
static String s_scan_options;
static uint32_t s_reboot_at = 0;

bool wifi_connect(const String &ssid, const String &pass, uint32_t timeout_ms, void (*pump)()) {
  if (ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ubersdr-dial");
  WiFi.setSleep(false);  // keeps WebSocket latency low
  // 20 MHz channel width: half the exposure to neighbouring networks than HT40,
  // and our streams need well under 1% of the bandwidth anyway.
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    if (pump) pump();
    delay(20);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_bandwidth_t bw;
    wifi_ps_type_t ps;
    esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    esp_wifi_get_ps(&ps);
    Serial.printf("[wifi] channel %d, %s, power save %s, rssi %d\n", WiFi.channel(), bw == WIFI_BW_HT20 ? "HT20" : "HT40",
                  ps == WIFI_PS_NONE ? "off" : "ON", (int)WiFi.RSSI());
  }
  return WiFi.status() == WL_CONNECTED;
}

static String html_escape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>UberSDR Dial setup</title><style>
body{font-family:system-ui,sans-serif;background:#05070d;color:#e5e7eb;margin:0;padding:24px}
.card{max-width:420px;margin:auto;background:#0f1424;border:1px solid #1f2a44;border-radius:16px;padding:24px}
h1{font-size:20px;margin:0 0 4px;color:#22d3ee}p{color:#9ca3af;font-size:14px}
label{display:block;margin:16px 0 6px;font-size:13px;color:#9ca3af}
input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #1f2a44;background:#05070d;color:#e5e7eb;font-size:16px}
button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:10px;background:#22d3ee;color:#05070d;font-weight:700;font-size:16px}
</style></head><body><div class=card>)";

static void handle_root() {
  String h = FPSTR(PAGE_HEAD);
  h += F("<h1>UberSDR Dial</h1><p>Connect the dial to your Wi-Fi network.</p>"
         "<form method=post action=/save><label>Network</label>"
         "<input name=ssid list=nets autocomplete=off required value=\"");
  h += html_escape(g_settings.wifi_ssid);
  h += F("\"><datalist id=nets>");
  h += s_scan_options;
  h += F("</datalist><label>Password</label><input name=pass type=password>"
         "<button>Save &amp; connect</button></form></div></body></html>");
  s_web->send(200, "text/html", h);
}

static void handle_save() {
  g_settings.wifi_ssid = s_web->arg("ssid");
  g_settings.wifi_pass = s_web->arg("pass");
  settings_save();
  String h = FPSTR(PAGE_HEAD);
  h += F("<h1>Saved</h1><p>The dial is restarting and joining <b>");
  h += html_escape(g_settings.wifi_ssid);
  h += F("</b>. You can close this page.</p></div></body></html>");
  s_web->send(200, "text/html", h);
  s_reboot_at = millis() + 1500;
}

void wifi_setup_start() {
  if (s_web) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  // Scan first so the form can offer nearby networks.
  int n = WiFi.scanNetworks();
  s_scan_options = "";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty() || s_scan_options.indexOf("\"" + html_escape(ssid) + "\"") >= 0) continue;
    s_scan_options += "<option value=\"" + html_escape(ssid) + "\">";
  }
  WiFi.scanDelete();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "UberSDR-Dial-%02X%02X", mac[4], mac[5]);
  s_ap_name = name;
  WiFi.softAP(name);
  delay(100);

  s_dns = new DNSServer();
  s_dns->start(53, "*", WiFi.softAPIP());
  s_web = new WebServer(80);
  s_web->on("/", HTTP_GET, handle_root);
  s_web->on("/save", HTTP_POST, handle_save);
  s_web->onNotFound([]() {  // captive portal: bounce everything to the form
    s_web->sendHeader("Location", "http://192.168.4.1/", true);
    s_web->send(302, "text/plain", "");
  });
  s_web->begin();
}

void wifi_setup_loop() {
  if (!s_web) return;
  s_dns->processNextRequest();
  s_web->handleClient();
  if (s_reboot_at && millis() > s_reboot_at) ESP.restart();
}

bool wifi_setup_active() { return s_web != nullptr; }
String wifi_setup_ap_name() { return s_ap_name; }
