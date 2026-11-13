#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t NativeStateControl_Init(void);
uint16_t NativeStateControl_Buttons(uint16_t Raw);
/* Console diagnostics: run the Menu+Select (1) or Menu+Start (2) shortcut as if
 * its buttons were pressed; the controller logs each step. */
void NativeStateControl_DebugShortcut(unsigned Event);
