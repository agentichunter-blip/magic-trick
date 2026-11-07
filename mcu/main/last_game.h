#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

enum { LAST_GAME_PATH_SIZE = 320 };
bool LastGame_ValidPath(const char *Path);
esp_err_t LastGame_Read(char Path[LAST_GAME_PATH_SIZE]);
/* Call only after a successful load and cleanup. Does not change ROM/save files. */
esp_err_t LastGame_Remember(const char *Path);
