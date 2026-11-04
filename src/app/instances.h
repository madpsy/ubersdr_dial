#pragma once
#include <Arduino.h>

#include <vector>

// Public receiver directory (instances.ubersdr.org).
struct Instance {
  String id;
  String callsign;
  String name;
  String location;
  String url;  // public_url, e.g. https://n2epe.tunnel.ubersdr.org/
  int available = 0;
  int max_clients = 0;
  int snr = 0;  // snr_0_30_mhz
  float lat = 0, lon = 0;
};

enum class FetchState { Idle, Loading, Done, Error };

void instances_fetch_async();
FetchState instances_state();
const String &instances_error();
const std::vector<Instance> &instances_list();
