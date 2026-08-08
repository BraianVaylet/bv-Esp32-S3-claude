#include <Arduino.h>
#include <lvgl.h>

#include "app_config.h"
#include "audio.h"
#include "board.h"
#include "board_config.h"
#include "display.h"
#include "net.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"
#include "usage_client.h"

static uint32_t s_lastUiMs = 0;

static void cycle_brightness()
{
    int idx = 0;
    for (int i = 0; i < BL_LEVELS_COUNT; i++)
        if (BL_LEVELS[i] == g_settings.brightness) { idx = i; break; }

    idx = (idx + 1) % BL_LEVELS_COUNT;
    g_settings.brightness = BL_LEVELS[idx];
    display_set_brightness(g_settings.brightness);
    settings_save();

    char msg[24];
    snprintf(msg, sizeof(msg), "brightness %d%%", (g_settings.brightness * 100) / 255);
    ui_show_toast(msg);
}

static void handle_buttons()
{
    switch (board_poll_buttons()) {
        case BtnEvent::BootTap:
            ui_next_screen();
            break;
        case BtnEvent::UserTap:
            usage_client_refresh_now();
            ui_show_toast("refreshing");
            break;
        case BtnEvent::PwrTap:
            cycle_brightness();
            break;
        case BtnEvent::PwrHold:
            ui_show_toast("resetting Wi-Fi");
            lv_timer_handler();
            delay(900);
            net_start_portal();  // clears NVS and reboots into the setup AP
            break;
        default:
            break;
    }
}

void setup()
{
    Serial.begin(115200);

    // Power latch first: on battery the board browns out without it.
    board_begin();
    settings_load();

    // Before the display: the codec talks over Arduino's Wire on I2C port 0,
    // which LovyanGFX then claims for the touch controller. audio_begin()
    // configures the codec and hands the bus back. See audio.h.
    if (audio_begin()) audio_set_volume(g_settings.volume);

    if (!display_begin()) {
        Serial.println("[fatal] display init failed");
        while (true) delay(1000);
    }
    display_set_brightness(g_settings.brightness);

    ui_begin();
    net_begin();
    usage_client_begin();

    Serial.printf("[boot] %s v%s on %s\n", APP_NAME, APP_VERSION, BOARD_NAME);
}

void loop()
{
    lv_timer_handler();
    net_loop();
    handle_buttons();

    const uint32_t now = millis();
    if (now - s_lastUiMs >= 400) {
        s_lastUiMs = now;
        UsageSnapshot snap;
        usage_client_get(snap);
        ui_update(snap);
    }

    delay(5);
}
