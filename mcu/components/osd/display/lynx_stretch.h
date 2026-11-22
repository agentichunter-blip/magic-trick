#pragma once

/* Lynx and Master System: letterbox the picture (Original; Lynx 160x102,
 * SMS 160x120) or fill the
 * 160x144 panel (Stretch). Sent to the FPGA as system control bit 15. */

#include "osd_shared.h"
#include "settings.h"

typedef enum LynxStretchState
{
    kLynxStretchState_Off,
    kLynxStretchState_On,
    kNumLynxStretchState,
} LynxStretchState_t;

OSD_Result_t LynxStretch_Draw(void* arg);
void LynxStretch_Update(const LynxStretchState_t eNewState);
OSD_Result_t LynxStretch_OnButton(const Button_t Button, const ButtonState_t State, void *arg);
OSD_Result_t LynxStretch_OnTransition(void* arg);
LynxStretchState_t LynxStretch_GetState(void);
OSD_Result_t LynxStretch_ApplySetting(SettingValue_t const *const pValue);
void LynxStretch_RegisterOnUpdateCb(fnOnUpdateCb_t fnOnUpdate);
