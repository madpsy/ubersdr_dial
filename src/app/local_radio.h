#pragma once
#include "dial_link.h"

// Receiver mode's far end: the dial is the radio. Holds the tuning, view,
// filter, squelch and volume, drives the spectrum and audio sockets, and
// answers the dial screen's codes as the v2 page would (see dial_link.h).
void local_radio_start();  // connect to g_settings.inst_url with the saved view
void local_radio_stop();   // disconnect, stop audio, save the view
