#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef struct esp_netif esp_netif_t;
typedef struct {struct {struct {uint32_t addr;} ip;} ip_info;} ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(P) (unsigned)(((P)->addr>>24)&255),(unsigned)(((P)->addr>>16)&255),(unsigned)(((P)->addr>>8)&255),(unsigned)((P)->addr&255)
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(esp_netif_t *);
esp_err_t esp_netif_set_hostname(esp_netif_t *,const char *);
