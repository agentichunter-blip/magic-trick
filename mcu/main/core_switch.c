#include "core_switch.h"
#include "cart_store.h"
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
static const char HopsKey[] = "hops";
/* Core to land on when it is not the game's own (cartridge staging, via GB). */
static const char ViaKey[] = "via";

void CoreSwitch_ObserveVersion(uint8_t Major, uint8_t Minor)
{
    atomic_store(&CoreIdentity, Major == 50 && Minor >= 1 && Minor <= 3 ? Minor : 0);
}
bool CoreSwitch_IsSupported(void) { return atomic_load(&CoreIdentity) != 0; }
CoreSystem_t CoreSwitch_Current(void) { return (CoreSystem_t)atomic_load(&CoreIdentity); }
bool CoreSwitch_IsCore(CoreSystem_t System) { return System != kCoreSystem_None && atomic_load(&CoreIdentity) == System; }
bool CoreSwitch_IsTarget(bool GameGear) { return CoreSwitch_IsCore(GameGear ? kCoreSystem_GG : kCoreSystem_GB); }
CoreSystem_t CoreSwitch_SystemForPath(const char *Path)
{
    const char *Extension = Path ? strrchr(Path, '.') : NULL;
    if (Extension && !strcasecmp(Extension, ".gg")) return kCoreSystem_GG;
    if (Extension && (!strcasecmp(Extension, ".lnx") || !strcasecmp(Extension, ".lyx") ||
                      !strcasecmp(Extension, ".o"))) return kCoreSystem_Lynx;
    return kCoreSystem_GB;
}
CoreSystem_t CoreSwitch_NextCore(CoreSystem_t System)
{
    return System == kCoreSystem_GB ? kCoreSystem_GG : System == kCoreSystem_GG ? kCoreSystem_Lynx :
        System == kCoreSystem_Lynx ? kCoreSystem_GB : kCoreSystem_None;
}
static esp_err_t ClearPending(void)
{
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READWRITE, &Handle);
    if (Result != ESP_OK) return Result;
    Result = nvs_erase_key(Handle, Key);
    if (Result == ESP_ERR_NVS_NOT_FOUND) Result = ESP_OK;
    esp_err_t HopsResult = nvs_erase_key(Handle, HopsKey);
    if (HopsResult == ESP_ERR_NVS_NOT_FOUND) HopsResult = ESP_OK;
    if (Result == ESP_OK) Result = HopsResult;
    const esp_err_t ViaResult = nvs_erase_key(Handle, ViaKey);
    if (Result == ESP_OK && ViaResult != ESP_ERR_NVS_NOT_FOUND) Result = ViaResult;
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
esp_err_t CoreSwitch_TakePendingHops(char Path[LAST_GAME_PATH_SIZE], uint8_t *pHops)
{
    return CoreSwitch_TakePendingTarget(Path, pHops, NULL);
}
esp_err_t CoreSwitch_TakePendingTarget(char Path[LAST_GAME_PATH_SIZE], uint8_t *pHops, CoreSystem_t *pTarget)
{
    if (!Path) return ESP_ERR_INVALID_ARG;
    Path[0] = 0;
    if (pHops) *pHops = 1;
    if (pTarget) *pTarget = kCoreSystem_None;
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READONLY, &Handle);
    if (Result != ESP_OK) return Result;
    size_t Size = LAST_GAME_PATH_SIZE;
    Result = nvs_get_str(Handle, Key, Path, &Size);
    uint8_t Hops = 1, Via = 0;
    if (Result == ESP_OK && nvs_get_u8(Handle, HopsKey, &Hops) != ESP_OK) Hops = 1;
    if (Result == ESP_OK && nvs_get_u8(Handle, ViaKey, &Via) != ESP_OK) Via = 0;
    nvs_close(Handle);
    if (Result == ESP_ERR_NVS_NOT_FOUND) return Result;
    if (pHops) *pHops = Hops;
    /* Clear corrupt/oversized records too. A failed clear never authorizes launch. */
    const esp_err_t ClearResult = ClearPending();
    if (ClearResult != ESP_OK) Result = ClearResult;
    if (Result == ESP_OK && !LastGame_ValidPath(Path) && !CartStore_IsCartPath(Path)) Result = ESP_ERR_INVALID_ARG;
    if (Result != ESP_OK) Path[0] = 0;
    if (Result == ESP_OK && pTarget)
        *pTarget = Via >= kCoreSystem_GB && Via <= kCoreSystem_Lynx ? (CoreSystem_t)Via : CoreSwitch_SystemForPath(Path);
    return Result;
}
esp_err_t CoreSwitch_TakePending(char Path[LAST_GAME_PATH_SIZE])
{
    return CoreSwitch_TakePendingHops(Path, NULL);
}
static const char *Name(CoreSystem_t System)
{
    return System == kCoreSystem_GG ? "GG" : System == kCoreSystem_Lynx ? "LYNX" : "GB_GBC";
}
static esp_err_t Reload(const char *Path, uint8_t Hops, CoreSystem_t Target)
{
    if (!LastGame_ValidPath(Path) && !CartStore_IsCartPath(Path)) return ESP_ERR_INVALID_ARG;
    if (!CoreSwitch_IsSupported()) return ESP_ERR_NOT_SUPPORTED;
    if (Target == kCoreSystem_None) Target = CoreSwitch_SystemForPath(Path);
    if (CoreSwitch_IsCore(Target)) return ESP_ERR_INVALID_STATE;
    if (Hops > kCoreSwitchMaxHops) return ESP_ERR_INVALID_STATE;
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READWRITE, &Handle);
    if (Result != ESP_OK) return Result;
    Result = nvs_set_str(Handle, Key, Path);
    if (Result == ESP_OK && Hops > 1) Result = nvs_set_u8(Handle, HopsKey, Hops);
    if (Result == ESP_OK && Target != CoreSwitch_SystemForPath(Path)) Result = nvs_set_u8(Handle, ViaKey, (uint8_t)Target);
    if (Result == ESP_OK) Result = nvs_commit(Handle);
    nvs_close(Handle);
    if (Result != ESP_OK) {
        (void)ClearPending();
        return Result;
    }
    const uint16_t Nonce = (uint16_t)esp_random();
    const CoreSystem_t Next = CoreSwitch_NextCore(CoreSwitch_Current());
    printf("CORE_SWITCH target=%s next=%s hop=%u pending=committed\n", Name(Target), Name(Next), (unsigned)Hops);
    /* The target byte is informational: the FPGA boots its static next slot. */
    const uint8_t TargetByte = Next == kCoreSystem_GG ? 1 : 0;
    Result = FPGA_Tx_CoreReload(0xa1, TargetByte, Nonce);
    if (Result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        Result = FPGA_Tx_CoreReload(0xa2, TargetByte, Nonce);
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
esp_err_t CoreSwitch_Request(const char *Path) { return Reload(Path, 1, kCoreSystem_None); }
esp_err_t CoreSwitch_RequestVia(const char *Path, CoreSystem_t Target) { return Reload(Path, 1, Target); }
esp_err_t CoreSwitch_Continue(const char *Path, uint8_t Hops)
{
    return CoreSwitch_ContinueTo(Path, Hops, kCoreSystem_None);
}
esp_err_t CoreSwitch_ContinueTo(const char *Path, uint8_t Hops, CoreSystem_t Target)
{
    if (Hops >= kCoreSwitchMaxHops) return ESP_ERR_INVALID_STATE;
    return Reload(Path, (uint8_t)(Hops + 1), Target);
}

esp_err_t CoreSwitch_SetPending(const char *Path)
{
    if (!LastGame_ValidPath(Path) && !CartStore_IsCartPath(Path)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t Handle;
    esp_err_t Result = nvs_open(Namespace, NVS_READWRITE, &Handle);
    if (Result != ESP_OK) return Result;
    Result = nvs_set_str(Handle, Key, Path);
    if (Result == ESP_OK) Result = nvs_commit(Handle);
    nvs_close(Handle);
    return Result;
}
