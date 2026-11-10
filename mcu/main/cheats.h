#pragma once
#include "cheat_types.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdio.h>
typedef struct { uint32_t Offset; uint8_t Original, Value; bool Compare; } CheatPatch_t;
typedef struct { CheatPatch_t *Patches; size_t Count; bool GameGear; } CheatPlan_t;
/* Menu functions run only on the serialized browser worker. */
esp_err_t Cheats_Menu(CheatAction_t Action, const char *RomPath, size_t Selected, CheatView_t *View);
void Cheats_Close(void);
/* Called with SD mounted, before disturbing the active game. Restores ROM seek. */
esp_err_t Cheats_Prepare(const char *RomPath, FILE *Rom, CheatPlan_t *Plan);
esp_err_t Cheats_Apply(const CheatPlan_t *Plan, uint32_t Offset, uint8_t *Data, size_t Size);
void Cheats_Free(CheatPlan_t *Plan);
uint32_t Cheats_CRC32(uint32_t CRC, const void *Data, size_t Size);

/* Lightweight badge lookup with SD already mounted; does not scan the ROM. */
esp_err_t Cheats_Badge(const char *RomPath, CheatBadge_t *Badge);
