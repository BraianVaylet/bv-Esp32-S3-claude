#pragma once
#include <Arduino.h>

struct Settings {
    String   ssid;
    String   pass;
    String   bridgeHost;   // IP or hostname of the PC running bridge.mjs
    uint16_t bridgePort;
    String   bridgeToken;  // optional bearer token
    uint32_t pollMs;
    uint8_t  brightness;

    bool configured() const { return ssid.length() > 0 && bridgeHost.length() > 0; }
};

extern Settings g_settings;

void settings_load();
void settings_save();
void settings_clear();
