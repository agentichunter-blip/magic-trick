#pragma once
#include <stddef.h>
#include <stdint.h>
// Create an exclusive, complete, synced backup copy of a battery save
// ("<canonical>.before-load-NNNN.sav") before a save-state load replaces its RAM.
// Returns 0 on success. Never modifies either source or canonical file.
int StateSaveBranch_Create(const char *canonical, const char *source,
                           size_t expected_size, char *branch, size_t capacity);
