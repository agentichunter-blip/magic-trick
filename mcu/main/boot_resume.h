#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    esp_err_t Result;
    bool Present;
    /* A dedicated Magic Trick cart (header title "MAGIC TRICK", written by
     * tools/mtcart.py): its single game boots instead of the cart's own
     * explanation program. */
    bool MagicTrick;
} BootResumeCartridge_t;

/* Probe once as soon as UART/QSPI are ready, before UI setup and startup waits.
 * The existing maintenance command resets the core, even for a read-only probe. */
BootResumeCartridge_t BootResume_CheckCartridge(void);
/* A plain cartridge found by the check stays held (not running) until this is
 * called, right after the intro, so it boots once the intro is over. */
void BootResume_ReleaseCartridge(void);
/* Use the early result after UI, console and power task are ready. An unknown
 * result falls back to the menu rather than probing a running cartridge late. */
void BootResume_Run(const BootResumeCartridge_t *Cartridge);
/* Power-on intro. Call after LVGL/display setup but before the LVGL timer
 * task starts drawing. Present() must send Frame to the OSD and return once
 * the transfer is complete. Skips after a core switch, in PC mode, or while
 * a button is held; any button press ends it early.
 * Returns true when it played (the caller may skip its startup delay). */
#include <stdint.h>
bool BootResume_PlayIntro(const BootResumeCartridge_t *Cartridge,
                          uint16_t *Frame, void (*Present)(void));
