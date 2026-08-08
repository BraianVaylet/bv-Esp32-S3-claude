#pragma once
#include <lvgl.h>

//
// Anthropic palette — dark only.
//
// Neutrals and accents are taken from Anthropic's brand ramp. There is no
// green/red traffic light anywhere: usage severity is expressed as an
// intensity ramp across the three warm accents (Manilla -> Kraft ->
// Book Cloth -> Crail), which keeps the whole UI inside the brand.
//

// Neutral ramp (dark -> light)
#define CC_BLACK      0x141413
#define CC_BG         0x191919
#define CC_SURFACE    0x262625
#define CC_SURFACE_HI 0x30302E
#define CC_BORDER     0x40403E
#define CC_MUTED      0x666663
#define CC_DIM        0x91918D
#define CC_SUBTLE     0xBFBFBA
#define CC_TEXT       0xFAFAF7

// Warm accent ramp (light -> deep)
#define CC_MANILLA    0xEBDBBC
#define CC_KRAFT      0xD4A27F
#define CC_CLAUDE     0xD97757  // Claude orange — primary accent
#define CC_BOOKCLOTH  0xCC785C
#define CC_CRAIL      0xC15F3C  // deepest — reserved for "nearly out"

static inline lv_color_t cc(uint32_t hex) { return lv_color_hex(hex); }

// Usage severity -> accent. Intensity, not hue-shift.
static inline uint32_t cc_level_color(float pct)
{
    if (pct >= 90.0f) return CC_CRAIL;
    if (pct >= 75.0f) return CC_BOOKCLOTH;
    if (pct >= 40.0f) return CC_CLAUDE;
    return CC_KRAFT;
}

// Fonts used across the UI
#define F_XS   &lv_font_montserrat_10
#define F_SM   &lv_font_montserrat_12
#define F_MD   &lv_font_montserrat_14
#define F_LG   &lv_font_montserrat_16
#define F_XL   &lv_font_montserrat_20
#define F_2XL  &lv_font_montserrat_28
#define F_HERO &lv_font_montserrat_40
