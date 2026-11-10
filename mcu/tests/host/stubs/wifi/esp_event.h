#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define IP_EVENT ((esp_event_base_t)2)
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_DISCONNECTED 1
#define IP_EVENT_STA_GOT_IP 2
#define IP_EVENT_STA_LOST_IP 3
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int32_t,void (*)(void *,esp_event_base_t,int32_t,void *),void *,esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t,int32_t,esp_event_handler_instance_t);

#define WIFI_EVENT_SCAN_DONE 4
