#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <math.h>

#include "audio.h"
#include "board_config.h"
#include "es8311.h"

// The codec runs at a fixed hardware volume; everything the user controls is
// applied to the samples instead, so the I2C bus is never needed again after
// boot. See the note in audio.h.
static constexpr int CODEC_VOLUME     = 80;
static constexpr int MCLK_MULTIPLE    = 256;
static constexpr i2s_port_t I2S_PORT  = I2S_NUM_0;

static bool    s_ready  = false;
static uint8_t s_volume = 70;

static bool codec_init()
{
    Wire.begin(I2C_SDA, I2C_SCL);

    es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!es) {
        Serial.println("[audio] es8311_create failed");
        Wire.end();
        return false;
    }

    const es8311_clock_config_t clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = AUDIO_SAMPLE_RATE * MCLK_MULTIPLE,
        .sample_frequency   = AUDIO_SAMPLE_RATE,
    };

    bool ok = es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK;
    ok = ok && es8311_sample_frequency_config(es, AUDIO_SAMPLE_RATE * MCLK_MULTIPLE,
                                              AUDIO_SAMPLE_RATE) == ESP_OK;
    ok = ok && es8311_voice_volume_set(es, CODEC_VOLUME, nullptr) == ESP_OK;
    ok = ok && es8311_microphone_config(es, false) == ESP_OK;

    es8311_delete(es);
    Wire.end();  // hand I2C port 0 back before LovyanGFX claims it for touch

    if (!ok) Serial.println("[audio] es8311 configuration failed");
    return ok;
}

static bool i2s_init()
{
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = AUDIO_SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.fixed_mclk           = AUDIO_SAMPLE_RATE * MCLK_MULTIPLE;
    cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;

    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = SND_I2S_MCLK;
    pins.bck_io_num   = SND_I2S_BCLK;
    pins.ws_io_num    = SND_I2S_WS;
    pins.data_out_num = SND_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;

    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

bool audio_begin()
{
    pinMode(SND_PA_PIN, OUTPUT);
    digitalWrite(SND_PA_PIN, LOW);  // amplifier off until something plays

    if (!codec_init()) return false;
    if (!i2s_init())   { Serial.println("[audio] i2s init failed"); return false; }

    s_ready = true;
    Serial.println("[audio] ready");
    return true;
}

void audio_set_volume(uint8_t percent) { s_volume = percent > 100 ? 100 : percent; }
uint8_t audio_get_volume()             { return s_volume; }

static void apply_gain(int16_t *samples, size_t count)
{
    if (s_volume >= 100) return;
    const int32_t g = s_volume;
    for (size_t i = 0; i < count; i++) samples[i] = (int16_t)((samples[i] * g) / 100);
}

// The amplifier is only powered while audio is actually moving: leaving it on
// puts an audible hiss through the speaker.
static void amp(bool on)
{
    digitalWrite(SND_PA_PIN, on ? HIGH : LOW);
    if (on) delay(20);  // let the NS4150B settle so the first word is not clipped
}

bool audio_play_pcm(Stream &src, size_t bytes)
{
    if (!s_ready || !bytes) return false;

    static constexpr size_t CHUNK = 1024;
    int16_t *buf = (int16_t *)malloc(CHUNK);
    if (!buf) return false;

    amp(true);

    size_t remaining = bytes;
    uint32_t idleSince = millis();
    bool ok = true;

    while (remaining > 0) {
        const size_t want = remaining < CHUNK ? remaining : CHUNK;
        const int got = src.readBytes((char *)buf, want);

        if (got <= 0) {
            if (millis() - idleSince > 4000) { ok = false; break; }
            delay(5);
            continue;
        }
        idleSince = millis();
        remaining -= got;

        apply_gain(buf, got / 2);

        size_t written = 0;
        i2s_write(I2S_PORT, buf, got, &written, portMAX_DELAY);
    }

    // Push silence so the DMA ring drains before the amplifier cuts out,
    // otherwise the tail of the phrase is clipped with a pop.
    memset(buf, 0, CHUNK);
    for (int i = 0; i < 4; i++) {
        size_t written = 0;
        i2s_write(I2S_PORT, buf, CHUNK, &written, portMAX_DELAY);
    }

    amp(false);
    free(buf);
    return ok;
}

void audio_test_tone()
{
    if (!s_ready) return;

    static constexpr int MS = 160;
    static constexpr int N  = AUDIO_SAMPLE_RATE * MS / 1000;
    int16_t *buf = (int16_t *)malloc(N * sizeof(int16_t));
    if (!buf) return;

    amp(true);
    for (int note = 0; note < 2; note++) {
        const float freq = note ? 988.0f : 659.0f;  // a short rising two-note chirp
        for (int i = 0; i < N; i++) {
            // Fade both ends of the note so it does not click.
            const float env = sinf((float)M_PI * i / N);
            buf[i] = (int16_t)(6000 * env * sinf(2.0f * (float)M_PI * freq * i / AUDIO_SAMPLE_RATE));
        }
        apply_gain(buf, N);
        size_t written = 0;
        i2s_write(I2S_PORT, buf, N * sizeof(int16_t), &written, portMAX_DELAY);
    }
    amp(false);
    free(buf);
}
