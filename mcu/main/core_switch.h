#pragma once
#include "esp_err.h"
#include "last_game.h"
#include <stdbool.h>
#include <stdint.h>

/* Only the explicitly qualified multiboot protocol advertises major 50. */
void CoreSwitch_ObserveVersion(uint8_t Major, uint8_t Minor);
bool CoreSwitch_IsSupported(void);
bool CoreSwitch_IsTarget(bool GameGear);
/* Caller must have saved active battery data, stopped the core, closed files,
 * released bulk transport and unmounted SD. Success resets the device. */
esp_err_t CoreSwitch_Request(const char *Path);
/* Consume before launching, including failure paths, to prevent reboot loops. */
esp_err_t CoreSwitch_TakePending(char Path[LAST_GAME_PATH_SIZE]);
/* True when a switch request is stored (does not consume it). */
bool CoreSwitch_HasPending(void);
