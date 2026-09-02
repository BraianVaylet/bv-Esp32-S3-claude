#pragma once
#include <stdint.h>

// Brings up LovyanGFX (ST7789 + CST816) and wires it into LVGL 9.
// Must be called before any lv_* call.
bool display_begin();

// 0..255, persisted by the caller.
void display_set_brightness(uint8_t level);
uint8_t display_get_brightness();

// Panel rotation, 0..3 in 90-degree steps.
//
// Done in the panel controller (MADCTL) rather than by rotating pixels in
// software, so it costs no CPU and no extra buffer. The panel is square, so
// LVGL keeps working in the same 240x240 logical space at every rotation and
// none of the hand-computed layout positions need to change. LovyanGFX
// transforms touch coordinates to match, so input stays aligned too.
void display_set_rotation(uint8_t rotation);
uint8_t display_get_rotation();
