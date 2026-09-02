#include <Arduino.h>
#include <math.h>

#include "autorotate.h"
#include "display.h"
#include "imu.h"
#include "settings.h"

// Solved from two measurements, each taken with the accelerometer sampled at
// the same moment the rotation was confirmed correct by eye:
//
//   atan2(ay, ax) ~= 7 degrees  -> panel rotation 3
//   atan2(ay, ax) ~= 80 degrees -> panel rotation 0
//
// which give (REF + 2*DIR) = 3 and (REF + DIR) = 0 mod 4, so DIR = -1 and
// REF = 1. Rotation runs opposite to the accelerometer's sense.
//
// Two measurements are the minimum, and they have to be a quarter turn apart.
// A pair half a turn apart cannot determine DIR at all: the step is +-2, and
// +2 and -2 are the same value mod 4, so both signs predict the same rotation
// and the mapping looks confirmed while still being wrong at the other two
// orientations.
//
// None of this is visible in a screenshot. Those come from LVGL's snapshot of
// the *logical* screen, which renders upright at every panel rotation, so the
// calibration can only be checked on the actual glass.
static constexpr float REF_ANGLE    = 180.0f;
static constexpr int   REF_ROTATION = 1;
static constexpr int   DIR          = -1;

// Below this, gravity is too close to the screen's normal to say which way is
// up — the device is lying flat on a desk. Hold the last orientation instead
// of letting noise pick one.
static constexpr float FLAT_THRESHOLD_G = 0.35f;

// A candidate has to hold for this many consecutive samples before the screen
// turns, so a device being picked up and set down does not flicker through
// two or three orientations on the way.
static constexpr int   STABLE_SAMPLES = 4;
static constexpr uint32_t SAMPLE_MS   = 200;

static uint8_t  s_current   = REF_ROTATION;
static uint8_t  s_candidate = REF_ROTATION;
static int      s_stable    = 0;
static bool     s_flat      = false;
static uint32_t s_lastMs    = 0;

// Returns false when the reading is unusable (no IMU, or lying flat).
static bool measure(uint8_t *out)
{
    float ax, ay, az;
    if (!imu_read(&ax, &ay, &az)) return false;

    const float planar = sqrtf(ax * ax + ay * ay);
    s_flat = planar < FLAT_THRESHOLD_G;
    if (s_flat) return false;

    const float angle = atan2f(ay, ax) * 180.0f / (float)M_PI;
    const int   step  = (int)lroundf((REF_ANGLE - angle) / 90.0f) * DIR;

    *out = (uint8_t)((REF_ROTATION + step) & 3);
    return true;
}

void autorotate_apply_setting()
{
    // settings.rotation: 0 = automatic, 1..4 = fixed rotation 0..3.
    if (g_settings.rotation != 0) {
        display_set_rotation((uint8_t)((g_settings.rotation - 1) & 3));
        return;
    }

    uint8_t r;
    if (imu_available() && measure(&r)) {
        s_current = s_candidate = r;
        display_set_rotation(r);
    }
}

void autorotate_tick()
{
    if (g_settings.rotation != 0 || !imu_available()) return;

    const uint32_t now = millis();
    if (now - s_lastMs < SAMPLE_MS) return;
    s_lastMs = now;

    uint8_t r;
    if (!measure(&r)) return;

    if (r != s_candidate) {
        s_candidate = r;
        s_stable    = 0;
        return;
    }
    if (s_candidate == s_current) return;
    if (++s_stable < STABLE_SAMPLES) return;

    s_current = s_candidate;
    display_set_rotation(s_current);
    Serial.printf("[rot] orientation -> %u\n", s_current);
}

uint8_t autorotate_current() { return s_current; }
bool    autorotate_is_flat() { return s_flat; }
