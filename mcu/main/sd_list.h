#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Lists one SD card folder with each entry's type and size, in a single pass.
 *
 * readdir() drops the size FatFs already read from the directory entry, and a
 * stat() per name makes FatFs rescan the folder from its start, so listing that
 * way is quadratic: a 400-game folder took 25 s over USB and long enough over
 * Wi-Fi for the file server to drop off the network (2026-11-17). Reading the
 * entries with f_readdir() keeps it linear.
 *
 * Path is a VFS path under SDCARD_MOUNT_POINT. Visit returns false to stop early.
 * Returns ESP_ERR_NOT_FOUND for a missing folder. */
typedef bool (*SDList_Visitor)(const char *Name, bool Directory, uint64_t Size, void *Context);
esp_err_t SDList_Folder(const char *Path, SDList_Visitor Visit, void *Context);

/* sd_card.c reports the FatFs drive of the mounted card (-1 when unmounted). */
void SDList_SetDrive(int Drive);
