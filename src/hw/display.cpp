#include "display.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board.h"
#include "st77916_init.h"

// QSPI framing used by ST77916 / SH8601-class panels:
//   register write : opcode 0x02, 24-bit address = reg << 8, data on 1 line
//   pixel write    : opcode 0x32, 24-bit address = 0x2C << 8 (or 0x3C to
//                    continue), data on 4 lines
static spi_device_handle_t s_spi;
static lv_display_t *s_disp;
static void *s_lvbuf;

static constexpr int kBufLines = 30;  // LVGL partial buffer height (internal RAM is precious)
static constexpr size_t kBufPx = LCD_W * kBufLines;
static constexpr size_t kMaxChunk = kBufPx * 2;  // bytes per DMA transaction

static uint32_t s_err_cmd = 0, s_err_px = 0, s_err_q = 0;
static esp_err_t s_err_last = ESP_OK;
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
  spi_transaction_t t = {};
  t.cmd = 0x02;
  t.addr = (uint32_t)cmd << 8;
  if (len) {
    t.length = len * 8;
    t.tx_buffer = data;
  }
  esp_err_t e = spi_device_polling_transmit(s_spi, &t);
  if (e != ESP_OK) {
    s_err_cmd++;
    s_err_last = e;
  }
}

static void lcd_set_window(int x1, int y1, int x2, int y2) {
  uint8_t ca[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  uint8_t ra[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
  lcd_cmd(0x2A, ca, 4);
  lcd_cmd(0x2B, ra, 4);
}

static void lcd_write_pixels(const uint8_t *px, size_t bytes, bool first = true) {
  while (bytes) {
    size_t n = bytes > kMaxChunk ? kMaxChunk : bytes;
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_MODE_QIO;
    t.base.cmd = 0x32;
    t.base.addr = (first ? 0x2C : 0x3C) << 8;
    t.base.length = n * 8;
    t.base.tx_buffer = px;
    esp_err_t e = spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
    if (e != ESP_OK) {
      s_err_px++;
      s_err_last = e;
    }
    px += n;
    bytes -= n;
    first = false;
  }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px_map, w * h);  // panel wants big-endian RGB565
  lcd_set_window(area->x1, area->y1, area->x2, area->y2);
  lcd_write_pixels(px_map, w * h * 2);
  lv_display_flush_ready(disp);
}

// ---------------------------------------------------------------- layered
static bool s_layered = false;
static uint8_t *s_layer;            // ARGB8888 UI layer, LCD_W x LCD_H (PSRAM)
static const uint16_t *s_bg;        // background, RGB565 native byte order
static int s_bg_stride = LCD_W;
// Or rows on demand (see display.h): nothing has to hold a finished picture.
static display_row_fn s_bg_row;
alignas(4) static uint16_t s_bg_scratch[LCD_W];  // internal RAM: where a provider builds a row
static uint8_t *s_out;              // composed RGB565 big-endian, internal DMA
static uint8_t *s_out2;             // second buffer: compose one while the other is sent
static size_t s_chunk = kMaxChunk;  // bytes per composed DMA chunk (each of s_out, s_out2)
static spi_transaction_ext_t s_tx[2];
static bool s_tx_busy[2];

// Queue a pixel chunk by DMA and return at once; wait_tx(i) before reusing
// buffer i. The panel is the only thing on this bus, and lcd_cmd() is only
// ever called with nothing queued.
static void send_async(int i, const uint8_t *px, size_t bytes, bool first) {
  spi_transaction_ext_t &t = s_tx[i];
  t = {};
  t.base.flags = SPI_TRANS_MODE_QIO;
  t.base.cmd = 0x32;
  t.base.addr = (first ? 0x2C : 0x3C) << 8;
  t.base.length = bytes * 8;
  t.base.tx_buffer = px;
  esp_err_t e = spi_device_queue_trans(s_spi, (spi_transaction_t *)&t, portMAX_DELAY);
  if (e != ESP_OK) {
    s_err_q++;
    s_err_last = e;
    return;  // not queued: nothing to wait for
  }
  s_tx_busy[i] = true;
}

static void wait_tx_all() {
  spi_transaction_t *done;
  for (int k = 0; k < 2; k++) {
    if (!s_tx_busy[0] && !s_tx_busy[1]) return;
    spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
    s_tx_busy[done == (spi_transaction_t *)&s_tx[0] ? 0 : 1] = false;
  }
}

static void wait_tx(int i) {
  spi_transaction_t *done;
  while (s_tx_busy[i]) {
    spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
    s_tx_busy[done == (spi_transaction_t *)&s_tx[0] ? 0 : 1] = false;
  }
}
// Per row, the columns where the UI layer has anything (l > r: nothing).
// Outside them a pixel is just the background, which is a fast copy.
static int16_t s_row_l[LCD_H], s_row_r[LCD_H];
// Finer than the row span: which 8-pixel blocks of each row have any UI.
// A block with none is a straight background copy (no 4-byte layer reads from
// PSRAM); most of this screen is waterfall between a few widgets.
static constexpr int BLK = 8, NBLK = LCD_W / BLK;  // 45
static uint64_t s_blk[LCD_H];                       // bit b = block b has UI

static void rescan_rows(int y1, int y2) {
  for (int y = y1; y <= y2; y++) {
    const uint32_t *lay = (const uint32_t *)(s_layer + (size_t)y * LCD_W * 4);
    uint64_t m = 0;
    int l = LCD_W, r = -1;
    for (int b = 0; b < NBLK; b++) {
      const uint32_t *p = lay + b * BLK;
      uint32_t any = (p[0] | p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7]) >> 24;
      if (any) {
        m |= 1ull << b;
        if (l == LCD_W) l = b * BLK;
        r = b * BLK + BLK - 1;
      }
    }
    s_blk[y] = m;
    s_row_l[y] = l;
    s_row_r[y] = r;
  }
}

