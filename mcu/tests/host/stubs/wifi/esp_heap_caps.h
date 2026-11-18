#pragma once
#include <stddef.h>
enum {MALLOC_CAP_8BIT = 1, MALLOC_CAP_DMA = 2, MALLOC_CAP_EXEC = 4, MALLOC_CAP_32BIT = 8};
/* Host stand-ins: the Wi-Fi status and diagnostics report memory figures. */
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 65536; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 65536; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 32768; }
