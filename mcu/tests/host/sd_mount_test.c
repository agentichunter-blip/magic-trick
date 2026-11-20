#include <assert.h>
#include <stdint.h>
#define CART_STORE_TEST_HAS_GAMES
#define SDCARD_STORE_MARKER "sd_mount_test.marker"
#include <stdbool.h>
bool CartStore_HasGames(void);
#include "../../main/sd_card.c"

/* The internal store (no card, cartridge with Magic Trick games). */
static bool HasGames, StoreMounts;
static int StoreMountCount, StoreUnmountCount, Registers;
static esp_err_t StoreResult;
bool CartStore_HasGames(void) { return HasGames; }
esp_flash_t *esp_flash_default_chip;
static esp_partition_t TestPartition;
esp_err_t esp_partition_register_external(esp_flash_t *chip, size_t offset, size_t size, const char *label,
    esp_partition_type_t type, esp_partition_subtype_t subtype, const esp_partition_t **out) {
    assert(offset == 0x110000 && offset + size == 0x25F000 && !strcmp(label, "mtstore"));
    ++Registers; *out = &TestPartition; return ESP_OK;
}
/* A fresh store has no file system: it is formatted once (aligned, with
 * the marker), then mounts as is. */
static bool Formatted, WlMounted;
static int Formats;
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *path, const char *label, const esp_vfs_fat_mount_config_t *c, wl_handle_t *h) {
    assert(!strcmp(path, "/sdcard") && !strcmp(label, "mtstore") && !c->format_if_mount_failed && !WlMounted);
    if (StoreResult != ESP_OK) return StoreResult;
    if (!Formatted) return ESP_FAIL;
    assert(!StoreMounts); StoreMounts = true; ++StoreMountCount; *h = 7; return ESP_OK;
}
esp_err_t esp_vfs_fat_spiflash_unmount_rw_wl(const char *path, wl_handle_t h) {
    assert(StoreMounts && h == 7); StoreMounts = false; ++StoreUnmountCount; return ESP_OK;
}
esp_err_t esp_vfs_fat_info(const char *p, uint64_t *t, uint64_t *f) { *t = *f = 0; return ESP_OK; }
BYTE ff_diskio_get_pdrv_wl(wl_handle_t h) { return 1; }
int wl_mount(const esp_partition_t *p, wl_handle_t *h) { assert(p == &TestPartition && !StoreMounts && !WlMounted); WlMounted = true; *h = 9; return ESP_OK; }
int wl_unmount(wl_handle_t h) { assert(h == 9 && WlMounted); WlMounted = false; return ESP_OK; }
static int DiskRegistered = -1;
esp_err_t ff_diskio_get_drive(BYTE *d) { *d = 2; return ESP_OK; }
esp_err_t ff_diskio_register_wl_partition(BYTE d, wl_handle_t h) { assert(d == 2 && h == 9); DiskRegistered = d; return ESP_OK; }
void ff_diskio_register(BYTE d, const void *impl) { assert(d == 2 && impl == NULL); DiskRegistered = -1; }
FRESULT f_mkfs(const char *path, const MKFS_PARM *o, void *work, UINT len) {
    /* One FAT, 4 KiB clusters, data on 4 KiB erase-block boundaries. */
    assert(!strcmp(path, "2:") && DiskRegistered == 2 && work && len >= 4096);
    assert(o->n_fat == 1 && o->align == 8 && o->au_size == 4096);
    Formatted = true; ++Formats; return FR_OK;
}
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }

static esp_err_t MountResult, CommandResult, CommandError;
static bool Responds, MissingResponseFlag;
static sdmmc_card_t Card;
static TickType_t LastWait;
static bool PreviewWinsRace, ReleasePreviewWhenAsked;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p) { return p; }
int xSemaphoreTake(SemaphoreHandle_t p, TickType_t wait) {
    LastWait = wait;
    if (PreviewWinsRace && wait == pdMS_TO_TICKS(2000)) {
        /* A preview acquires the mutex after the foreground's initial check. */
        PreviewWinsRace = false;
        p->held = 1; PreviewOwnsGate = true;
        assert(!PreviewYieldRequested);
        return 0;
    }
    if (ReleasePreviewWhenAsked && wait == pdMS_TO_TICKS(4750) && SDCard_PreviewShouldYield()) {
        ReleasePreviewWhenAsked = false;
        assert(SDCard_Unmount(&Card) == ESP_OK);
    }
    if (p->held) return 0;
    p->held = 1; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t p) { assert(p->held); p->held = 0; return pdTRUE; }
esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t *cmd) {
    cmd->error = CommandError;
    return CommandResult;
}
esp_err_t esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host,
    const sdmmc_slot_config_t *slot, const esp_vfs_fat_sdmmc_mount_config_t *config,
    sdmmc_card_t **out) {
    assert(!config->format_if_mount_failed && slot->width == 1);
    sdmmc_command_t cmd = {.opcode = 8, .flags = MissingResponseFlag ? 0 : SCF_RSP_PRESENT};
    if (Responds) host->do_transaction(1, &cmd);
    *out = MountResult == ESP_OK ? &Card : NULL;
    return MountResult;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card) { assert(card == &Card); return ESP_OK; }
