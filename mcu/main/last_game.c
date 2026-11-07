#include "last_game.h"
#include "rom_title.h"
#include "nvs.h"
#include <string.h>
#include <strings.h>

static const char Root[] = "/sdcard/CHROMAGIC/BACKUPS/";
static const char Namespace[] = "last_game";
static const char Key[] = "path";

bool LastGame_ValidPath(const char *Path)
{
    if (!Path) return false;
    const size_t Length = strnlen(Path, LAST_GAME_PATH_SIZE);
    if (Length >= LAST_GAME_PATH_SIZE || Length <= sizeof(Root)-1 ||
        strncmp(Path, Root, sizeof(Root)-1)) return false;
    if (RomTitle_IsDemo(Path)) return false;
    for (const char *Part = Path + sizeof(Root)-1; *Part; )
    {
        const size_t Size = strcspn(Part, "/");
        if (!Size || (Size == 1 && Part[0] == '.') ||
            (Size == 2 && Part[0] == '.' && Part[1] == '.')) return false;
        Part += Size;
        if (*Part) ++Part;
    }
    const char *Extension = strrchr(Path, '.');
    return Extension && (!strcasecmp(Extension, ".gb") || !strcasecmp(Extension, ".gbc"));
}

esp_err_t LastGame_Read(char Path[LAST_GAME_PATH_SIZE])
{
    if (!Path) return ESP_ERR_INVALID_ARG;
    Path[0] = '\0';
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READONLY, &Handle);
    if (Result != ESP_OK) return Result;
    size_t Size = LAST_GAME_PATH_SIZE;
    Result = nvs_get_str(Handle, Key, Path, &Size);
    nvs_close(Handle);
    if (Result == ESP_OK && !LastGame_ValidPath(Path)) Result = ESP_ERR_INVALID_ARG;
    if (Result != ESP_OK) Path[0] = '\0';
    return Result;
}

esp_err_t LastGame_Remember(const char *Path)
{
    if (!LastGame_ValidPath(Path)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READWRITE, &Handle);
    if (Result != ESP_OK) return Result;
    char Previous[LAST_GAME_PATH_SIZE];
    size_t Size = sizeof(Previous);
    const esp_err_t ReadResult = nvs_get_str(Handle, Key, Previous, &Size);
    /* Replaying the same game does not wear flash with an identical write. */
    if (ReadResult != ESP_OK || strcmp(Previous, Path))
    {
        Result = nvs_set_str(Handle, Key, Path);
        if (Result == ESP_OK) Result = nvs_commit(Handle);
    }
    nvs_close(Handle);
    return Result;
}
