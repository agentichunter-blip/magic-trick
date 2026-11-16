#pragma once
#include <stddef.h>
enum {MALLOC_CAP_8BIT, MALLOC_CAP_DMA};
/* Host stand-ins: the Wi-Fi status and diagnostics report memory figures. */
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 65536; }
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 65536; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 32768; }
