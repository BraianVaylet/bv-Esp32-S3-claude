#pragma once
#include <lvgl.h>

// Draws the Claude Code starburst mark procedurally into an LVGL canvas.
//
// It is rasterised at runtime rather than shipped as a bitmap so it stays
// crisp at any size and the repo carries no copied brand asset. `rays` = 0
// picks a sensible count for the requested size.
lv_obj_t *claude_logo_create(lv_obj_t *parent, int size, uint32_t color, int rays = 0);
