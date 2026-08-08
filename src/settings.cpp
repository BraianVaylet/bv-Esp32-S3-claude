#include <Preferences.h>

#include "app_config.h"
#include "settings.h"

Settings g_settings;

static Preferences prefs;

void settings_load()
{
    prefs.begin(NVS_NS, true);
    g_settings.ssid        = prefs.getString("ssid", "");
    g_settings.pass        = prefs.getString("pass", "");
    g_settings.bridgeHost  = prefs.getString("host", "");
    g_settings.bridgePort  = prefs.getUShort("port", DEFAULT_BRIDGE_PORT);
    g_settings.bridgeToken = prefs.getString("token", "");
    g_settings.pollMs      = prefs.getULong("poll", DEFAULT_POLL_MS);
    g_settings.brightness  = prefs.getUChar("bl", 255);
    g_settings.alerts      = prefs.getBool("alerts", true);
    g_settings.volume      = prefs.getUChar("vol", 70);
    g_settings.quietStart  = prefs.getUShort("qs", 23 * 60);  // 23:00
    g_settings.quietEnd    = prefs.getUShort("qe", 8 * 60);   // 08:00
    prefs.end();

    if (g_settings.pollMs < 5000) g_settings.pollMs = 5000;
}

void settings_save()
{
    prefs.begin(NVS_NS, false);
    prefs.putString("ssid",  g_settings.ssid);
    prefs.putString("pass",  g_settings.pass);
    prefs.putString("host",  g_settings.bridgeHost);
    prefs.putUShort("port",  g_settings.bridgePort);
    prefs.putString("token", g_settings.bridgeToken);
    prefs.putULong("poll",   g_settings.pollMs);
    prefs.putUChar("bl",     g_settings.brightness);
    prefs.putBool("alerts",  g_settings.alerts);
    prefs.putUChar("vol",    g_settings.volume);
    prefs.putUShort("qs",    g_settings.quietStart);
    prefs.putUShort("qe",    g_settings.quietEnd);
    prefs.end();
}

void settings_clear()
{
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
}
