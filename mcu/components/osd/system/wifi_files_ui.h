#pragma once
#include "osd_shared.h"
#include <stdbool.h>
#include "wifi_network_types.h"
typedef struct {
    bool Busy, Ready;
    char AccessCode[13], Message[48];
    WifiNetworkStatus Network;
} WifiFilesStatus;
typedef void (*WifiFilesToggle)(bool Enable);
typedef void (*WifiFilesGetStatus)(WifiFilesStatus *Status);
void WifiFilesUI_Register(WifiFilesToggle Toggle, WifiFilesGetStatus GetStatus);
void WifiFilesUI_RegisterNetwork(bool (*Scan)(void), void (*GetScan)(WifiScanStatus *),
                                 bool (*Join)(const char *, const char *));
bool WifiFilesUI_IsBusy(void);
bool WifiFilesUI_IsSelected(void);
OSD_Result_t WifiFilesUI_Draw(void *Arg);
OSD_Result_t WifiFilesUI_OnButton(Button_t Button, ButtonState_t State, void *Arg);
OSD_Result_t WifiFilesUI_OnTransition(void *Arg);

bool WifiFilesUI_IsOpen(void);
bool WifiFilesUI_IsConnected(void);
bool WifiFilesUI_HandlesButton(Button_t Button);

lv_obj_t *WifiFilesUI_CreateIcon(lv_obj_t *Parent,bool Dark);
