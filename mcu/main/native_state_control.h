#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t NativeStateControl_Init(void);
uint16_t NativeStateControl_Buttons(uint16_t Raw);
