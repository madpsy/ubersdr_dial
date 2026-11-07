#include "wifi_setup.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <ArduinoJson.h>

#include <vector>

#include "ota.h"
#include "settings.h"

static WebServer *s_web = nullptr;
static DNSServer *s_dns = nullptr;
static String s_ap_name;
// Last scan, strongest first, one entry per name.
struct Net {
  String ssid;
  int rssi;
  bool secure;
};
static std::vector<Net> s_nets;
static bool s_scanning = false;
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

// The form: nearby networks to tap (with signal and a padlock), a Rescan, and
// the name box for anything not listed (hidden networks).
static const char PAGE_FORM[] PROGMEM = R"(<h1>UberSDR Dial</h1><p>Connect the dial to your Wi-Fi network.</p>
<style>
.nets{margin-top:12px;border:1px solid #1f2a44;border-radius:10px;overflow:hidden}
.net{display:flex;align-items:center;gap:10px;width:100%;margin:0;padding:12px;border:0;border-bottom:1px solid #1f2a44;
 border-radius:0;background:#05070d;color:#e5e7eb;font-weight:400;font-size:16px;text-align:left}
.net:last-child{border-bottom:0}.net.on{background:#0e3a44}
.net .n{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.net .m{color:#9ca3af;font-size:13px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}.bars i{width:4px;background:#374151;border-radius:1px}
.bars i.f{background:#22d3ee}
.row{display:flex;justify-content:space-between;align-items:center;margin-top:16px}
.row label{margin:0}.link{width:auto;margin:0;padding:6px 12px;background:#1f2a44;color:#e5e7eb;font-weight:400;font-size:14px}
#st{padding:12px;color:#9ca3af;font-size:14px}
</style>
<div class=row><label>Nearby networks</label><button type=button class=link id=rs>Rescan</button></div>
<div class=nets id=nets><div id=st>Scanning...</div></div>
<form method=post action=/save><label>Network name</label>
<input name=ssid id=ssid autocomplete=off autocapitalize=off required value="%SSID%">
<label>Password</label><input name=pass id=pass type=password>
<button>Save &amp; connect</button></form>
<script>
const L=document.getElementById('nets'),S=document.getElementById('ssid'),P=document.getElementById('pass');
function esc(t){const d=document.createElement('div');d.textContent=t;return d.innerHTML}
function bars(r){const n=r>-55?4:r>-65?3:r>-75?2:1;let h='<span class=bars>';
 for(let i=1;i<=4;i++)h+='<i class="'+(i<=n?'f':'')+'" style="height:'+(i*25)+'%"></i>';return h+'</span>'}
function show(nets){if(!nets.length){L.innerHTML='<div id=st>No networks found</div>';return}
 L.innerHTML='';nets.forEach(n=>{const b=document.createElement('button');b.type='button';
  b.className='net'+(n.s===S.value?' on':'');
  b.innerHTML=bars(n.r)+'<span class=n>'+esc(n.s)+'</span><span class=m>'+(n.l?'&#128274;':'open')+'</span>';
  b.onclick=()=>{S.value=n.s;[...L.children].forEach(c=>c.classList.remove('on'));b.classList.add('on');
   P.value='';if(n.l)P.focus()};L.appendChild(b)})}
async function scan(fresh){L.innerHTML='<div id=st>Scanning...</div>';
 for(let i=0;i<40;i++){try{const r=await(await fetch('/scan'+(fresh&&!i?'?fresh=1':''))).json();
  if(!r.scanning){show(r.nets);return}}catch(e){}await new Promise(r=>setTimeout(r,500))}
 L.innerHTML='<div id=st>Scan failed - type the name below</div>'}
document.getElementById('rs').onclick=()=>scan(true);scan(false);
</script></div></body></html>)";

static void handle_root() {
  String h = FPSTR(PAGE_HEAD);
  String form = FPSTR(PAGE_FORM);
  form.replace("%SSID%", html_escape(g_settings.wifi_ssid));
  h += form;
  s_web->send(200, "text/html", h);
}

// Collect a finished async scan into s_nets.
static void scan_collect() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  s_scanning = false;
  s_nets.clear();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden: type it in
    int rssi = WiFi.RSSI(i);
    bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    bool dup = false;
    for (Net &o : s_nets) {
      if (o.ssid == ssid) {  // several access points, one name: keep the strongest
        dup = true;
        if (rssi > o.rssi) o.rssi = rssi;
      }
    }
    if (!dup) s_nets.push_back({ssid, rssi, secure});
  }
  std::sort(s_nets.begin(), s_nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  WiFi.scanDelete();
  Serial.printf("[wifi] scan: %u networks\n", (unsigned)s_nets.size());
}

static void scan_start() {
  if (s_scanning) return;
  // Async, so the page and the hotspot keep answering while it runs.
  s_scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
}

// GET /scan[?fresh=1]: {"scanning":true} while one runs, else the networks.
static void handle_scan() {
  if (s_web->hasArg("fresh")) scan_start();
  if (s_scanning) scan_collect();
  JsonDocument doc;
  if (s_scanning) {
    doc["scanning"] = true;
  } else {
    JsonArray a = doc["nets"].to<JsonArray>();
    for (const Net &n : s_nets) {
      JsonObject o = a.add<JsonObject>();
      o["s"] = n.ssid;
      o["r"] = n.rssi;
      o["l"] = n.secure;
    }
  }
  String out;
  serializeJson(doc, out);
  s_web->sendHeader("Cache-Control", "no-store");
  s_web->send(200, "application/json", out);
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
  // Scan first, before the hotspot is up, so the page has a list straight away.
  WiFi.scanNetworks();
  scan_collect();

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
  s_web->on("/scan", HTTP_GET, handle_scan);
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
  if (s_scanning) scan_collect();
  if (s_reboot_at && millis() > s_reboot_at) {
    ota_mark_good();  // a restart chosen from the UI isn't a failed update
    ESP.restart();
  }
}

bool wifi_setup_active() { return s_web != nullptr; }
String wifi_setup_ap_name() { return s_ap_name; }
