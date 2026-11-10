#pragma once
#include <stddef.h>
enum {MALLOC_CAP_8BIT, MALLOC_CAP_DMA};
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