BYTE ff_diskio_get_pdrv_card(const sdmmc_card_t *card) { (void)card; return 0; }
void SDList_SetDrive(int Drive) { (void)Drive; }
void sdmmc_card_print_info(FILE *f, const sdmmc_card_t *card) {}
int esp_console_cmd_register(const esp_console_cmd_t *c) { return ESP_OK; }
int main(void) {
    (void)remove("sd_mount_test.marker");
    sdmmc_card_t *card;
    MountResult = ESP_ERR_TIMEOUT;
    assert(SDCard_Mount(&card) == ESP_ERR_NOT_FOUND && card == NULL);
    Responds = true;
    assert(SDCard_Mount(&card) == ESP_ERR_TIMEOUT && CardResponded);
    CommandError = ESP_ERR_TIMEOUT;
    assert(SDCard_Mount(&card) == ESP_ERR_NOT_FOUND && !CardResponded);
    CommandError = ESP_OK; MissingResponseFlag = true;
    assert(SDCard_Mount(&card) == ESP_ERR_NOT_FOUND);
    MissingResponseFlag = false;
    MountResult = ESP_FAIL;
    assert(SDCard_Mount(&card) == ESP_FAIL);
    MountResult = ESP_OK;
    assert(SDCard_Mount(&card) == ESP_OK && card == &Card);
    sdmmc_card_t *other;
    assert(SDCard_Mount(&other) == ESP_ERR_INVALID_STATE);
    assert(SDCard_Unmount(card) == ESP_OK);
    assert(SDCard_Mount(&card) == ESP_OK);
    assert(SDCard_Unmount(card) == ESP_OK);
    assert(SDCard_MountPreview(&card) == ESP_OK && LastWait == 0);
    assert(!SDCard_PreviewShouldYield());
    assert(SDCard_MountPreview(&other) == ESP_ERR_INVALID_STATE && LastWait == 0);
    assert(SDCard_Mount(&other) == ESP_ERR_INVALID_STATE && LastWait == 4750);
    assert(SDCard_PreviewShouldYield());
    assert(SDCard_Unmount(card) == ESP_OK);
    assert(SDCard_Mount(&card) == ESP_OK && LastWait == 2000);
    assert(!SDCard_PreviewShouldYield());
    assert(SDCard_MountPreview(&other) == ESP_ERR_INVALID_STATE && LastWait == 0);
    assert(SDCard_Unmount(card) == ESP_OK);
    PreviewWinsRace = ReleasePreviewWhenAsked = true;
    assert(SDCard_Mount(&card) == ESP_OK && card == &Card && LastWait == 4750);
    assert(!PreviewOwnsGate && !SDCard_PreviewShouldYield() && !ReleasePreviewWhenAsked);
    assert(SDCard_Unmount(card) == ESP_OK);

    /* No card: the internal store only with Magic Trick games on the cartridge,
     * never for previews or bulk transfers, and only when no card answers. */
    Responds = false; MountResult = ESP_ERR_TIMEOUT;
    assert(SDCard_Mount(&card) == ESP_ERR_NOT_FOUND && !StoreMounts);
    HasGames = true;
    assert(SDCard_MountPhysical(&card) == ESP_ERR_NOT_FOUND && !StoreMounts);
    assert(SDCard_MountPreview(&card) == ESP_ERR_NOT_FOUND && !StoreMounts);
    assert(SDCard_Mount(&card) == ESP_OK && card == &StoreCard && StoreMounts && SDCard_IsVirtual());
    assert(SDCard_Mount(&other) == ESP_ERR_INVALID_STATE); /* gate still held */
    assert(SDCard_Unmount(card) == ESP_OK && !StoreMounts && !SDCard_IsVirtual());
    assert(SDCard_Mount(&card) == ESP_OK && SDCard_IsVirtual() && Registers == 1);
    assert(SDCard_Unmount(card) == ESP_OK);
    StoreResult = ESP_FAIL; /* store fails: the card error stands, gate released */
    assert(SDCard_Mount(&card) == ESP_ERR_NOT_FOUND && !SDCard_IsVirtual());
    StoreResult = ESP_OK;
    Responds = true; /* a card that answers but fails is never replaced */
    assert(SDCard_Mount(&card) == ESP_ERR_TIMEOUT && !StoreMounts);
    Responds = false; MountResult = ESP_OK; /* card back: the card wins */
    assert(SDCard_Mount(&card) == ESP_OK && card == &Card && !SDCard_IsVirtual());
    assert(SDCard_Unmount(card) == ESP_OK);
    /* The failing store above was formatted again too (a mount failure means no file system). */
    assert(StoreMountCount == 2 && StoreUnmountCount == 2 && Formats == 2 && DiskRegistered == -1 && !WlMounted);
    /* A store without the marker (older layout) is formatted again. */
    assert(remove("sd_mount_test.marker") == 0);
    Responds = false; MountResult = ESP_ERR_TIMEOUT;
    assert(SDCard_Mount(&card) == ESP_OK && SDCard_IsVirtual() && Formats == 3);
    assert(SDCard_Unmount(card) == ESP_OK);
    (void)remove("sd_mount_test.marker");
    return 0;
}
