#pragma once
#include <stdbool.h>
typedef struct {
    bool Connected, Hotspot, Configured;
    char Address[16], SSID[33], HotspotSSID[24], HotspotPassword[24], Message[48];
} WifiNetworkStatus;

#define WIFI_SCAN_MAX 12
typedef struct { char SSID[33]; int RSSI; bool Secure, Supported; } WifiScanEntry;
typedef struct { bool Busy; unsigned Count; char Error[64]; WifiScanEntry Entries[WIFI_SCAN_MAX]; } WifiScanStatus;
