#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef struct { char SSID[33], Password[65]; } WifiCredentials;
bool WifiCredentials_Valid(const WifiCredentials *Credentials);
esp_err_t WifiCredentials_Read(WifiCredentials *Credentials);
esp_err_t WifiCredentials_Save(const WifiCredentials *Credentials);
esp_err_t WifiCredentials_Forget(void);
