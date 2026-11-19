#include "sd_card.h"

#include "driver/sdmmc_host.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "diskio_wl.h"
#include "cart_store.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "sd_list.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

enum {
    kMaxOpenFiles = 2,
    kAllocationUnitSize = 16 * 1024,
    /* The internal store: MCU flash no partition uses (the stock layout's
     * application ends at 0x110000; the installer's core record sits at
     * 0x25F000; the Magic Trick application starts at 0x260000). */
    kStoreOffset = 0x110000,
    kStoreSize = 0x25F000 - 0x110000,
};

static StaticSemaphore_t MountGateStorage;
static SemaphoreHandle_t MountGate;
static bool PreviewOwnsGate, PreviewYieldRequested;
static portMUX_TYPE GateInitLock = portMUX_INITIALIZER_UNLOCKED;

static const char *const TAG = "SDCard";
static const char *const MountPoint = SDCARD_MOUNT_POINT;
static const char *const ProbePath = "/sdcard/CHRMTEST.TMP";
static const uint8_t ProbeData[] = {'C', 'H', 'R', 'M'};

static const char *const kStoreLabel = "mtstore";
static const esp_partition_t *StorePartition;
static wl_handle_t StoreHandle = WL_INVALID_HANDLE;
static sdmmc_card_t StoreCard; /* stands for the store in SDCard_Unmount */
static bool StoreMounted;

static bool CardResponded;
static int LastCommand;
static esp_err_t LastCommandError;

static esp_err_t MountTransaction(int Slot, sdmmc_command_t *Command)
{
    const esp_err_t Result = sdmmc_host_do_transaction(Slot, Command);
    LastCommand = Command->opcode;
    LastCommandError = Result == ESP_OK ? Command->error : Result;
    if (LastCommandError == ESP_OK && (Command->flags & SCF_RSP_PRESENT))
        CardResponded = true;
    return Result;
}

/* Mounts the internal store at /sdcard (formatting it the first time) with
 * the folders cartridge saves expect. Called with the mount gate held. */
static esp_err_t MountStore(void)
{
    esp_err_t Result = ESP_OK;
    if (!StorePartition)
    {
        Result = esp_partition_register_external(esp_flash_default_chip, kStoreOffset, kStoreSize, kStoreLabel,
                                                 ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                 &StorePartition);
        if (Result != ESP_OK)
        {
            printf("SDCARD_STORE register=%s\n", esp_err_to_name(Result));
            return Result;
        }
    }
    const esp_vfs_fat_mount_config_t Config = {
        .format_if_mount_failed = true,
        .max_files = kMaxOpenFiles,
        .allocation_unit_size = 4096,
    };
    Result = esp_vfs_fat_spiflash_mount_rw_wl(MountPoint, kStoreLabel, &Config, &StoreHandle);
    if (Result != ESP_OK)
    {
        printf("SDCARD_STORE mount=%s\n", esp_err_to_name(Result));
        StoreHandle = WL_INVALID_HANDLE;
        return Result;
    }
    StoreMounted = true;
    SDList_SetDrive(ff_diskio_get_pdrv_wl(StoreHandle));
    static const char *const kFolders[] = {"/sdcard/CHROMAGIC", CART_STORE_SIDECAR_ROOT};
    for (size_t i = 0; i < sizeof(kFolders) / sizeof(kFolders[0]); ++i)
        if (mkdir(kFolders[i], 0775) != 0 && errno != EEXIST)
            printf("SDCARD_STORE mkdir=%s errno=%d\n", kFolders[i], errno);
    return ESP_OK;
}

bool SDCard_IsVirtual(void) { return StoreMounted; }

