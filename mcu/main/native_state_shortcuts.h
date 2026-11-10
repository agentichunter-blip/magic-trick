#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool Tracking, Fired, Swallow; uint16_t Chord; uint32_t Start, Last; } NativeShortcutState;
// Events are emitted only once all shortcut buttons are released: 1 save, 2 load.
unsigned NativeShortcut_Update(NativeShortcutState *S,uint16_t Raw,uint32_t Now,bool Enabled,bool *Consume);
