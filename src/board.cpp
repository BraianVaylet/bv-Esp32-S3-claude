#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "board.h"
#include "board_config.h"

struct Button {
    uint8_t  pin;
    bool     down       = false;
    uint32_t downMs     = 0;
    bool     holdFired  = false;
};

static Button s_boot{BTN_BOOT};
static Button s_pwr{BTN_PWR};
static Button s_user{BTN_USER};

static constexpr uint32_t DEBOUNCE_MS  = 40;
static constexpr uint32_t HOLD_MS      = 1200;
static uint32_t s_lastBatSampleMs = 0;
static uint16_t s_batMv           = 0;

void board_begin()
{
    // Power-hold latch first — everything else can wait.
    pinMode(BAT_EN, OUTPUT);
    digitalWrite(BAT_EN, HIGH);

    pinMode(BTN_BOOT, INPUT_PULLUP);
    pinMode(BTN_PWR,  INPUT_PULLUP);
    pinMode(BTN_USER, INPUT_PULLUP);

    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
}

// Returns true on a clean release (tap). Sets *hold when the hold threshold
// is crossed while still pressed.
static bool update(Button &b, bool *hold)
{
    const bool pressed = digitalRead(b.pin) == LOW;
    const uint32_t now = millis();

    if (pressed && !b.down) {
        b.down      = true;
        b.downMs    = now;
        b.holdFired = false;
        return false;
    }

    if (pressed && b.down && !b.holdFired && now - b.downMs > HOLD_MS) {
        b.holdFired = true;
        Serial.printf("[btn] GPIO%u hold\n", b.pin);
        if (hold) *hold = true;
        return false;
    }

    if (!pressed && b.down) {
        b.down = false;
        const uint32_t held = now - b.downMs;
        // Logged for every release, so a button that is wired to a different
        // GPIO than expected shows up immediately instead of being guessed at.
        Serial.printf("[btn] GPIO%u released after %lums%s\n", b.pin,
                      (unsigned long)held, b.holdFired ? " (hold already fired)" : "");
        return !b.holdFired && held > DEBOUNCE_MS;
    }

    return false;
}

BtnEvent board_poll_buttons()
{
    bool hold = false;

    if (update(s_boot, nullptr)) return BtnEvent::BootTap;
    if (update(s_pwr, &hold))    return BtnEvent::PwrTap;
    if (hold)                    return BtnEvent::PwrHold;
    if (update(s_user, nullptr)) return BtnEvent::UserTap;

    return BtnEvent::None;
}

uint16_t board_battery_mv()
{
    const uint32_t now = millis();
    if (s_batMv && now - s_lastBatSampleMs < 5000) return s_batMv;
    s_lastBatSampleMs = now;

    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(BAT_ADC_PIN);
    s_batMv = static_cast<uint16_t>((acc / 8) * BAT_VOLT_DIVIDER);
    return s_batMv;
}

uint8_t board_battery_pct()
{
    const float mv = board_battery_mv();
    if (mv <= BAT_EMPTY_MV) return 0;
    if (mv >= BAT_FULL_MV)  return 100;
    return static_cast<uint8_t>((mv - BAT_EMPTY_MV) * 100.0f / (BAT_FULL_MV - BAT_EMPTY_MV));
}

// No PMU on this board, so "on USB" is inferred: the charger holds the pack
// above the li-ion ceiling while plugged in.
bool board_on_usb() { return board_battery_mv() > 4250; }

void board_power_off()
{
    // Silence the amplifier first, or the rail collapsing puts a pop through
    // the speaker.
    pinMode(SND_PA_PIN, OUTPUT);
    digitalWrite(SND_PA_PIN, LOW);

    Serial.println("[power] shutting down");
    Serial.flush();

    // On battery this is the end of the line: the latch is the only thing
    // keeping the rail up.
    digitalWrite(BAT_EN, LOW);
    delay(150);

    // Still running means USB is supplying VBUS and the latch cannot win.
    // Deep sleep is the closest thing to off, woken by BOOT going low. The
    // pull-up has to be held through sleep or the pad floats and the board
    // wakes on noise.
    rtc_gpio_pullup_en((gpio_num_t)BTN_BOOT);
    rtc_gpio_pulldown_dis((gpio_num_t)BTN_BOOT);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_BOOT, 0);
    esp_deep_sleep_start();

    while (true) {}  // unreachable; keeps the noreturn contract honest
}