// Background only, two pixels at a time, byte-swapped for the panel.
static inline void copy_bg(uint8_t *o, const uint16_t *bg, int n) {
  if (!bg) {
    memset(o, 0, n * 2);
    return;
  }
  const uint32_t *src = (const uint32_t *)bg;
  uint32_t *dst = (uint32_t *)o;
  for (int i = 0; i < n / 2; i++) {
    uint32_t v = src[i];
    dst[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  }
}

// Channel by channel: the packed "& 0xF81F" trick only works with a 5-bit
// alpha. With 8 bits, red's product runs down into blue's bits and blue
// comes out nearly random (the speckle on every anti-aliased edge).
static inline uint16_t blend(uint16_t fg, uint16_t bg, uint32_t a) {  // a: 0..255
  uint32_t na = 255 - a;
  uint32_t r = ((fg >> 11) * a + (bg >> 11) * na + 127) / 255;
  uint32_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * na + 127) / 255;
  uint32_t b = ((fg & 31) * a + (bg & 31) * na + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Background + UI layer for a rectangle, streamed to the panel.
static uint32_t s_spi_us = 0, s_px = 0;  // how much of a present is the SPI transfer

static void compose_send(int x1, int y1, int x2, int y2) {
  x1 &= ~1;
  y1 &= ~1;
  x2 |= 1;
  y2 |= 1;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 >= LCD_W) x2 = LCD_W - 1;
  if (y2 >= LCD_H) y2 = LCD_H - 1;
  int w = x2 - x1 + 1;
  int rows_per = (int)(s_chunk / (w * 2));
  if (rows_per < 1) rows_per = 1;
  wait_tx_all();  // nothing may be in flight when the window changes
  lcd_set_window(x1, y1, x2, y2);
  bool first = true;
  int buf = 0;
  for (int y0 = y1; y0 <= y2; y0 += rows_per) {
    int rows = y2 - y0 + 1 < rows_per ? y2 - y0 + 1 : rows_per;
    // Compose into the buffer that isn't being sent; the other one goes out
    // by DMA meanwhile.
    wait_tx(buf);
    uint8_t *out = buf ? s_out2 : s_out;
    uint8_t *o = out;
    for (int y = y0; y < y0 + rows; y++) {
      const uint32_t *lay = (const uint32_t *)(s_layer + ((size_t)y * LCD_W + x1) * 4);
      const uint16_t *bg = s_bg_row ? s_bg_row(y, s_bg_scratch) + x1
                                    : (s_bg ? s_bg + (size_t)y * s_bg_stride + x1 : nullptr);
      // Nothing over the background anywhere in this window's row: one copy.
      int l = s_row_l[y] - x1, r = s_row_r[y] - x1;
      if (r < 0 || l >= w) {
        copy_bg(o, bg, w);
        o += w * 2;
        continue;
      }
      // Otherwise block by block: background copies where the UI layer is
      // empty, a blend only where it isn't.
      uint64_t m = s_blk[y];
      int x = 0;
      while (x < w) {
        int ax = x1 + x;                   // absolute column
        int bidx = ax / BLK;
        int bend = (bidx + 1) * BLK - x1;  // end of this block, window-relative
        if (bend > w) bend = w;
        int n = bend - x;
        if (!((m >> bidx) & 1)) {
          copy_bg(o, bg ? bg + x : nullptr, n);
          o += n * 2;
        } else {
          for (int i = x; i < bend; i++) {
            uint32_t p = lay[i];  // LVGL ARGB8888 in memory: B, G, R, A
            uint32_t a = p >> 24;
            uint16_t c;
            if (a == 0) {
              c = bg ? bg[i] : 0;
            } else {
              uint16_t fg = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
              c = a >= 250 ? fg : blend(fg, bg ? bg[i] : 0, a);
            }
            *o++ = c >> 8;  // the panel wants big-endian
            *o++ = c & 0xFF;
          }
        }
        x = bend;
      }
    }
    uint32_t ts = micros();
    send_async(buf, out, (size_t)rows * w * 2, first);
    s_spi_us += micros() - ts;  // now only the time to queue (and any wait above)
    s_px += rows * w;
    first = false;
    buf ^= 1;
  }
  wait_tx_all();  // done before returning: LVGL may reuse its buffers, and lcd_cmd needs the bus
}

static void layered_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  // DIRECT mode: LVGL has drawn the changed part of the UI into s_layer;
  // note where the UI now is in those rows, then put them on the panel.
  rescan_rows(area->y1 < 0 ? 0 : area->y1, area->y2 >= LCD_H ? LCD_H - 1 : area->y2);
  compose_send(area->x1, area->y1, area->x2, area->y2);
  lv_display_flush_ready(disp);
}

