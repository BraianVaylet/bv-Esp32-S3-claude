#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_config.h"
#include "imu.h"

// Register map and init sequence taken from the QMI8658 constants and driver
// that ship with Waveshare's own demo for this board (SensorLib), not from
// memory.
static constexpr int     I2C_PORT = 0;      // the port LovyanGFX opened for touch
static constexpr uint8_t ADDR     = 0x6B;   // QMI8658_L_SLAVE_ADDRESS
static constexpr uint32_t FREQ    = 400000;

static constexpr uint8_t REG_WHOAMI = 0x00;  // reads 0x05
static constexpr uint8_t REG_CTRL1  = 0x02;
static constexpr uint8_t REG_CTRL2  = 0x03;
static constexpr uint8_t REG_CTRL7  = 0x08;
static constexpr uint8_t REG_AX_L   = 0x35;

static constexpr uint8_t WHOAMI_EXPECTED = 0x05;

// CTRL2 packs the accelerometer range in bits [6:4] and the output data rate
// in [3:0]. 2 g gives the finest resolution, and gravity never exceeds it.
// ODR 13 is the 21 Hz low-power mode — only selectable with the gyro off, and
// still four times the rate orientation is actually sampled at.
static constexpr uint8_t ACC_RANGE_2G = 0;
static constexpr uint8_t ACC_ODR_LP_21HZ = 13;
static constexpr float   ACC_SCALE_2G = 2.0f / 32768.0f;

static bool s_ready = false;

static bool read8(uint8_t reg, uint8_t *out)
{
    auto r = lgfx::i2c::readRegister8(I2C_PORT, ADDR, reg, FREQ);
    if (!r.has_value()) return false;
    *out = r.value();
    return true;
}

static bool write8(uint8_t reg, uint8_t value)
{
    return lgfx::i2c::writeRegister8(I2C_PORT, ADDR, reg, value, 0, FREQ).has_value();
}

bool imu_begin()
{
    uint8_t who = 0;
    if (!read8(REG_WHOAMI, &who) || who != WHOAMI_EXPECTED) {
        Serial.printf("[imu] not found (WHO_AM_I=0x%02X, expected 0x%02X)\n",
                      who, WHOAMI_EXPECTED);
        return false;
    }

    // ADDR_AI: auto-increment the register pointer, so the six accelerometer
    // bytes come back in one burst read instead of six transactions.
    if (!write8(REG_CTRL1, 0x40)) return false;
    if (!write8(REG_CTRL2, (uint8_t)((ACC_RANGE_2G << 4) | ACC_ODR_LP_21HZ))) return false;
    if (!write8(REG_CTRL7, 0x01)) return false;  // accelerometer on, gyro off

    s_ready = true;
    Serial.println("[imu] QMI8658 ready (accelerometer only)");
    return true;
}

bool imu_available() { return s_ready; }

bool imu_read(float *ax, float *ay, float *az)
{
    if (!s_ready) return false;

    uint8_t raw[6];
    if (!lgfx::i2c::readRegister(I2C_PORT, ADDR, REG_AX_L, raw, sizeof(raw), FREQ).has_value())
        return false;

    const int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    const int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    const int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    if (ax) *ax = x * ACC_SCALE_2G;
    if (ay) *ay = y * ACC_SCALE_2G;
    if (az) *az = z * ACC_SCALE_2G;
    return true;
}
