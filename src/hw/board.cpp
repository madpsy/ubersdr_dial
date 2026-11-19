#include "board.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

// Guition JC3636K718 (from the manufacturer demo pincfg.h).
static const Board kK718 = {
    .name = "JC3636K718",
    .lcd_sck = 11, .lcd_cs = 12, .lcd_d0 = 13, .lcd_d1 = 14, .lcd_d2 = 15, .lcd_d3 = 16, .lcd_rst = 17, .lcd_bl = 21,
    .tp_sda = 9, .tp_scl = 10, .tp_int = 7, .tp_rst = 8,
    // GPIO1 = clockwise (right), GPIO2 = anticlockwise (left).
    .knob_cw = 1, .knob_ccw = 2,
    .i2s_bck = 3, .i2s_ws = 45, .i2s_do = 42, .dac_mute = 46,  // LOW = mute
    // 13 LEDs on GPIO0 (also the BOOT strap)
    .led_ring = 0, .led_count = 13,
    .bat_adc = 6, .bat_divider = 2.0f,
    .haptic_lra = false,
    .rotate180 = false,
};

// Guition JC3636K518C, V2 board (Waveshare ESP32-S3-Knob-Touch-LCD-1.8 layout).
// The two revisions share the display, touch, knob, battery and haptic I2C
// pins, but on V1:
//   - the PCM5100A is fed only by the second chip (ESP32-U4WDH IO25/26/27);
//     S3 GPIO39/40 are the UART to that chip, so S3 I2S audio is silent;
//   - the DRV2605 EN pin is S3 GPIO38 (tied to 3V3 on V2).
// The DAC is reached through a CH445P switch that picks the S3 or the second
// chip as its source. S3 GPIO0 (I2S_SWITCH_IN, also the BOOT strap) selects
// it; its 10K pull-up (R58) selects the S3, so the firmware leaves GPIO0
// alone. The DAC's XSMT (mute) is the second chip's IO32, so there's no mute
// pin here. S3 GPIO38/48 are a UART to that chip.
static const Board kK518 = {
    .name = "JC3636K518",
    .lcd_sck = 13, .lcd_cs = 14, .lcd_d0 = 15, .lcd_d1 = 16, .lcd_d2 = 17, .lcd_d3 = 18, .lcd_rst = 21, .lcd_bl = 47,
    .tp_sda = 11, .tp_scl = 12, .tp_int = 9, .tp_rst = 10,
    // GPIO8 = clockwise, GPIO7 = anticlockwise. No push switch.
    .knob_cw = 8, .knob_ccw = 7,
    .i2s_bck = 39, .i2s_ws = 40, .i2s_do = 41, .dac_mute = -1,
    .led_ring = -1, .led_count = 0,  // no LED ring
    .bat_adc = 1, .bat_divider = 2.0f,  // ADC1_CH0
    .haptic_lra = true,
    .rotate180 = true,  // held with the USB-C socket at the top
};

const Board *g_board = &kK718;

// Does anything answer at the touch (0x15) or haptic (0x5A) address with the
// I2C bus on these pins? On the other board these pins are harmless to wiggle
// before anything is set up: K518 pins on a K718 are the LCD's clock and chip
// select; K718 pins on a K518 are the touch chip's INT and RST.
static bool probe(int sda, int scl) {
  Wire.begin(sda, scl, 100000);
  bool found = false;
  for (uint8_t a : {(uint8_t)0x5A, (uint8_t)0x15}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found = true;
  }
  Wire.end();
  pinMode(sda, INPUT);
  pinMode(scl, INPUT);
  return found;
}

void board_detect() {
  // K518 pins first: on a K518 that leaves its touch INT/RST (the K718's
  // SDA/SCL) alone; the K718 has nothing on its own 11/12 to upset.
  const Board *found = nullptr;
  const char *how = "probed";
  if (probe(kK518.tp_sda, kK518.tp_scl)) found = &kK518;
  else if (probe(kK718.tp_sda, kK718.tp_scl)) found = &kK718;

  // Remember the answer, so a board whose touch and haptic chips both fail to
  // answer one boot still gets the right pins.
  Preferences p;
  p.begin("board", false);
  String saved = p.getString("id", "");
  if (found) {
    if (saved != found->name) p.putString("id", found->name);
  } else {
    found = saved == kK518.name ? &kK518 : &kK718;
    how = saved.isEmpty() ? "nothing answered, default" : "nothing answered, last seen";
  }
  p.end();
  g_board = found;
  Serial.printf("[board] %s (%s)\n", g_board->name, how);
}
