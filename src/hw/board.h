// Guition JC3636K718 pin map (from the manufacturer demo pincfg.h).
#pragma once

#define LCD_W 360
#define LCD_H 360

// ST77916 QSPI panel
#define PIN_LCD_SCK 11
#define PIN_LCD_CS 12
#define PIN_LCD_D0 13
#define PIN_LCD_D1 14
#define PIN_LCD_D2 15
#define PIN_LCD_D3 16
#define PIN_LCD_RST 17
#define PIN_LCD_BL 21

// CST816 touch (I2C)
#define PIN_TP_SDA 9
#define PIN_TP_SCL 10
#define PIN_TP_INT 7
#define PIN_TP_RST 8

// Knob: not quadrature. Each detent pulses one pin LOW:
// GPIO1 = clockwise (right), GPIO2 = anticlockwise (left).
#define PIN_KNOB_CW 1
#define PIN_KNOB_CCW 2

// PCM5100A I2S DAC (for the audio phase)
#define PIN_I2S_BCK 3
#define PIN_I2S_WS 45
#define PIN_I2S_DO 42
#define PIN_DAC_MUTE 46  // LOW = mute

// WS2812 ring, 13 LEDs, GRB (GPIO0 is also the BOOT strap)
#define PIN_LED_RING 0
#define LED_RING_COUNT 13
