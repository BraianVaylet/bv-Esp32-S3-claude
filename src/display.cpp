#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "display.h"

// ---------------------------------------------------------------- panel ----

class LGFX_WS154 : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789   _panel;
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _light;
    lgfx::Touch_CST816S  _touch;

public:
    LGFX_WS154()
    {
        {
            auto c = _bus.config();
            c.spi_host    = SPI2_HOST;
            c.spi_mode    = 0;
            c.freq_write  = 80000000;
            c.freq_read   = 16000000;
            c.spi_3wire   = false;
            c.use_lock    = true;
            c.dma_channel = SPI_DMA_CH_AUTO;
            c.pin_sclk    = LCD_SCLK;
            c.pin_mosi    = LCD_MOSI;
            c.pin_miso    = LCD_MISO;
            c.pin_dc      = LCD_DC;
            _bus.config(c);
            _panel.setBus(&_bus);
        }
        {
            auto c = _panel.config();
            c.pin_cs           = LCD_CS;
            c.pin_rst          = LCD_RST;
            c.pin_busy         = -1;
            c.panel_width      = LCD_WIDTH;
            c.panel_height     = LCD_HEIGHT;
            c.memory_width     = 240;
            c.memory_height    = 320;
            c.offset_x         = 0;
            c.offset_y         = 0;
            c.offset_rotation  = 0;
            c.readable         = false;
            c.invert           = true;   // IPS panel
            c.rgb_order        = false;
            c.dlen_16bit       = false;
            c.bus_shared       = false;
            _panel.config(c);
        }
        {
            auto c = _light.config();
            c.pin_bl      = LCD_BL;
            c.invert      = false;
            c.freq        = 12000;
            c.pwm_channel = 7;
            _light.config(c);
            _panel.setLight(&_light);
        }
        {
            auto c = _touch.config();
            c.x_min      = 0;
            c.x_max      = LCD_WIDTH - 1;
            c.y_min      = 0;
            c.y_max      = LCD_HEIGHT - 1;
            c.pin_int    = TP_INT;
            c.pin_rst    = TP_RST;
            c.bus_shared = false;
            c.offset_rotation = 0;
            c.i2c_port   = 0;
            c.i2c_addr   = TP_ADDR;
            c.pin_sda    = I2C_SDA;
            c.pin_scl    = I2C_SCL;
            c.freq       = 400000;
            _touch.config(c);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};

static LGFX_WS154 tft;
static uint8_t s_brightness = 255;

// ------------------------------------------------------------- LVGL glue ----

// Two partial buffers, 1/6 of the panel each. Kept in internal RAM so the SPI
// push is fast; PSRAM would work but costs a cache miss per scanline.
static constexpr uint32_t DRAW_LINES = LCD_HEIGHT / 6;
static constexpr uint32_t DRAW_PX    = LCD_WIDTH * DRAW_LINES;

static uint8_t *s_buf1 = nullptr;
static uint8_t *s_buf2 = nullptr;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.pushImage(area->x1, area->y1, w, h, reinterpret_cast<lgfx::rgb565_t *>(px_map));
    tft.endWrite();

    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (tft.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t tick_cb() { return millis(); }

bool display_begin()
{
    if (!tft.init()) return false;
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
    tft.setBrightness(s_brightness);

    lv_init();
    lv_tick_set_cb(tick_cb);

    const size_t buf_bytes = DRAW_PX * sizeof(uint16_t);
    s_buf1 = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    s_buf2 = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!s_buf1) return false;

    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    return true;
}

void display_set_brightness(uint8_t level)
{
    s_brightness = level;
    tft.setBrightness(level);
}

uint8_t display_get_brightness() { return s_brightness; }
