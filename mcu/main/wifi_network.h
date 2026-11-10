#pragma once
#include "wifi_network_types.h"
#include "wifi_credentials.h"
esp_err_t WifiNetwork_Start(bool ForceHotspot);
void WifiNetwork_Stop(void);
esp_err_t WifiNetwork_Poll(void);
void WifiNetwork_GetStatus(WifiNetworkStatus *Status);
esp_err_t WifiNetwork_Configure(const WifiCredentials *Credentials);
esp_err_t WifiNetwork_Forget(void);

esp_err_t WifiNetwork_RequestScan(void);
void WifiNetwork_GetScan(WifiScanStatus *Status);
