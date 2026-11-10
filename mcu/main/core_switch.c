#include "core_switch.h"
#include "fpga_tx.h"
#include "nvs.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>

static atomic_uchar CoreIdentity;
static const char Namespace[] = "core_switch";
static const char Key[] = "path";

void CoreSwitch_ObserveVersion(uint8_t Major, uint8_t Minor)
{
    atomic_store(&CoreIdentity, Major == 50 && (Minor == 1 || Minor == 2) ? Minor : 0);
}
bool CoreSwitch_IsSupported(void) { return atomic_load(&CoreIdentity) != 0; }
bool CoreSwitch_IsTarget(bool GameGear) { return atomic_load(&CoreIdentity) == (GameGear ? 2 : 1); }
static bool IsGameGear(const char *Path)
{
    const char *Extension = strrchr(Path, '.');
    return Extension && !strcasecmp(Extension, ".gg");
}
static esp_err_t ClearPending(void)
{
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READWRITE, &Handle);
    if (Result != ESP_OK) return Result;
    Result = nvs_erase_key(Handle, Key);
    if (Result == ESP_ERR_NVS_NOT_FOUND) Result = ESP_OK;
    if (Result == ESP_OK) Result = nvs_commit(Handle);
    nvs_close(Handle);
    return Result;
}
bool CoreSwitch_HasPending(void)
{
    nvs_handle_t Handle;
    if (nvs_open(Namespace, NVS_READONLY, &Handle) != ESP_OK) return false;
    size_t Size = 0;
    const esp_err_t Result = nvs_get_str(Handle, Key, NULL, &Size);
    nvs_close(Handle);
    return Result == ESP_OK;
}
esp_err_t CoreSwitch_TakePending(char Path[LAST_GAME_PATH_SIZE])
{
    if (!Path) return ESP_ERR_INVALID_ARG;
    Path[0] = 0;
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READONLY, &Handle);
    if (Result != ESP_OK) return Result;
    size_t Size = LAST_GAME_PATH_SIZE;
    Result = nvs_get_str(Handle, Key, Path, &Size);
    nvs_close(Handle);
    if (Result == ESP_ERR_NVS_NOT_FOUND) return Result;
    /* Clear corrupt/oversized records too. A failed clear never authorizes launch. */
    const esp_err_t ClearResult = ClearPending();
    if (ClearResult != ESP_OK) Result = ClearResult;
    if (Result == ESP_OK && !LastGame_ValidPath(Path)) Result = ESP_ERR_INVALID_ARG;
    if (Result != ESP_OK) Path[0] = 0;
    return Result;
}
esp_err_t CoreSwitch_Request(const char *Path)
{
    if (!LastGame_ValidPath(Path)) return ESP_ERR_INVALID_ARG;
    if (!CoreSwitch_IsSupported()) return ESP_ERR_NOT_SUPPORTED;
    const bool GameGear = IsGameGear(Path);
    if (CoreSwitch_IsTarget(GameGear)) return ESP_ERR_INVALID_STATE;
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READWRITE, &Handle);
    if (Result != ESP_OK) return Result;
    Result = nvs_set_str(Handle, Key, Path);
    if (Result == ESP_OK) Result = nvs_commit(Handle);
    nvs_close(Handle);
    if (Result != ESP_OK) {
        (void)ClearPending();
        return Result;
    }
    const uint16_t Nonce = (uint16_t)esp_random();
    printf("CORE_SWITCH target=%s pending=committed\n", GameGear ? "GG" : "GB_GBC");
    Result = FPGA_Tx_CoreReload(0xa1, GameGear, Nonce);
    if (Result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        Result = FPGA_Tx_CoreReload(0xa2, GameGear, Nonce);
    }
    if (Result == ESP_OK) {
        /* Real reconfiguration resets the ESP32. Never restart it to conceal
         * a rejected FPGA request or boot the wrong core in a retry loop. */
        vTaskDelay(pdMS_TO_TICKS(5000));
        Result = ESP_ERR_TIMEOUT;
    }
    const esp_err_t ClearResult = ClearPending();
    printf("CORE_SWITCH failed=%s pending_clear=%s\n", esp_err_to_name(Result), esp_err_to_name(ClearResult));
    return ClearResult == ESP_OK ? Result : ClearResult;
}
