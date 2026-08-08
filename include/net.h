#pragma once
#include <Arduino.h>

enum class NetState { Boot, Connecting, Online, Portal, Failed };

void     net_begin();
void     net_loop();
NetState net_state();

String   net_ip();
String   net_ssid();
int      net_rssi();

// Drops stored credentials and reboots into the setup portal.
void net_start_portal();
