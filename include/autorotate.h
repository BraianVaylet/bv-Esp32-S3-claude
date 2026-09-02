#pragma once
#include <stdint.h>

// Follows the device's physical orientation and turns the UI to match.
//
// Call from loop(); it rate-limits itself. Does nothing unless the settings
// have rotation set to automatic and the IMU actually answered at boot.
void autorotate_tick();

// Applies whatever the settings currently ask for: a fixed rotation, or one
// immediate IMU-derived reading when set to automatic. Call once at startup
// and again after settings change.
void autorotate_apply_setting();

// Last orientation the IMU resolved, 0..3, for display on the SYSTEM screen.
uint8_t autorotate_current();

// True when the device is lying too flat for gravity to indicate orientation.
bool autorotate_is_flat();
