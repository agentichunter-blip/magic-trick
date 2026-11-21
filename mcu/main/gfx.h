#pragma once

#include "lvgl.h"

void Gfx_Start(lv_obj_t *const pScreen);
/* Full-screen "LOADING GAME" card for a start-up that continues a core switch.
 * Call before the LVGL task starts; Gfx_HideCard may be called from any task
 * and returns the menu screen on the next LVGL tick. */
/* pArt: the game's box art (BOX_ART_BYTES of RGB565, malloc'd; the card owns
 * and frees it), or NULL. */
void Gfx_ShowCard(const char *pTitle, uint8_t *pArt);
void Gfx_HideCard(void);
