#pragma once
#include <Arduino.h>

// Over-the-air updates, from the same files the web flasher uses (GitHub
// Pages, docs/flash/). release.sh publishes ota.json next to firmware.bin:
//   {"version": "1.9.2", "size": 1985568, "sha256": "<hex>"}
//
// Safe by construction:
//  - The new firmware goes into the other app slot; the running one is never
//    touched. Power loss or a failed download leaves the dial as it was.
//  - HTTPS with a verified certificate, and the download must match ota.json's
//    size and SHA-256 before the dial will boot it (and the image's own
//    checksum when it's marked bootable).
//  - The bootloader rolls back: the new firmware boots on trial and must call
//    ota_mark_good(). If it resets first (crash, hang + watchdog, power cut)
//    the bootloader starts the previous one again, and that version isn't
//    offered automatically again.
//  - Only ever newer versions; nothing is installed without a tap.

enum class OtaState { Idle, Checking, UpToDate, Available, Installing, Done, Error };

void ota_boot();       // early in setup(): notes a fresh update on trial, or a rollback
void ota_mark_good();  // this firmware works: keep it (call before any deliberate restart)
void ota_tick();       // every loop: marks good after a while running

// Background check (core 0). join_wifi: connect with the saved network first
// (from the start-up settings, where Wi-Fi is still off).
void ota_check_async(bool join_wifi);
// Download and install what the check found (state Available). Done = restart.
void ota_install_async();

OtaState ota_state();
const String &ota_error();
const String &ota_latest();  // version on offer (after a check)
bool ota_latest_failed();     // that version was installed before and rolled back
float ota_progress();         // 0..1 while installing

// Is this a release build (a version an update can be compared with)?
bool ota_version_known();
