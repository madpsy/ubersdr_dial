#include "haptic.h"

#include <Arduino.h>
#include <Wire.h>

// TI DRV2605(L): fixed I2C address 0x5A. Plays effects from its built-in ROM
// libraries: write the effect id to the waveform sequencer, then set GO.
static constexpr uint8_t kAddr = 0x5A;
static constexpr uint8_t REG_STATUS = 0x00, REG_MODE = 0x01, REG_LIB = 0x03, REG_SEQ0 = 0x04,
                         REG_SEQ1 = 0x05, REG_GO = 0x0C;
// ERM library 1 effects
static constexpr uint8_t FX_SHARP_TICK_3 = 26;  // "Sharp Tick 3 - 60%": light detent
static constexpr uint8_t FX_SHARP_CLICK_60 = 6;  // "Sharp Click - 60%": touch

static bool s_present = false, s_enabled = true;
static uint32_t s_last = 0;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int rd(uint8_t reg) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(kAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

void haptic_init() {
  int st = rd(REG_STATUS);
  if (st < 0) {
    Serial.println("[haptic] DRV2605 not found at 0x5A");
    return;
  }
  // Status bits 7:5 = device id (3 = DRV2605, 7 = DRV2605L).
  Serial.printf("[haptic] DRV2605 found, id %d\n", (st >> 5) & 7);
  s_present = wr(REG_MODE, 0x00)     // out of standby, internal trigger
              && wr(REG_LIB, 0x01)   // ERM library A
              && wr(REG_SEQ1, 0x00); // single-effect sequences
}

bool haptic_present() { return s_present; }
void haptic_enable(bool on) { s_enabled = on; }

static void play(uint8_t fx, uint32_t min_gap_ms) {
  if (!s_present || !s_enabled) return;
  uint32_t now = millis();
  if (now - s_last < min_gap_ms) return;  // fast spins: don't smear into a buzz
  s_last = now;
  wr(REG_SEQ0, fx);
  wr(REG_GO, 1);
}

void haptic_tick() { play(FX_SHARP_TICK_3, 35); }
void haptic_click() { play(FX_SHARP_CLICK_60, 60); }
