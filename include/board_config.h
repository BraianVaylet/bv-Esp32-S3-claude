#pragma once
//
// Waveshare ESP32-S3-Touch-LCD-1.54
// 240x240 ST7789 (4-wire SPI) + CST816 touch + QMI8658 IMU + ES8311 codec.
//
// Pin map cross-checked against two independent sources that agree exactly:
//   1. Waveshare's own Arduino demo
//      examples/ESP32-S3-Touch-LCD-1.54-demo/Arduino-3.2.0/examples/09_lvgl_arduino_v9
//      (github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54)
//   2. Clawdmeter's hardware-tested board profile
//      firmware/src/boards/waveshare_lcd_154/board.h
//

#define BOARD_NAME "ESP32-S3-Touch-LCD-1.54"

// ---- Panel geometry ----
#define LCD_WIDTH  240
#define LCD_HEIGHT 240

// ---- ST7789 over SPI ----
#define LCD_SCLK 38
#define LCD_MOSI 39
#define LCD_MISO -1
#define LCD_CS   21
#define LCD_DC   45
#define LCD_RST  40
#define LCD_BL   46  // backlight — LEDC PWM, the panel has no brightness command

// ---- Shared I2C bus: CST816 touch + QMI8658 IMU + ES8311 codec ----
#define I2C_SDA 42
#define I2C_SCL 41

// ---- CST816 capacitive touch ----
#define TP_INT  48
#define TP_RST  47
#define TP_ADDR 0x15

// ---- Power / battery ----
// VBAT goes through a 3.0x divider into GPIO1. BAT_EN is a power-hold latch:
// it MUST be driven HIGH early in boot or the board browns out on battery.
#define BAT_EN            2
#define BAT_ADC_PIN       1
#define BAT_VOLT_DIVIDER  3.0f
#define BAT_FULL_MV       4200.0f
#define BAT_EMPTY_MV      3300.0f

// ---- Audio: ES8311 codec -> NS4150B amplifier -> speaker ----
// Same values in Waveshare's 01_i2s_audio example and Clawdmeter's board
// profile. The codec sits on the shared I2C bus at 0x18; the amplifier enable
// is a plain GPIO, which is what lets us mute without touching I2C.
#define SND_I2S_MCLK 8
#define SND_I2S_BCLK 9
#define SND_I2S_WS   10  // LRCK
#define SND_I2S_DOUT 12  // ESP -> codec
#define SND_PA_PIN   7   // amplifier enable, HIGH = on

// ---- Buttons (active LOW) ----
#define BTN_BOOT 0  // left  — next screen
#define BTN_PWR  4  // mid   — cycle backlight brightness (long hold = power off)
#define BTN_USER 5  // right — force refresh
