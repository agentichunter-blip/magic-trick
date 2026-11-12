#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t VirtualCart_Start(const char *pPath);
esp_err_t VirtualCart_Stop(void);
bool VirtualCart_IsActive(void);
bool VirtualCart_IsBusy(void);

esp_err_t VirtualCart_RegisterConsoleCommands(void);

esp_err_t VirtualCart_StateCapture(bool load);

/* True when the last successful native save could not be written to the SD card. */
bool VirtualCart_LastStateSdFailed(void);
esp_err_t VirtualCart_NativeInfo(bool *Supported, bool *Valid, bool *Uncertain, uint32_t *Session);
void VirtualCart_RegisterStateWait(void (*Callback)(void));

esp_err_t VirtualCart_StateCaptureForSession(bool load, uint32_t Session);

/* Three save-state slots per game (1-3). Bit n-1 of ValidMask: slot n can be loaded. */
esp_err_t VirtualCart_NativeSlotInfo(bool *Supported, uint8_t *ValidMask, bool *Uncertain, uint32_t *Session);
esp_err_t VirtualCart_StateCaptureSlot(bool load, unsigned Slot, uint32_t Session);
