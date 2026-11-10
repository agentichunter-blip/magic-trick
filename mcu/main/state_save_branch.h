#pragma once
#include <stddef.h>
#include <stdint.h>
// Create an exclusive, complete, synced copy before any RAM restoration.
// Returns 0 on success. Never modifies either source or canonical file.
int StateSaveBranch_Create(const char *canonical, const char *source,
                           size_t expected_size, char *branch, size_t capacity);
