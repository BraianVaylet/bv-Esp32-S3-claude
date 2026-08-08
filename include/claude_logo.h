#pragma once
#include <lvgl.h>

// Draws the Claude Code starburst mark procedurally into an LVGL canvas.
//
// It is rasterised at runtime rather than shipped as a bitmap so it stays
// crisp at any size and the repo carries no copied brand asset. `rays` = 0
// picks a sensible count for the requested size.
lv_obj_t *claude_logo_create(lv_obj_t *parent, int size, uint32_t color, int rays = 0);

// The Claude Code mascot, drawn as a block grid. `block` is the size of one
// pixel-art cell; the result is 16*block wide and 9*block tall.
lv_obj_t *claude_code_logo_create(lv_obj_t *parent, int block, uint32_t color);

#define CLAWD_COLS 16
#define CLAWD_ROWS 9
