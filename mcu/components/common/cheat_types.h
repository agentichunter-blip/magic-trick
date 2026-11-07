#pragma once
#include <stdbool.h>
#include <stdint.h>
/* UI snapshots own their text; the worker never exposes pointers into SD data. */
#define CHEAT_ROWS 3
#define CHEAT_NAME_SIZE 96
#define CHEAT_MAX_ENTRIES 256
#define CHEAT_MAX_PATCHES 512
#define CHEAT_HEADER_SIZE 32
#define CHEAT_ENTRY_SIZE 104
#define CHEAT_PATCH_SIZE 8
#define CHEAT_SELECTION_BYTES (CHEAT_MAX_ENTRIES / 8)
typedef struct {
    uint16_t Count, Selected, First, Enabled;
    struct { char Name[CHEAT_NAME_SIZE]; bool Enabled; } Rows[CHEAT_ROWS];
} CheatView_t;
typedef enum { kCheatOpen, kCheatView, kCheatToggle, kCheatDisableAll } CheatAction_t;

/* Available pack and saved selections for the next launch, not live activation. */
typedef struct { bool Available, Enabled; } CheatBadge_t;