static esp_err_t Mount(sdmmc_card_t **const ppCard, bool Preview, bool AllowStore)
{
    if (ppCard == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&GateInitLock);
    if (MountGate == NULL) MountGate = xSemaphoreCreateMutexStatic(&MountGateStorage);
    portEXIT_CRITICAL(&GateInitLock);
    if (MountGate == NULL) return ESP_ERR_NO_MEM;
    *ppCard = NULL;
    portENTER_CRITICAL(&GateInitLock);
    if (!Preview && PreviewOwnsGate) PreviewYieldRequested = true;
    portEXIT_CRITICAL(&GateInitLock);
    /* A foreground mount waits out a background battery save (a Game Gear
     * copy holds the card for a few hundred ms) instead of failing. */
    if (xSemaphoreTake(MountGate, Preview ? 0 : pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        portENTER_CRITICAL(&GateInitLock);
        const bool WaitForPreview = !Preview && PreviewOwnsGate;
        /* The preview may have acquired the gate after our first check. */
        if (WaitForPreview) PreviewYieldRequested = true;
        portEXIT_CRITICAL(&GateInitLock);
        /* A save/backup must be allowed to drain the small in-flight preview.
         * Foreground-to-foreground contention retains the original timeout. */
        if (!WaitForPreview || xSemaphoreTake(MountGate, pdMS_TO_TICKS(4750)) != pdTRUE)
            return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&GateInitLock);
    PreviewOwnsGate = Preview;
    PreviewYieldRequested = false;
    portEXIT_CRITICAL(&GateInitLock);
    sdmmc_host_t Host = SDMMC_HOST_DEFAULT();
    Host.do_transaction = MountTransaction;
    CardResponded = false;
    LastCommand = -1;
    LastCommandError = ESP_OK;
    sdmmc_slot_config_t Slot = SDMMC_SLOT_CONFIG_DEFAULT();
    Slot.width = 1;
    Slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t MountConfig = {
        .format_if_mount_failed = false,
        .max_files = kMaxOpenFiles,
        .allocation_unit_size = kAllocationUnitSize,
    };

    esp_err_t Result = esp_vfs_fat_sdmmc_mount(MountPoint, &Host, &Slot, &MountConfig, ppCard);
    if (Result != ESP_OK)
    {
        printf("SDCARD_MOUNT error=%s responded=%u command=%d command_error=%s\n",
               esp_err_to_name(Result), CardResponded ? 1 : 0, LastCommand,
               esp_err_to_name(LastCommandError));
        if (Result == ESP_ERR_TIMEOUT && !CardResponded)
            Result = ESP_ERR_NOT_FOUND;
        /* No card at all, and the cartridge carries Magic Trick games: their
         * saves go to the internal store instead (never for previews). */
        if (Result == ESP_ERR_NOT_FOUND && AllowStore && !Preview && CartStore_HasGames() &&
            MountStore() == ESP_OK)
        {
            *ppCard = &StoreCard;
            return ESP_OK;
        }
        portENTER_CRITICAL(&GateInitLock);
        PreviewOwnsGate = false;
        portEXIT_CRITICAL(&GateInitLock);
        xSemaphoreGive(MountGate);
    }
    else SDList_SetDrive(ff_diskio_get_pdrv_card(*ppCard));
    return Result;
}

esp_err_t SDCard_Mount(sdmmc_card_t **ppCard) { return Mount(ppCard, false, true); }
esp_err_t SDCard_MountPhysical(sdmmc_card_t **ppCard) { return Mount(ppCard, false, false); }
esp_err_t SDCard_MountPreview(sdmmc_card_t **ppCard) { return Mount(ppCard, true, false); }

esp_err_t SDCard_ProbeReadWrite(void)
{
    esp_err_t Result = ESP_FAIL;
    bool CreatedProbe = false;
    FILE *pFile = NULL;

    struct stat ExistingFile;
    if (stat(ProbePath, &ExistingFile) == 0)
    {
        ESP_LOGE(TAG, "Refusing to replace existing probe path %s", ProbePath);
        printf("SDTEST_STAGE=probe_exists\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (errno != ENOENT)
    {
        ESP_LOGE(TAG, "Cannot inspect probe path: errno=%d", errno);
        printf("SDTEST_STAGE=stat errno=%d\r\n", errno);
        return ESP_FAIL;
    }

    pFile = fopen(ProbePath, "wb");
    if (pFile == NULL)
    {
        ESP_LOGE(TAG, "Probe create failed: errno=%d", errno);
        printf("SDTEST_STAGE=create errno=%d\r\n", errno);
        return ESP_FAIL;
    }
    CreatedProbe = true;

    if (fwrite(ProbeData, sizeof(ProbeData), 1, pFile) != 1)
    {
        ESP_LOGE(TAG, "Probe write failed: errno=%d", errno);
        printf("SDTEST_STAGE=write errno=%d\r\n", errno);
        goto Cleanup;
    }

    if (fflush(pFile) != 0)
    {
        ESP_LOGE(TAG, "Probe flush failed: errno=%d", errno);
        printf("SDTEST_STAGE=flush errno=%d\r\n", errno);
        goto Cleanup;
    }

    if (fsync(fileno(pFile)) != 0)
    {
        ESP_LOGE(TAG, "Probe sync failed: errno=%d", errno);
        printf("SDTEST_STAGE=sync errno=%d\r\n", errno);
        goto Cleanup;
    }

    if (fclose(pFile) != 0)
    {
        pFile = NULL;
        ESP_LOGE(TAG, "Probe close failed: errno=%d", errno);
        printf("SDTEST_STAGE=write_close errno=%d\r\n", errno);
        goto Cleanup;
    }
    pFile = NULL;

    pFile = fopen(ProbePath, "rb");
    if (pFile == NULL)
    {
        ESP_LOGE(TAG, "Probe reopen failed: errno=%d", errno);
        printf("SDTEST_STAGE=reopen errno=%d\r\n", errno);
        goto Cleanup;
    }

    uint8_t Readback[sizeof(ProbeData)] = {0};
    if ((fread(Readback, sizeof(Readback), 1, pFile) != 1) ||
        (memcmp(Readback, ProbeData, sizeof(ProbeData)) != 0) ||
        (fgetc(pFile) != EOF))
    {
        ESP_LOGE(TAG, "Probe readback did not match");
        printf("SDTEST_STAGE=readback errno=%d\r\n", errno);
        Result = ESP_ERR_INVALID_RESPONSE;
        goto Cleanup;
    }

    Result = ESP_OK;

Cleanup:
    if (pFile != NULL)
    {
        if ((fclose(pFile) != 0) && (Result == ESP_OK))
        {
            ESP_LOGE(TAG, "Probe close failed: errno=%d", errno);
            printf("SDTEST_STAGE=read_close errno=%d\r\n", errno);
            Result = ESP_FAIL;
        }
    }

    if (CreatedProbe && (unlink(ProbePath) != 0))
    {
        ESP_LOGE(TAG, "Probe cleanup failed: errno=%d", errno);
        printf("SDTEST_STAGE=cleanup errno=%d\r\n", errno);
        if (Result == ESP_OK)
        {
            Result = ESP_FAIL;
        }
    }

    return Result;
}

esp_err_t SDCard_RunReadWriteTest(void)
{
    sdmmc_card_t *pCard = NULL;
    esp_err_t Result = SDCard_Mount(&pCard);
    if (Result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(Result));
        return Result;
    }

    ESP_LOGI(TAG, "Card mounted in conservative 1-bit mode");
    sdmmc_card_print_info(stdout, pCard);

    Result = SDCard_ProbeReadWrite();
    printf("SDTEST_PROBE=%s (%s)\r\n",
           (Result == ESP_OK) ? "PASS" : "FAIL", esp_err_to_name(Result));
    const esp_err_t UnmountResult = SDCard_Unmount(pCard);
    printf("SDTEST_UNMOUNT=%s (%s)\r\n",
           (UnmountResult == ESP_OK) ? "PASS" : "FAIL", esp_err_to_name(UnmountResult));
    if (UnmountResult != ESP_OK)
    {
        ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(UnmountResult));
        if (Result == ESP_OK)
        {
            Result = UnmountResult;
        }
    }

    if (Result == ESP_OK)
    {
        ESP_LOGI(TAG, "SD read/write/sync test passed; card unmounted");
    }

    return Result;
}

esp_err_t SDCard_Unmount(sdmmc_card_t *const pCard)
{
    if (pCard == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    SDList_SetDrive(-1);
    esp_err_t Result;
    if (pCard == &StoreCard)
    {
        Result = esp_vfs_fat_spiflash_unmount_rw_wl(MountPoint, StoreHandle);
        StoreHandle = WL_INVALID_HANDLE;
        StoreMounted = false;
    }
    else Result = esp_vfs_fat_sdcard_unmount(MountPoint, pCard);
    portENTER_CRITICAL(&GateInitLock);
    PreviewOwnsGate = false;
    portEXIT_CRITICAL(&GateInitLock);
    xSemaphoreGive(MountGate);
    return Result;
}

static int SDTestCommand(const int argc, char **const argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t Result = SDCard_RunReadWriteTest();
    printf("{\"sdtest\":\"%s\",\"error\":\"%s\"}\r\n",
           (Result == ESP_OK) ? "pass" : "fail", esp_err_to_name(Result));
    return (Result == ESP_OK) ? 0 : 1;
}

/* storeinfo: mount what SDCard_Mount would (card or internal store), print
 * which, its free space and the cartridge save folder. */
static int StoreInfoCommand(const int argc, char **const argv)
{
    (void)argc; (void)argv;
    sdmmc_card_t *pCard = NULL;
    const esp_err_t Result = SDCard_Mount(&pCard);
    if (Result != ESP_OK) { printf("STOREINFO FAIL %s\n", esp_err_to_name(Result)); return 1; }
    uint64_t Total = 0, Free = 0;
    (void)esp_vfs_fat_info(MountPoint, &Total, &Free);
    printf("STOREINFO kind=%s total=%llu free=%llu\n", StoreMounted ? "internal" : "sd",
           (unsigned long long)Total, (unsigned long long)Free);
    DIR *pDir = opendir(CART_STORE_SIDECAR_ROOT);
    for (struct dirent *pEntry; pDir && (pEntry = readdir(pDir)) != NULL;)
    {
        char Path[300];
        struct stat Stat;
        snprintf(Path, sizeof(Path), "%s/%s", CART_STORE_SIDECAR_ROOT, pEntry->d_name);
        printf("STOREINFO file=\"%s\" bytes=%ld\n", pEntry->d_name, stat(Path, &Stat) == 0 ? (long)Stat.st_size : -1L);
    }
    if (pDir) closedir(pDir);
    (void)SDCard_Unmount(pCard);
    return 0;
}

esp_err_t SDCard_RegisterConsoleCommand(void)
{
    const esp_console_cmd_t Info = {
        .command = "storeinfo",
        .help = "Show the save storage in use (microSD card or internal store)",
        .func = StoreInfoCommand,
    };
    const esp_err_t InfoResult = esp_console_cmd_register(&Info);
    if (InfoResult != ESP_OK) return InfoResult;
    const esp_console_cmd_t Command = {
        .command = "sdtest",
        .help = "Safely test the internal microSD card, then unmount it",
        .hint = NULL,
        .func = SDTestCommand,
        .argtable = NULL,
    };

    return esp_console_cmd_register(&Command);
}

bool SDCard_PreviewShouldYield(void) {
    portENTER_CRITICAL(&GateInitLock);
    const bool Yield = PreviewYieldRequested;
    portEXIT_CRITICAL(&GateInitLock);
    return Yield;
}
