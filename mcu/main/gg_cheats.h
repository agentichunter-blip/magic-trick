#pragma once
#include "cheats.h"
/* Core must be stopped/prepared. Clears old codes even when the plan is empty. */
esp_err_t GGCheats_Install(const CheatPlan_t *Plan);
