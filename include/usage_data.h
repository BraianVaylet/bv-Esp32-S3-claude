#pragma once
#include <stdint.h>

#define MAX_DAYS   7
#define MAX_MODELS 4

struct Window {
    float    pct        = 0.0f;  // 0..100
    uint32_t resetInSec = 0;     // as reported by the bridge
    bool     valid      = false;
};

struct DayCost {
    char  label[4] = {0};  // "Mon"
    float usd      = 0.0f;
};

struct ModelCost {
    char  name[20] = {0};
    float usd      = 0.0f;
    float pct      = 0.0f;
};

// Everything the bridge reports, plus the local bookkeeping the UI needs.
struct UsageSnapshot {
    bool     ok         = false;   // last poll succeeded
    char     error[48]  = {0};
    uint32_t fetchedMs  = 0;       // millis() of the last successful poll
    uint32_t attemptMs  = 0;       // millis() of the last attempt

    // --- plan limits (from anthropic-ratelimit-unified-* headers) ---
    bool     limitsOk   = false;
    char     plan[20]   = {0};     // "max_20x", "pro", ...
    char     status[16] = {0};     // allowed | allowed_warning | rejected
    Window   session;              // rolling 5h window
    Window   week;                 // 7d, all models
    Window   weekOpus;             // 7d, Opus only

    // --- local transcript accounting ---
    bool      costOk        = false;
    float     todayUsd      = 0.0f;
    float     monthUsd      = 0.0f;
    float     sessionUsd    = 0.0f;
    DayCost   days[MAX_DAYS];
    uint8_t   dayCount      = 0;
    ModelCost models[MAX_MODELS];
    uint8_t   modelCount    = 0;
    uint64_t  tokIn         = 0;
    uint64_t  tokOut        = 0;
    uint64_t  tokCacheRead  = 0;
    uint64_t  tokCacheWrite = 0;
    uint32_t  lastActivitySec = 0;
};

// Live countdown: the bridge sends seconds-remaining, the device ticks it down
// so no clock sync or timezone handling is needed anywhere.
static inline uint32_t window_remaining(const Window &w, uint32_t fetchedMs, uint32_t nowMs)
{
    if (!w.valid) return 0;
    const uint32_t elapsed = (nowMs - fetchedMs) / 1000;
    return (elapsed >= w.resetInSec) ? 0 : (w.resetInSec - elapsed);
}
