#pragma once
#include "ctrl_link.h"

// Bluetooth transport for controller mode: the dial as a BLE peripheral the
// UberSDR v2 page reaches over Web Bluetooth. GATT layout: docs/PROTOCOL.md.
// Use it through ctrl_start(&kBleTransport).
