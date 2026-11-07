#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    esp_err_t Result;
    bool Present;
} BootResumeCartridge_t;

/* Probe once as soon as UART/QSPI are ready, before UI setup and startup waits.
 * The existing maintenance command resets the core, even for a read-only probe. */
BootResumeCartridge_t BootResume_CheckCartridge(void);
/* Use the early result after UI, console and power task are ready. An unknown
 * result falls back to the menu rather than probing a running cartridge late. */
void BootResume_Run(const BootResumeCartridge_t *Cartridge);
