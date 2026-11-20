#pragma once

/* What a power-on without a cartridge does: resume the last SD game (the
 * default) or open the GAMES menu. Read once by boot_resume.c. */

#include <stdbool.h>
#include "osd_shared.h"
#include "settings.h"

typedef enum StartUpMode
{
    kStartUpMode_LastGame,
    kStartUpMode_GamesMenu,
    kNumStartUpModes,
} StartUpMode_t;

OSD_Result_t StartUp_Draw(void *arg);
OSD_Result_t StartUp_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t StartUp_OnTransition(void *arg);
StartUpMode_t StartUp_GetMode(void);
OSD_Result_t StartUp_ApplySetting(SettingValue_t const *const pValue);
