#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "sdmmc_cmd.h"

#ifndef SDCARD_MOUNT_POINT
#define SDCARD_MOUNT_POINT "/sdcard"
#endif

esp_err_t SDCard_RunReadWriteTest(void);

esp_err_t SDCard_Mount(sdmmc_card_t **ppCard);
/* The microSD card only, never the internal store (for features that copy
 * games or backups in bulk: USB/Wi-Fi transfers, cart backup). */
esp_err_t SDCard_MountPhysical(sdmmc_card_t **ppCard);
/* True while SDCard_Mount has the internal store at /sdcard instead of a
 * card: with no microSD card and a cartridge carrying Magic Trick games,
 * saves and states for those games live in ~1.3 MB of the MCU flash
 * (0x110000-0x25F000, free in every layout the installer writes). */
bool SDCard_IsVirtual(void);
/* Best-effort preview mount; never waits for a foreground storage owner. */
esp_err_t SDCard_MountPreview(sdmmc_card_t **ppCard);

esp_err_t SDCard_ProbeReadWrite(void);

esp_err_t SDCard_Unmount(sdmmc_card_t *pCard);

esp_err_t SDCard_RegisterConsoleCommand(void);

bool SDCard_PreviewShouldYield(void);
