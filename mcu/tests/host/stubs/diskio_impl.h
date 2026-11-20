#pragma once
#include "esp_err.h"
#include "diskio_wl.h"
esp_err_t ff_diskio_get_drive(BYTE *out_pdrv);
void ff_diskio_register(BYTE pdrv, const void *discio_impl);
esp_err_t ff_diskio_register_wl_partition(BYTE pdrv, wl_handle_t flash_handle);
