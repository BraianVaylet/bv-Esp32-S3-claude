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

// Shuts the device down and does not return.
//
// On battery this is a real power off: dropping the BAT_EN latch removes power
// outright. On USB it cannot be — VBUS feeds the board without passing through
// the latch — so it falls back to deep sleep, which is as close to off as the
// hardware allows. Either way the BOOT button brings it back.
void board_power_off() __attribute__((noreturn));
