#pragma once

#define APP_NAME    "Claude Usage"
#define APP_VERSION "1.0.0"

// SoftAP shown when no WiFi credentials are stored yet.
#define AP_SSID     "claude-usage-setup"
#define AP_PASSWORD "claudecode"

// Defaults for the desk bridge (overridable from the setup portal).
#define DEFAULT_BRIDGE_PORT 8787
#define DEFAULT_POLL_MS     15000

// HTTP
#define HTTP_TIMEOUT_MS 6000

// NVS namespace
#define NVS_NS "cusage"

// Backlight levels cycled by the PWR button
#define BL_LEVELS_COUNT 4
static const uint8_t BL_LEVELS[BL_LEVELS_COUNT] = {255, 160, 80, 25};
