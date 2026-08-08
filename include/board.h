#pragma once
#include <stdint.h>

enum class BtnEvent { None, BootTap, PwrTap, UserTap, PwrHold };

// Drives the BAT_EN power latch and configures ADC + buttons.
// Call first in setup(): on battery, the board browns out without the latch.
void board_begin();

// Poll from the main loop. Returns at most one event per call.
BtnEvent board_poll_buttons();

// Battery pack voltage in mV, and a rough 0..100 state of charge.
uint16_t board_battery_mv();
uint8_t  board_battery_pct();
bool     board_on_usb();
