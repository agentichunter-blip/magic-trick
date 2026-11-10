#include "wifi_credentials.h"
#include "nvs.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>
/* Single versioned NVS blob: SSID and password are committed together. */
typedef struct { uint32_t Version; WifiCredentials Credentials; } Record;
bool WifiCredentials_Valid(const WifiCredentials *C)
{
    if (!C) return false;
    size_t S = strnlen(C->SSID, sizeof(C->SSID)), P = strnlen(C->Password, sizeof(C->Password));
    if (!S || S > 32 || P > 64 || (P && P < 8)) return false;
    for (size_t I=0; I<S; ++I) if ((unsigned char)C->SSID[I] < 32 || C->SSID[I] == 127) return false;
    for (size_t I=0; I<P; ++I)
        if ((unsigned char)C->Password[I] < 32 || (unsigned char)C->Password[I] > 126 ||
            (P == 64 && !isxdigit((unsigned char)C->Password[I]))) return false;
    return true;
}
esp_err_t WifiCredentials_Read(WifiCredentials *C)
{
    if (!C) return ESP_ERR_INVALID_ARG;
    memset(C, 0, sizeof(*C));
    nvs_handle_t H;
    esp_err_t R = nvs_open("cm_wifi", NVS_READONLY, &H);
    if (R != ESP_OK) return R;
    Record Data = {0}; size_t Size = sizeof(Data);
    R = nvs_get_blob(H, "network", &Data, &Size); nvs_close(H);
    if (R == ESP_OK && (Size != sizeof(Data) || Data.Version != 1 || !WifiCredentials_Valid(&Data.Credentials)))
        R = ESP_ERR_INVALID_RESPONSE;
    if (R == ESP_OK) *C = Data.Credentials;
    memset(&Data, 0, sizeof(Data));
    return R;
}
esp_err_t WifiCredentials_Save(const WifiCredentials *C)
{
    if (!WifiCredentials_Valid(C)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t H; esp_err_t R = nvs_open("cm_wifi", NVS_READWRITE, &H);
    if (R != ESP_OK) return R;
    Record Data = {.Version=1, .Credentials=*C};
    R = nvs_set_blob(H, "network", &Data, sizeof(Data));
    if (R == ESP_OK) R = nvs_commit(H);
    nvs_close(H); memset(&Data, 0, sizeof(Data)); return R;
}
esp_err_t WifiCredentials_Forget(void)
{
    nvs_handle_t H; esp_err_t R = nvs_open("cm_wifi", NVS_READWRITE, &H);
    if (R != ESP_OK) return R;
    R = nvs_erase_key(H, "network");
    if (R == ESP_ERR_NVS_NOT_FOUND) R = ESP_OK;
    if (R == ESP_OK) R = nvs_commit(H);
    nvs_close(H); return R;
}
