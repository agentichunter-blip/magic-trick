#pragma once
#include "esp_err.h"
#include <stddef.h>

enum { RECENT_GAMES_LIMIT = 20, RECENT_GAME_PATH_SIZE = 320 };
typedef enum { kRecentNone, kRecentGB, kRecentGBC } RecentSystem_t;
typedef struct {
    size_t Count;
    char Paths[RECENT_GAMES_LIMIT][RECENT_GAME_PATH_SIZE];
} RecentGames_t;

/* Paths refer to the existing files under BACKUPS/GB or BACKUPS/GBC. */
RecentSystem_t RecentGames_System(const char *Path);
esp_err_t RecentGames_Read(RecentSystem_t System, RecentGames_t *Games);
/* Call only after a successful launch and cleanup. No writes while browsing. */
esp_err_t RecentGames_Remember(const char *Path);
