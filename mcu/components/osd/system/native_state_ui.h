#pragma once
#include "osd_shared.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    kStateUIIdle, kStateUIWaiting, kStateUISaving, kStateUILoading,
    kStateUISaved, kStateUILoaded, kStateUIError
} NativeStatePhase;
enum { kNativeStateSlots = 3 };
typedef struct {
    /* Valid: the current slot can be loaded. ValidMask bit n-1: slot n can be loaded. */
    bool Supported, Valid, Shortcuts, Available;
    uint8_t ValidMask, Slot; /* Slot: 1-3, used by the shortcuts and the last action. */
    NativeStatePhase Phase;
    char Message[64];
} NativeStateView;
typedef void (*NativeStateRead)(NativeStateView *);
typedef bool (*NativeStateRequest)(bool Load, unsigned Slot);
typedef void (*NativeStateShortcuts)(bool Enabled);
void NativeStateUI_Register(NativeStateRead Read, NativeStateRequest Request, NativeStateShortcuts Shortcuts);
OSD_Result_t NativeStateUI_Draw(void *Screen);
OSD_Result_t NativeStateUI_OnButton(Button_t Button, ButtonState_t State, void *Arg);
OSD_Result_t NativeStateUI_OnTransition(void *Arg);
bool NativeStateUI_HandlesButton(Button_t Button);
bool NativeStateUI_IsOpen(void);
void NativeStateUI_Open(void);

void NativeStateUI_RequestOpen(void);
bool NativeStateUI_TakeOpenRequest(void);
