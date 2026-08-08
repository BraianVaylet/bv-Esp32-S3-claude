#pragma once
#include <stdint.h>

// Brings up LovyanGFX (ST7789 + CST816) and wires it into LVGL 9.
// Must be called before any lv_* call.
bool display_begin();

// 0..255, persisted by the caller.
void display_set_brightness(uint8_t level);
uint8_t display_get_brightness();
