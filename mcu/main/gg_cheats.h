#pragma once
#include "cheats.h"
/* Game Genie codes for the Game Gear or NES core's CODES engine (gg_cheat_loader.sv).
 * Core must be stopped/prepared. Clears old codes even when the plan is empty. */
esp_err_t GGCheats_Install(const CheatPlan_t *Plan);
