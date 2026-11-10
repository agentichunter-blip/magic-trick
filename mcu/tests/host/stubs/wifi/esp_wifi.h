#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {int ignored;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct {
 struct {unsigned char ssid[32], password[64]; int ssid_len, channel, max_connection, authmode;} ap;
 struct {unsigned char ssid[32], password[64]; struct {int authmode;} threshold;} sta;
} wifi_config_t;
enum {WIFI_STORAGE_RAM, WIFI_MODE_AP, WIFI_MODE_STA, WIFI_MODE_APSTA, WIFI_IF_AP, WIFI_IF_STA, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_OPEN};
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, wifi_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
void esp_fill_random(void *, size_t);

enum {WIFI_AUTH_WPA_WPA2_PSK=20, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_PSK};
typedef struct { uint8_t ssid[33]; int rssi, authmode; } wifi_ap_record_t;
esp_err_t esp_wifi_scan_start(const void *,bool);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *,wifi_ap_record_t *);
esp_err_t esp_wifi_clear_ap_list(void);
