#pragma once
#include "usage_data.h"

void ui_begin();

// Repaints every widget from the snapshot. Cheap enough to call ~2 Hz.
void ui_update(const UsageSnapshot &snap);

void ui_next_screen();

// Jumps straight to a tile by index (0..3: PLAN, API VALUE, TOKENS, SYSTEM).
// Out-of-range indices are ignored. Used by GET /goto on the settings server,
// mainly so a screenshot of every screen can be scripted from a PC instead of
// standing at the device pressing BOOT between each /screenshot.bmp request.
void ui_goto_screen(int index);
void ui_show_toast(const char *msg);
