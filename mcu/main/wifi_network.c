#include "wifi_network.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static portMUX_TYPE Lock = portMUX_INITIALIZER_UNLOCKED;
static WifiNetworkStatus State;
static WifiCredentials Pending;
static bool PendingChange;
static WifiScanStatus Scan;
static bool ScanRequested, ScanDone, ScanActive;
static int64_t ScanStarted;
static bool Initialized, Driver, Started, HaveConfig;
static esp_netif_t *STA;
static esp_event_handler_instance_t WifiHandler, IPHandler;
static bool WifiRegistered, IPRegistered;
static int64_t NextRetry;

void WifiNetwork_GetStatus(WifiNetworkStatus *Out)
{
    portENTER_CRITICAL(&Lock); *Out = State; portEXIT_CRITICAL(&Lock);
}
static void SetMessage(const char *Message)
{
    portENTER_CRITICAL(&Lock); snprintf(State.Message,sizeof(State.Message),"%s",Message); portEXIT_CRITICAL(&Lock);
}
static void OnEvent(void *Arg, esp_event_base_t Base, int32_t ID, void *Data)
{
    (void)Arg;
    portENTER_CRITICAL(&Lock);
    if (Base == WIFI_EVENT && ID == WIFI_EVENT_SCAN_DONE)
    {
        if(ScanActive) ScanDone=true;
    }
    else if (Base == IP_EVENT && ID == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *Event = Data;
        State.Connected = true;
        snprintf(State.Address, sizeof(State.Address), IPSTR, IP2STR(&Event->ip_info.ip));
        snprintf(State.Message,sizeof(State.Message),"ROUTER CONNECTED");
    }
    else if ((Base == WIFI_EVENT && ID == WIFI_EVENT_STA_DISCONNECTED) ||
             (Base == IP_EVENT && ID == IP_EVENT_STA_LOST_IP))
    {
        State.Connected = false; State.Address[0] = 0;
        snprintf(State.Message,sizeof(State.Message),"ROUTER DISCONNECTED");
    }
    portEXIT_CRITICAL(&Lock);
}
static esp_err_t ApplyCredentials(const WifiCredentials *C)
{
    wifi_config_t Config = {0};
    HaveConfig = WifiCredentials_Valid(C);
    if (HaveConfig)
    {
        memcpy(Config.sta.ssid,C->SSID,strlen(C->SSID));
        memcpy(Config.sta.password,C->Password,strlen(C->Password));
        Config.sta.threshold.authmode = C->Password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
    (void)esp_wifi_disconnect();
    esp_err_t R=esp_wifi_set_config(WIFI_IF_STA,&Config);
    portENTER_CRITICAL(&Lock);
    State.Connected=false; State.Address[0]=0; State.Configured=HaveConfig;
    snprintf(State.SSID,sizeof(State.SSID),"%s",HaveConfig ? C->SSID : "");
    portEXIT_CRITICAL(&Lock);
    NextRetry=esp_timer_get_time()+5000000;
    if (R != ESP_OK) return R;
    if (HaveConfig)
    {
        SetMessage("CONNECTING TO ROUTER");
        /* Association failures are recoverable; Poll retries and opens fallback. */
        (void)esp_wifi_connect();
    }
    else
    {
        SetMessage("SCAN TO CHOOSE A NETWORK");
    }
    return R;
}
esp_err_t WifiNetwork_Configure(const WifiCredentials *C)
{
    portENTER_CRITICAL(&Lock); bool Busy=Scan.Busy; portEXIT_CRITICAL(&Lock);
    if(Busy) return ESP_ERR_INVALID_STATE;
    esp_err_t R=WifiCredentials_Save(C);
    if(R != ESP_OK) return R;
    portENTER_CRITICAL(&Lock); Pending=*C; PendingChange=true; portEXIT_CRITICAL(&Lock);
    return ESP_OK;
}
esp_err_t WifiNetwork_Forget(void)
{
    portENTER_CRITICAL(&Lock); bool Busy=Scan.Busy; portEXIT_CRITICAL(&Lock);
    if(Busy) return ESP_ERR_INVALID_STATE;
    esp_err_t R=WifiCredentials_Forget();
    if(R != ESP_OK) return R;
    portENTER_CRITICAL(&Lock); memset(&Pending,0,sizeof(Pending)); PendingChange=true; portEXIT_CRITICAL(&Lock);
    return ESP_OK;
}
void WifiNetwork_GetScan(WifiScanStatus *Out)
{
    portENTER_CRITICAL(&Lock); *Out=Scan; portEXIT_CRITICAL(&Lock);
}
esp_err_t WifiNetwork_RequestScan(void)
{
    portENTER_CRITICAL(&Lock);
    if(Scan.Busy || PendingChange) { portEXIT_CRITICAL(&Lock); return ESP_ERR_INVALID_STATE; }
    memset(&Scan,0,sizeof(Scan)); Scan.Busy=true; ScanRequested=true;
    portEXIT_CRITICAL(&Lock);
    return ESP_OK;
}
static void ScanError(const char *Message)
{
    (void)esp_wifi_clear_ap_list();
    portENTER_CRITICAL(&Lock);
    Scan.Busy=ScanActive=ScanDone=false;
    snprintf(Scan.Error,sizeof(Scan.Error),"%s",Message);
    portEXIT_CRITICAL(&Lock);
}
static void PollScan(void)
{
    bool Requested,Done,Active;
    portENTER_CRITICAL(&Lock);
    Requested=ScanRequested; ScanRequested=false; Done=ScanDone; ScanDone=false; Active=ScanActive;
    portEXIT_CRITICAL(&Lock);
    if(Requested)
    {
        WifiNetworkStatus Current; WifiNetwork_GetStatus(&Current);
        if(!Current.Connected) (void)esp_wifi_disconnect();
        portENTER_CRITICAL(&Lock); ScanActive=true; portEXIT_CRITICAL(&Lock);
        ScanStarted=esp_timer_get_time();
        if(esp_wifi_scan_start(NULL,false)!=ESP_OK) ScanError("Could not scan. Wait a moment and try again.");
        return;
    }
    if(Active && esp_timer_get_time()-ScanStarted>15000000)
    {
        (void)esp_wifi_scan_stop(); ScanError("Scan timed out. Try again."); return;
    }
    if(!Active || !Done) return;
    uint16_t Count=WIFI_SCAN_MAX;
    wifi_ap_record_t *Records=calloc(Count,sizeof(*Records));
    if(!Records) { ScanError("Not enough memory to scan. Try again."); return; }
    esp_err_t R=esp_wifi_scan_get_ap_records(&Count,Records);
    if(R!=ESP_OK) { free(Records); ScanError("Could not read scan results. Try again."); return; }
    portENTER_CRITICAL(&Lock);
    Scan.Count=0;
    for(unsigned I=0; I<Count; ++I)
    {
        char SSID[33]; memcpy(SSID,Records[I].ssid,32); SSID[32]=0;
        if(!SSID[0]) continue;
        unsigned J=0; while(J<Scan.Count && strcmp(SSID,Scan.Entries[J].SSID)) ++J;
        if(J<Scan.Count && Scan.Entries[J].RSSI>=Records[I].rssi) continue;
        if(J==Scan.Count) ++Scan.Count;
        WifiScanEntry *E=&Scan.Entries[J];
        memcpy(E->SSID,SSID,sizeof(SSID)); E->RSSI=Records[I].rssi;
        E->Secure=Records[I].authmode!=WIFI_AUTH_OPEN;
        E->Supported=Records[I].authmode==WIFI_AUTH_OPEN || Records[I].authmode==WIFI_AUTH_WPA2_PSK ||
            Records[I].authmode==WIFI_AUTH_WPA_WPA2_PSK || Records[I].authmode==WIFI_AUTH_WPA2_WPA3_PSK;
    }
    for(unsigned I=0;I<Scan.Count;++I) for(unsigned J=I+1;J<Scan.Count;++J)
        if(Scan.Entries[J].RSSI>Scan.Entries[I].RSSI)
        { WifiScanEntry T=Scan.Entries[I]; Scan.Entries[I]=Scan.Entries[J]; Scan.Entries[J]=T; }
    Scan.Busy=ScanActive=ScanDone=false;
    portEXIT_CRITICAL(&Lock);
    free(Records);
    NextRetry=esp_timer_get_time()+1000000;
}
esp_err_t WifiNetwork_Start(bool Unused)
{
    (void)Unused;
    portENTER_CRITICAL(&Lock);
    memset(&State,0,sizeof(State)); memset(&Pending,0,sizeof(Pending));
    PendingChange=false;
    memset(&Scan,0,sizeof(Scan)); ScanRequested=ScanDone=ScanActive=false;
    portEXIT_CRITICAL(&Lock);
    esp_err_t R;
    if(!Initialized)
    {
        R=esp_netif_init(); if(R != ESP_OK) return R;
        R=esp_event_loop_create_default(); if(R != ESP_OK && R != ESP_ERR_INVALID_STATE) return R;
        Initialized=true;
    }
    STA=esp_netif_create_default_wifi_sta();
    if(!STA) { R=ESP_ERR_NO_MEM; goto Fail; }
    (void)esp_netif_set_hostname(STA,"chromagic");
    wifi_init_config_t Init=WIFI_INIT_CONFIG_DEFAULT();
    R=esp_wifi_init(&Init); if(R != ESP_OK) goto Fail; Driver=true;
    R=esp_wifi_set_storage(WIFI_STORAGE_RAM); if(R != ESP_OK) goto Fail;
    R=esp_wifi_set_mode(WIFI_MODE_STA); if(R != ESP_OK) goto Fail;
    R=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,OnEvent,NULL,&WifiHandler);
    if(R != ESP_OK) goto Fail;
    WifiRegistered=true;
    R=esp_event_handler_instance_register(IP_EVENT,ESP_EVENT_ANY_ID,OnEvent,NULL,&IPHandler);
    if(R != ESP_OK) goto Fail;
    IPRegistered=true;
    R=esp_wifi_start(); if(R != ESP_OK) goto Fail; Started=true;
    WifiCredentials Saved={0}; (void)WifiCredentials_Read(&Saved);
    R=ApplyCredentials(&Saved); memset(&Saved,0,sizeof(Saved));
    if(R != ESP_OK) goto Fail;
    return ESP_OK;
Fail:
    WifiNetwork_Stop(); return R;
}
esp_err_t WifiNetwork_Poll(void)
{
    PollScan();
    WifiCredentials New={0}; bool Change,Scanning;
    portENTER_CRITICAL(&Lock);
    Change=PendingChange; Scanning=Scan.Busy;
    if(Change) { New=Pending; memset(&Pending,0,sizeof(Pending)); PendingChange=false; }
    portEXIT_CRITICAL(&Lock);
    if(Change)
    {
        esp_err_t R=ApplyCredentials(&New); memset(&New,0,sizeof(New));
        if(R != ESP_OK) return R;
    }
    WifiNetworkStatus Current; WifiNetwork_GetStatus(&Current);
    int64_t Now=esp_timer_get_time();
    if(HaveConfig && !Current.Connected && !Scanning && Now>=NextRetry)
    { (void)esp_wifi_connect(); NextRetry=Now+5000000; }
    return ESP_OK;
}
void WifiNetwork_Stop(void)
{
    if(ScanActive) { (void)esp_wifi_scan_stop(); (void)esp_wifi_clear_ap_list(); }
    if(WifiRegistered) { esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,WifiHandler); WifiRegistered=false; }
    if(IPRegistered) { esp_event_handler_instance_unregister(IP_EVENT,ESP_EVENT_ANY_ID,IPHandler); IPRegistered=false; }
    if(Started) { esp_wifi_stop(); Started=false; }
    if(Driver) { esp_wifi_deinit(); Driver=false; }
    if(STA) { esp_netif_destroy_default_wifi(STA); STA=NULL; }
    portENTER_CRITICAL(&Lock); memset(&State,0,sizeof(State)); memset(&Pending,0,sizeof(Pending)); PendingChange=false; memset(&Scan,0,sizeof(Scan)); ScanRequested=ScanDone=ScanActive=false; portEXIT_CRITICAL(&Lock);
}
