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

    // Spoken alerts
    bool     alerts;      // master switch
    uint8_t  volume;      // 0..100
    uint16_t quietStart;  // minutes since local midnight
    uint16_t quietEnd;    // wraps past midnight when end < start

    bool configured() const { return ssid.length() > 0 && bridgeHost.length() > 0; }

    // `nowMinutes` comes from the bridge, which reads the PC's wall clock —
    // the device has no RTC and no timezone. A zero-length range is never quiet.
    bool isQuiet(uint16_t nowMinutes) const
    {
        if (quietStart == quietEnd) return false;
        if (quietStart < quietEnd) return nowMinutes >= quietStart && nowMinutes < quietEnd;
        return nowMinutes >= quietStart || nowMinutes < quietEnd;  // spans midnight
    }
};

extern Settings g_settings;

void settings_load();
void settings_save();
void settings_clear();
