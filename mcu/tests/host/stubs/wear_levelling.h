#pragma once
#include <stdint.h>
#include "esp_partition.h"
typedef int32_t wl_handle_t;
#define WL_INVALID_HANDLE -1
int wl_mount(const esp_partition_t *partition, wl_handle_t *out_handle);
int wl_unmount(wl_handle_t handle);
