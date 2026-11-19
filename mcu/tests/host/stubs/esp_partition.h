#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_flash.h"
typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_FAT = 0x81 } esp_partition_subtype_t;
typedef struct { size_t address, size; } esp_partition_t;
esp_err_t esp_partition_register_external(esp_flash_t *, size_t, size_t, const char *, esp_partition_type_t,
                                          esp_partition_subtype_t, const esp_partition_t **);
