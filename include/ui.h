#pragma once
#include "usage_data.h"

void ui_begin();

// Repaints every widget from the snapshot. Cheap enough to call ~2 Hz.
void ui_update(const UsageSnapshot &snap);

void ui_next_screen();
void ui_show_toast(const char *msg);
