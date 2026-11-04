#include "instances.h"

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#include <algorithm>

static const char *kHost = "instances.ubersdr.org";
// minimal=true asks the collector for a few-KB list instead of ~700 KB; older
// collectors ignore it and send the full list, which we still cope with.
static const char *kPath = "/api/instances?online_only=true&minimal=true";

static volatile FetchState s_state = FetchState::Idle;
static String s_error;
static std::vector<Instance> s_list;

// Small, deterministic HTTP/1.1 GET into a PSRAM buffer. HTTPClient was
// unreliable on the large chunked TLS response (stalls, truncation), so this
// does its own header + chunked parsing with a hard overall deadline.
// The collector rejects HTTP/1.0 with 426, hence 1.1 + Connection: close.
static bool read_line(WiFiClient &c, String &line, uint32_t deadline) {
  line = "";
  while ((int32_t)(millis() - deadline) < 0) {
    int ch = c.read();
    if (ch < 0) {
      if (!c.connected()) return false;
      delay(2);
      continue;
    }
    if (ch == '\n') return true;
    if (ch != '\r') line += (char)ch;
  }
  return false;
}

static bool read_exact(WiFiClient &c, char *dst, size_t n, uint32_t deadline) {
  while (n && (int32_t)(millis() - deadline) < 0) {
    int r = c.read((uint8_t *)dst, n);
    if (r > 0) {
      dst += r;
      n -= r;
    } else {
      if (!c.connected() && !c.available()) return false;
      delay(2);
    }
  }
  return n == 0;
}

static bool http_get(const char *host, const char *path, char **out, size_t *out_len, String *err) {
  const uint32_t deadline = millis() + 30000;
  WiFiClientSecure c;
  c.setInsecure();  // POC: no CA bundle yet
  c.setTimeout(10);
  if (!c.connect(host, 443)) {
    *err = "connect failed";
    return false;
  }
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: UberSDR-Dial/0.1\r\nAccept: application/json\r\n"
           "Connection: close\r\n\r\n",
           path, host);
  String line;
  if (!read_line(c, line, deadline)) {
    *err = "no response";
    return false;
  }
  int sp = line.indexOf(' ');
  int code = sp > 0 ? line.substring(sp + 1).toInt() : 0;
  bool chunked = false;
  long content_len = -1;
  while (read_line(c, line, deadline) && line.length()) {
    String l = line;
    l.toLowerCase();
    if (l.startsWith("transfer-encoding:") && l.indexOf("chunked") > 0) chunked = true;
    if (l.startsWith("content-length:")) content_len = l.substring(15).toInt();
  }
  if (code != 200) {
    *err = "HTTP " + String(code);
    return false;
  }
  size_t cap = 64 * 1024, len = 0;
  char *buf = (char *)ps_malloc(cap);
  auto grow = [&](size_t need) {
    if (len + need + 1 <= cap) return true;
    while (len + need + 1 > cap) cap *= 2;
    char *nb = (char *)ps_realloc(buf, cap);
    if (!nb) return false;
    buf = nb;
    return true;
  };
  bool ok = buf != nullptr;
  if (ok && chunked) {
    while (ok) {
      if (!read_line(c, line, deadline)) { ok = false; break; }
      long n = strtol(line.c_str(), nullptr, 16);
      if (n <= 0) break;  // last chunk
      if (!grow(n) || !read_exact(c, buf + len, n, deadline)) { ok = false; break; }
      len += n;
      read_line(c, line, deadline);  // CRLF after chunk data
    }
  } else if (ok) {
    while (true) {
      if (!grow(4096)) { ok = false; break; }
      int r = c.read((uint8_t *)buf + len, 4096);
      if (r > 0) { len += r; continue; }
      if (!c.connected() && !c.available()) break;
      if ((int32_t)(millis() - deadline) >= 0) { ok = false; break; }
      if (content_len >= 0 && (long)len >= content_len) break;
      delay(2);
    }
  }
  c.stop();
  if (!ok) {
    free(buf);
    *err = (int32_t)(millis() - deadline) >= 0 ? "timed out" : "read failed";
    return false;
  }
  buf[len] = 0;
  *out = buf;
  *out_len = len;
  return true;
}

static void fetch_task(void *) {
  std::vector<Instance> out;
  String err;
  {
    char *body = nullptr;
    size_t len = 0;
    uint32_t t0 = millis();
    if (!http_get(kHost, kPath, &body, &len, &err)) {
      // err already set
    } else {
      Serial.printf("[instances] downloaded %u bytes in %lu ms\n", (unsigned)len, millis() - t0);
      {
        JsonDocument filter;
        JsonObject f = filter["instances"][0].to<JsonObject>();
        for (const char *k : {"id", "callsign", "name", "location", "public_url", "available_clients",
                              "max_clients", "snr_0_30_mhz", "latitude", "longitude"})
          f[k] = true;
        JsonDocument doc;
        DeserializationError e = deserializeJson(doc, body, len, DeserializationOption::Filter(filter));
        if (e) {
          err = String("JSON: ") + e.c_str();
        } else {
          for (JsonObject o : doc["instances"].as<JsonArray>()) {
            Instance i;
            i.id = o["id"] | "";
            i.callsign = o["callsign"] | "";
            i.name = o["name"] | "";
            i.location = o["location"] | "";
            i.url = o["public_url"] | "";
            i.available = o["available_clients"] | 0;
            i.max_clients = o["max_clients"] | 0;
            i.snr = o["snr_0_30_mhz"] | 0;
            i.lat = o["latitude"] | 0.0f;
            i.lon = o["longitude"] | 0.0f;
            if (i.url.isEmpty()) continue;
            if (!i.url.endsWith("/")) i.url += "/";
            out.push_back(i);
          }
        }
      }
      free(body);
    }
  }
  // Receivers with free slots first, then best SNR.
  std::sort(out.begin(), out.end(), [](const Instance &a, const Instance &b) {
    bool fa = a.available > 0, fb = b.available > 0;
    if (fa != fb) return fa;
    return a.snr > b.snr;
  });
  if (err.isEmpty() && out.empty()) err = "no receivers online";
  s_list = std::move(out);
  s_error = err;
  s_state = err.isEmpty() ? FetchState::Done : FetchState::Error;
  Serial.printf("[instances] %u receivers%s%s\n", (unsigned)s_list.size(), err.isEmpty() ? "" : ", error: ",
                err.c_str());
  vTaskDelete(nullptr);
}

void instances_fetch_async() {
  if (s_state == FetchState::Loading) return;
  s_state = FetchState::Loading;
  xTaskCreatePinnedToCore(fetch_task, "inst", 16384, nullptr, 1, nullptr, 0);
}

FetchState instances_state() { return s_state; }
const String &instances_error() { return s_error; }
const std::vector<Instance> &instances_list() { return s_list; }
