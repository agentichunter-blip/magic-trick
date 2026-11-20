#pragma once

#include "lvgl.h"

void Gfx_Start(lv_obj_t *const pScreen);
/* Full-screen "LOADING GAME" card for a start-up that continues a core switch.
 * Call before the LVGL task starts; Gfx_HideCard may be called from any task
 * and returns the menu screen on the next LVGL tick. */
void Gfx_ShowCard(const char *pTitle);
void Gfx_HideCard(void);
