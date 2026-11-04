#pragma once
#include <Arduino.h>

#include <vector>

// Receivers to pick from: ones on the local network (mDNS _ubersdr._tcp, as the
// other UberSDR clients find them) first, then the public directory
// (instances.ubersdr.org).
struct Instance {
  String id;  // the receiver's UUID (directory id / description public_uuid)
  String callsign;
  String name;
  String location;
  String url;  // public_url, e.g. https://n2epe.tunnel.ubersdr.org/
  int available = 0;
  int max_clients = 0;
  int snr = 0;  // snr_0_30_mhz (public list only)
  bool local = false;  // found on the LAN (url is http://ip:port/)
  float lat = 0, lon = 0;
};

enum class FetchState { Idle, Loading, Done, Error };

void instances_fetch_async();
FetchState instances_state();
const String &instances_error();
const std::vector<Instance> &instances_list();  // local ones first
int instances_local_count();                    // how many of the list's head are local

// Favourites, by receiver UUID (saved in settings). A favourite that isn't in
// the current list just isn't shown; it's back when the receiver is.
bool instances_is_fav(const String &id);
bool instances_toggle_fav(const String &id);  // returns true if it's now a favourite
