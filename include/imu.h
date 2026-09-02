#pragma once
#include <stdint.h>

// Minimal QMI8658 accelerometer driver — just enough to tell which way is
// down, so the UI can follow the device when it is turned.
//
// Deliberately not the vendor SensorLib: that talks over Arduino's `Wire`,
// which would put a second master on the I2C bus LovyanGFX already owns for
// the CST816 touch controller (see the note in audio.h for the same problem
// with the ES8311 codec). This reads through lgfx::i2c on that same port
// instead, so the bus keeps exactly one owner, and every access happens from
// loop() so there is no concurrency to guard either.
//
// Gyroscope stays powered down. Orientation only needs gravity, and leaving
// it off keeps the accelerometer's low-power modes available.

// Must be called after display_begin(), which is what initialises the I2C
// port this rides on. Returns false if the chip does not answer.
bool imu_begin();

bool imu_available();

// Latest reading in g. Returns false if the sample could not be read.
bool imu_read(float *ax, float *ay, float *az);
