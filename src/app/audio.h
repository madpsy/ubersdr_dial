#pragma once
#include <Arduino.h>

// Opus audio from UberSDR's /ws socket -> PCM5100A over I2S.
// Runs its own tasks on core 0; all calls here are safe from the UI loop.

void audio_init();

// Session shared with the spectrum socket. Pass gen=0 / empty uuid to stop.
void audio_set_session(const String &host, uint16_t port, bool tls, const String &prefix, const String &uuid,
                       uint32_t gen);
void audio_stop();

// mode: usb lsb cwu cwl am sam fm nfm. lo/hi: passband edges in Hz.
void audio_tune(uint32_t freq_hz, const char *mode, int lo_hz, int hi_hz);
void audio_set_volume(uint8_t vol_0_100, bool muted);
// Server-side audio gate (squelch): pass a min SNR in dB, or -999 for off.
void audio_set_squelch(float min_snr_db);

void audio_test_tone(uint32_t ms);
void audio_i2s_enable(bool on);  // debug  // debug: 1 kHz tone, no network
bool audio_has_session();  // false after audio_stop()
bool audio_live();      // frames arriving
float audio_snr_db();          // latest reading received; NAN when unknown
float audio_snr_db_playing();  // reading for the audio coming out of the speaker now
float audio_dbfs_playing();    // its baseband power, dBFS (the S-meter's input); NAN when unknown