bool display_layered_begin(int chunk_lines) {
  if (s_layered) return true;
  size_t bytes = (size_t)LCD_W * LCD_H * 4;
  s_chunk = (size_t)LCD_W * 2 * (chunk_lines < 1 ? 1 : (chunk_lines > kBufLines ? kBufLines : chunk_lines));
  s_layer = (uint8_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
  s_out = (uint8_t *)heap_caps_malloc(s_chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  s_out2 = (uint8_t *)heap_caps_malloc(s_chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!s_layer || !s_out || !s_out2) {
    free(s_layer);
    free(s_out);
    free(s_out2);
    s_layer = s_out = s_out2 = nullptr;
    return false;
  }
  memset(s_layer, 0, bytes);
  for (int y = 0; y < LCD_H; y++) {
    s_row_l[y] = LCD_W;
    s_row_r[y] = -1;
    s_blk[y] = 0;
  }
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_ARGB8888);
  lv_display_set_buffers(s_disp, s_layer, nullptr, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(s_disp, layered_flush_cb);
  free(s_lvbuf);  // the old partial buffer isn't used any more
  s_lvbuf = nullptr;
  s_layered = true;
  Serial.printf("[display] layered mode: UI layer over background, %u-byte DMA chunks, internal heap %u\n",
                (unsigned)s_chunk, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

bool display_layered() { return s_layered; }

void display_set_background_rows(display_row_fn fn) { s_bg_row = fn; }

void display_set_background(const uint16_t *rgb565, int stride_px) {
  s_bg_row = nullptr;
  s_bg = rgb565;
  s_bg_stride = stride_px > 0 ? stride_px : LCD_W;
}

// One present per frame, of everything that changed, at most ~30 fps: the
// background can change far more often than that (meter smoothing, morph,
// marker) and pushing each change separately is what swamped the CPU.
static int s_dx1 = LCD_W, s_dy1 = LCD_H, s_dx2 = -1, s_dy2 = -1;
static uint32_t s_last_present = 0, s_present_us = 0, s_present_n = 0, s_present_log = 0;

void display_mark_dirty(int x1, int y1, int x2, int y2) {
  if (!s_layered || x2 < x1 || y2 < y1) return;
  if (x1 < s_dx1) s_dx1 = x1;
  if (y1 < s_dy1) s_dy1 = y1;
  if (x2 > s_dx2) s_dx2 = x2;
  if (y2 > s_dy2) s_dy2 = y2;
}

void display_flush_dirty() {
  if (!s_layered) return;
  uint32_t now = millis();
  if (s_dx2 >= 0 && now - s_last_present >= 33) {
    uint32_t t0 = micros();
    compose_send(s_dx1, s_dy1, s_dx2, s_dy2);
    s_present_us += micros() - t0;
    s_present_n++;
    s_last_present = now;
    s_dx1 = LCD_W;
    s_dy1 = LCD_H;
    s_dx2 = s_dy2 = -1;
  }
  if (now - s_present_log > 5000) {
    if (s_err_cmd || s_err_px || s_err_q)
      Serial.printf("[spi] ERRORS cmd %lu px %lu queue %lu, last %s\n", (unsigned long)s_err_cmd,
                    (unsigned long)s_err_px, (unsigned long)s_err_q, esp_err_to_name(s_err_last));
    Serial.printf("[present] %lu frames/5s, %lu us each (SPI %lu us), %lu kpx each, %lu%% of one core\n",
                  (unsigned long)s_present_n, (unsigned long)(s_present_n ? s_present_us / s_present_n : 0),
                  (unsigned long)(s_present_n ? s_spi_us / s_present_n : 0),
                  (unsigned long)(s_present_n ? s_px / s_present_n / 1000 : 0), (unsigned long)(s_present_us / 50000));
    s_present_us = s_present_n = s_spi_us = s_px = 0;
    s_present_log = now;
  }
}

void display_present_rect(int x1, int y1, int x2, int y2) {
  if (!s_layered || x2 < x1 || y2 < y1) return;
  compose_send(x1, y1, x2, y2);
}

void display_present(int y1, int y2) {
  if (!s_layered) return;
  compose_send(0, y1, LCD_W - 1, y2);
}

// The ST77916 needs window start/end on even coordinates.
static uint32_t s_inv_px = 0, s_inv_n = 0, s_inv_at = 0;
static lv_area_t s_inv_big;
static void rounder_cb(lv_event_t *e) {
  lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
  // Debug: how much gets invalidated, and the biggest piece, per 5 s.
  uint32_t px = (uint32_t)lv_area_get_width(a) * lv_area_get_height(a);
  if (px > (uint32_t)lv_area_get_width(&s_inv_big) * lv_area_get_height(&s_inv_big)) s_inv_big = *a;
  s_inv_px += px;
  s_inv_n++;
  if (millis() - s_inv_at > 5000) {
    Serial.printf("[inv] %lu areas, %lu kpx/5s, biggest %dx%d at %d,%d\n", (unsigned long)s_inv_n,
                  (unsigned long)(s_inv_px / 1000), (int)lv_area_get_width(&s_inv_big),
                  (int)lv_area_get_height(&s_inv_big), (int)s_inv_big.x1, (int)s_inv_big.y1);
    s_inv_px = s_inv_n = 0;
    s_inv_big = {0, 0, -1, -1};
    s_inv_at = millis();
  }
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}

bool display_use_buffer_lines(int lines) {
  size_t bytes = (size_t)LCD_W * lines * 2;
  void *b = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!b) return false;
  // Flushes are synchronous and lcd_write_pixels() splits into DMA-sized
  // pieces, so a bigger buffer needs nothing else.
  lv_display_set_buffers(s_disp, b, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  free(s_lvbuf);
  s_lvbuf = b;
  Serial.printf("[display] draw buffer now %d lines\n", lines);
  return true;
}

void display_set_brightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  ledcWrite(0, (uint32_t)pct * 255 / 100);
}

void display_init() {
  ledcSetup(0, 20000, 8);
  ledcAttachPin(PIN_LCD_BL, 0);
  ledcWrite(0, 0);

  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, HIGH);
  delay(10);
  digitalWrite(PIN_LCD_RST, LOW);
  delay(10);
  digitalWrite(PIN_LCD_RST, HIGH);
  delay(120);

  spi_bus_config_t bus = {};
  bus.data0_io_num = PIN_LCD_D0;
  bus.data1_io_num = PIN_LCD_D1;
  bus.sclk_io_num = PIN_LCD_SCK;
  bus.data2_io_num = PIN_LCD_D2;
  bus.data3_io_num = PIN_LCD_D3;
  bus.max_transfer_sz = kMaxChunk;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev = {};
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 0;
  // TEMPORARY 40 MHz (was 80) while chasing receiver-mode grain/freezes. It
  // did NOT fix them (2026-11-03), so this goes back to 80 once they're found.
  dev.clock_speed_hz = 40 * 1000 * 1000;
  dev.spics_io_num = PIN_LCD_CS;
  dev.queue_size = 4;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

  for (const uint8_t *p = ST77916_INIT; !(p[0] == 0xFF && p[1] == 0xFF);) {
    uint8_t cmd = p[0], len = p[1];
    lcd_cmd(cmd, p + 2, len);
    p += 2 + len;
  }
  uint8_t colmod = 0x55;  // RGB565
  lcd_cmd(0x3A, &colmod, 1);
  lcd_cmd(0x21, nullptr, 0);  // inversion on (panel is wired inverted)
  lcd_cmd(0x11, nullptr, 0);  // sleep out
  delay(120);
  lcd_cmd(0x29, nullptr, 0);  // display on
  delay(20);

  // Clear to black before the backlight comes up.
  uint8_t *line = (uint8_t *)heap_caps_calloc(kBufPx, 2, MALLOC_CAP_DMA);
  lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
  for (int y = 0; y < LCD_H; y += kBufLines) lcd_write_pixels(line, kMaxChunk);
  free(line);

  lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
  s_disp = disp;
  // One buffer: flushes are synchronous, so a second buffer would only cost
  // ~22 KB of internal RAM that the two TLS sockets need more.
  void *b1 = heap_caps_malloc(kBufPx * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  s_lvbuf = b1;
  lv_display_set_buffers(disp, b1, nullptr, kBufPx * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
}

// Debug: one row, pixel by pixel - background (as the provider gives it), the
// UI layer over it, and what's sent.
void display_debug_row(int y, int x0, int x1) {
  if (!s_layered || y < 0 || y >= LCD_H) return;
  const uint16_t *bg = s_bg_row ? s_bg_row(y, s_bg_scratch) : (s_bg ? s_bg + (size_t)y * s_bg_stride : nullptr);
  const uint32_t *lay = (const uint32_t *)(s_layer + (size_t)y * LCD_W * 4);
  Serial.printf("[row %d] blk=%016llx l=%d r=%d\n", y, (unsigned long long)s_blk[y], s_row_l[y], s_row_r[y]);
  for (int x = x0; x <= x1 && x < LCD_W; x++) {
    uint32_t p = lay[x];
    uint16_t b = bg ? bg[x] : 0;
    uint32_t a = p >> 24;
    uint16_t fg = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
    uint16_t out = a == 0 ? b : (a >= 250 ? fg : blend(fg, b, a));
    Serial.printf("  x=%3d bg=%04x layer=%08lx out=%04x\n", x, b, (unsigned long)p, out);
  }
}

// Debug: horizontal bands of solid RGB565 colours straight to the panel
// (nothing composed). Shows whether a colour itself looks grainy.
void display_debug_fill(const uint16_t *cols, int n) {
  wait_tx_all();
  static constexpr int kRows = 4;
  uint8_t *dma = (uint8_t *)heap_caps_malloc(LCD_W * 2 * kRows, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!dma) {
    Serial.println("[dbg] no DMA RAM for the test fill");
    return;
  }
  lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
  int band = LCD_H / n;
  for (int y = 0; y < LCD_H; y += kRows) {
    uint16_t c = cols[(y / band) < n ? y / band : n - 1];
    for (int i = 0; i < LCD_W * kRows; i++) {
      dma[i * 2] = c >> 8;  // big-endian
      dma[i * 2 + 1] = c & 0xFF;
    }
    lcd_write_pixels(dma, LCD_W * 2 * kRows, y == 0);
  }
  free(dma);
}

void display_debug_fill_red() {
  const uint16_t red = 0xF800;
  display_debug_fill(&red, 1);
  Serial.println("[dbg] panel filled red (stays until the next redraw)");
}

void display_debug_reinit() {
  wait_tx_all();
  lcd_cmd(0x11, nullptr, 0);  // sleep out
  delay(10);
  uint8_t colmod = 0x55;
  lcd_cmd(0x3A, &colmod, 1);
  lcd_cmd(0x21, nullptr, 0);
  lcd_cmd(0x29, nullptr, 0);  // display on
  display_mark_dirty(0, 0, LCD_W - 1, LCD_H - 1);
  if (s_disp) lv_obj_invalidate(lv_screen_active());
  Serial.println("[dbg] panel re-initialised, full redraw queued");
}
