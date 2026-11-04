#pragma once
#include <Arduino.h>

// Station connect with timeout. Returns true when connected.
bool wifi_connect(const String &ssid, const String &pass, uint32_t timeout_ms,
                  void (*pump)() = nullptr);

// Start the setup hotspot + captive portal. Saving the form stores the
// credentials and reboots.
void wifi_setup_start();
void wifi_setup_loop();
bool wifi_setup_active();
String wifi_setup_ap_name();
