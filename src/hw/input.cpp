#include "input.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "board.h"
#include "haptic.h"
#include "../app/saver.h"

// ---------------------------------------------------------------- knob
// Not a quadrature encoder: each detent drives ONE pin low for ~100 ms
// (GPIO1 = clockwise, GPIO2 = anticlockwise). The end of a GPIO1 pulse also
// couples a burst of microsecond glitches onto GPIO2, so edge interrupts
// miscount. Instead sample both pins every 1 ms and count a detent when a pin
// has been low for two consecutive samples after being high.
static volatile int32_t s_knob = 0;
static volatile uint32_t s_knob_last = 0;
static volatile uint32_t s_knob_gap = 1000;  // ms between the last two clicks
static portMUX_TYPE s_knob_mux = portMUX_INITIALIZER_UNLOCKED;

static void knob_poll(void *) {
  static uint8_t hist[2] = {0xFF, 0xFF};
  static bool down[2] = {false, false};
  const int pins[2] = {PIN_KNOB_CW, PIN_KNOB_CCW};
  for (int i = 0; i < 2; i++) {
    hist[i] = (hist[i] << 1) | (gpio_get_level((gpio_num_t)pins[i]) & 1);
    uint8_t h = hist[i] & 0x07;
    if (!down[i] && h == 0x04) {  // high, low, low
      down[i] = true;
      portENTER_CRITICAL(&s_knob_mux);
      s_knob += i == 0 ? 1 : -1;
      portEXIT_CRITICAL(&s_knob_mux);
      uint32_t now = millis();
      s_knob_gap = now - s_knob_last;
      s_knob_last = now;
    } else if (down[i] && (hist[i] & 0x03) == 0x03) {
      down[i] = false;
    }
  }
}

int knob_take() {
  portENTER_CRITICAL(&s_knob_mux);
  int v = s_knob;
  s_knob = 0;
  portEXIT_CRITICAL(&s_knob_mux);
  return v;
}

uint32_t knob_idle_ms() { return millis() - s_knob_last; }
uint32_t knob_gap_ms() { return s_knob_gap; }

int knob_accel() {
  uint32_t gap = s_knob_gap;
  return gap < 35 ? 4 : (gap < 70 ? 2 : 1);
}

// ---------------------------------------------------------------- touch
static constexpr uint8_t kTouchAddr = 0x15;

static bool cst816_read(int16_t *x, int16_t *y) {
  Wire.beginTransmission(kTouchAddr);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kTouchAddr, (uint8_t)5) != 5) return false;
  uint8_t d[5];
  for (int i = 0; i < 5; i++) d[i] = Wire.read();
  if ((d[0] & 0x0F) == 0) return false;
  *x = ((d[1] & 0x0F) << 8) | d[2];
  *y = ((d[3] & 0x0F) << 8) | d[4];
  return true;
}

static volatile uint32_t s_touch_last = 0;
uint32_t touch_idle_ms() { return millis() - s_touch_last; }

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  static int16_t lx = 0, ly = 0;
  static bool was_down = false;
  int16_t x, y;
  bool down = cst816_read(&x, &y);
  // A touch that wakes a dark screen only wakes it: the whole press (until the
  // finger lifts) is hidden from the UI.
  static bool swallow = false;
  if (down && !was_down && saver_screen_off()) swallow = true;
  if (down) s_touch_last = millis();
  if (down && !was_down && !swallow) haptic_click();  // touch-down feedback
  was_down = down;
  if (swallow) {
    if (!down) swallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (down) {
    lx = constrain(x, 0, LCD_W - 1);
    ly = constrain(y, 0, LCD_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = lx;
  data->point.y = ly;
}

void touch_sleep() {
  Wire.beginTransmission(kTouchAddr);
  Wire.write(0xFE);
  Wire.write(0x00);  // auto sleep back on
  Wire.endTransmission();
}

void input_init() {
  pinMode(PIN_KNOB_CW, INPUT_PULLUP);
  pinMode(PIN_KNOB_CCW, INPUT_PULLUP);
esp_timer_create_args_t ta = {};
  ta.callback = knob_poll;
  ta.name = "knob";
  esp_timer_handle_t th;
  esp_timer_create(&ta, &th);
  esp_timer_start_periodic(th, 1000);

  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(50);
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  // Keep the controller awake so polling never hits a sleeping chip.
  Wire.beginTransmission(kTouchAddr);
  Wire.write(0xFE);
  Wire.write(0x01);
  Wire.endTransmission();

  lv_indev_t *ind = lv_indev_create();
  lv_indev_set_type(ind, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(ind, touch_read_cb);
}
