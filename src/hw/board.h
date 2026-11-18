// Board pin maps. One firmware runs on both boards: board_detect() works out
// which one it is at boot (see board.cpp) and points g_board at its map.
//   JC3636K718    LED ring, DRV2605 ERM, PCM5100A mute on the S3
//   JC3636K518C   Waveshare ESP32-S3-Knob-Touch-LCD-1.8 layout (V2 board): no
//                 LED ring, DRV2605 LRA, DAC mute owned by the second chip
// The PIN_* names read the detected board's map at run time, so they can't be
// used in #if or in static initialisers.
#pragma once

#include <stdint.h>

#define LCD_W 360
#define LCD_H 360

struct Board {
  const char *name;
  // ST77916 QSPI panel
  int8_t lcd_sck, lcd_cs, lcd_d0, lcd_d1, lcd_d2, lcd_d3, lcd_rst, lcd_bl;
  // CST816 touch (I2C 0x15); the DRV2605 haptic (0x5A) shares this bus
  int8_t tp_sda, tp_scl, tp_int, tp_rst;
  // Knob: not quadrature. Each detent pulses one pin LOW.
  int8_t knob_cw, knob_ccw;
  // PCM5100A I2S DAC; dac_mute -1 = not on the S3
  int8_t i2s_bck, i2s_ws, i2s_do, dac_mute;
  // WS2812 ring, GRB; led_count 0 = no ring
  int8_t led_ring, led_count;
  // Battery ADC and its divider
  int8_t bat_adc;
  float bat_divider;
  bool haptic_lra;  // DRV2605 drives an LRA (else an ERM)
};

extern const Board *g_board;

// Probe the touch/haptic I2C bus of each board and pick the one that answers.
// Call first thing in setup(), before anything touches a pin.
void board_detect();

#define BOARD_NAME (g_board->name)
#define PIN_LCD_SCK (g_board->lcd_sck)
#define PIN_LCD_CS (g_board->lcd_cs)
#define PIN_LCD_D0 (g_board->lcd_d0)
#define PIN_LCD_D1 (g_board->lcd_d1)
#define PIN_LCD_D2 (g_board->lcd_d2)
#define PIN_LCD_D3 (g_board->lcd_d3)
#define PIN_LCD_RST (g_board->lcd_rst)
#define PIN_LCD_BL (g_board->lcd_bl)
#define PIN_TP_SDA (g_board->tp_sda)
#define PIN_TP_SCL (g_board->tp_scl)
#define PIN_TP_INT (g_board->tp_int)
#define PIN_TP_RST (g_board->tp_rst)
#define PIN_KNOB_CW (g_board->knob_cw)
#define PIN_KNOB_CCW (g_board->knob_ccw)
#define PIN_I2S_BCK (g_board->i2s_bck)
#define PIN_I2S_WS (g_board->i2s_ws)
#define PIN_I2S_DO (g_board->i2s_do)
#define PIN_DAC_MUTE (g_board->dac_mute)
#define PIN_LED_RING (g_board->led_ring)
#define LED_RING_COUNT (g_board->led_count)
#define PIN_BAT_ADC (g_board->bat_adc)
#define BAT_DIVIDER (g_board->bat_divider)
#define HAPTIC_LRA (g_board->haptic_lra)
