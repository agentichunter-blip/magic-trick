#include "sd_list.h"
#include "sd_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "ff.h"

static int Drive = -1;

void SDList_SetDrive(int NewDrive) { Drive = NewDrive; }

esp_err_t SDList_Folder(const char *Path, SDList_Visitor Visit, void *Context)
{
    const size_t Root = strlen(SDCARD_MOUNT_POINT);
    if (Path == NULL || Visit == NULL || strncmp(Path, SDCARD_MOUNT_POINT, Root) != 0 ||
        (Path[Root] != 0 && Path[Root] != '/')) return ESP_ERR_INVALID_ARG;
    if (Drive < 0) return ESP_ERR_INVALID_STATE;
    const char *Inside = Path[Root] ? Path + Root : "/";
    const size_t Size = strlen(Inside) + 8;
    char *FatPath = malloc(Size);
    FF_DIR *Dir = malloc(sizeof(*Dir));
    FILINFO *Info = malloc(sizeof(*Info));
    esp_err_t Result = ESP_ERR_NO_MEM;
    if (FatPath != NULL && Dir != NULL && Info != NULL)
    {
        snprintf(FatPath, Size, "%d:%s", Drive, Inside);
        FRESULT Fat = f_opendir(Dir, FatPath);
        if (Fat == FR_NO_PATH || Fat == FR_NO_FILE || Fat == FR_INVALID_NAME) Result = ESP_ERR_NOT_FOUND;
        else if (Fat != FR_OK) Result = ESP_FAIL;
        else
        {
            Result = ESP_OK;
            while ((Fat = f_readdir(Dir, Info)) == FR_OK && Info->fname[0] != 0)
            {
                if (strcmp(Info->fname, ".") == 0 || strcmp(Info->fname, "..") == 0) continue;
                const bool Directory = (Info->fattrib & AM_DIR) != 0;
                if (!Visit(Info->fname, Directory, Directory ? 0 : Info->fsize, Context)) break;
            }
            if (Fat != FR_OK) Result = ESP_FAIL;
            f_closedir(Dir);
        }
    }
    free(Info); free(Dir); free(FatPath);
    return Result;
}

#else   /* host tests: a real POSIX folder, where stat() is cheap */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

void SDList_SetDrive(int NewDrive) { (void)NewDrive; }

esp_err_t SDList_Folder(const char *Path, SDList_Visitor Visit, void *Context)
{
    if (Path == NULL || Visit == NULL) return ESP_ERR_INVALID_ARG;
    DIR *Dir = opendir(Path);
    if (Dir == NULL) return errno == ENOENT || errno == ENOTDIR ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    esp_err_t Result = ESP_OK;
    struct dirent *Entry;
    char Child[1024];
    while ((Entry = readdir(Dir)) != NULL)
    {
        if (strcmp(Entry->d_name, ".") == 0 || strcmp(Entry->d_name, "..") == 0) continue;
        struct stat Info;
        if (snprintf(Child, sizeof(Child), "%s/%s", Path, Entry->d_name) >= (int)sizeof(Child) ||
            stat(Child, &Info) != 0) continue;
        if (!Visit(Entry->d_name, S_ISDIR(Info.st_mode), S_ISDIR(Info.st_mode) ? 0 : (uint64_t)Info.st_size, Context))
            break;
    }
    closedir(Dir);
    return Result;
}
#endif
