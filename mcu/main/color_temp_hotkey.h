#pragma once
#include <stdint.h>

/* Color temperature hotkey, as in ModRetro's firmware: with the menu closed
 * and Menu held, Up makes the picture one step warmer and Down one step cooler
 * (Menu + Left/Right is brightness, Menu + Select/Start the save states). The
 * cores hide every button from the game while Menu is held, and the FPGA does
 * not open the menu when another button was pressed with Menu. */
typedef struct {
    uint16_t Last;
} ColorTempHotkey_t;

/* Raw: button word from the FPGA (bit 4 Up, 7 Down, 8 Menu held, 9 menu
 * closed). Returns +1 warmer, -1 cooler, 0 nothing. */
int ColorTempHotkey_Update(ColorTempHotkey_t *State, uint16_t Raw);
