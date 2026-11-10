#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/wifi_network.c"
static int64_t Now=1000;
static int Mode, Connects, Nets, Registrations;
static bool HasSaved, Radio, FailInit, ScanFails;
static unsigned Scans, Clears;
static WifiCredentials Saved={.SSID="Home",.Password="password123"};
int64_t esp_timer_get_time(void){return Now;}
void esp_fill_random(void *B,size_t N){assert(Radio);memset(B,0xAB,N);}
esp_err_t esp_netif_init(void){return ESP_OK;}
esp_err_t esp_event_loop_create_default(void){return ESP_OK;}
esp_netif_t *esp_netif_create_default_wifi_ap(void){++Nets;return (esp_netif_t *)1;}
esp_netif_t *esp_netif_create_default_wifi_sta(void){++Nets;return (esp_netif_t *)2;}
void esp_netif_destroy_default_wifi(esp_netif_t *N){assert(Nets>0);--Nets;}
esp_err_t esp_netif_set_hostname(esp_netif_t *N,const char *Name){return ESP_OK;}
esp_err_t esp_event_handler_instance_register(esp_event_base_t B,int32_t I,void (*F)(void *,esp_event_base_t,int32_t,void *),void *A,esp_event_handler_instance_t *H){++Registrations;*H=(void *)1;return ESP_OK;}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t B,int32_t I,esp_event_handler_instance_t H){assert(Registrations>0);--Registrations;return ESP_OK;}
esp_err_t esp_wifi_init(const wifi_init_config_t *C){return FailInit?ESP_ERR_NO_MEM:ESP_OK;}
esp_err_t esp_wifi_deinit(void){assert(!Radio);return ESP_OK;}
esp_err_t esp_wifi_set_storage(int S){return ESP_OK;}
esp_err_t esp_wifi_set_mode(int M){Mode=M;return ESP_OK;}
esp_err_t esp_wifi_set_config(int I,wifi_config_t *C){return ESP_OK;}
esp_err_t esp_wifi_start(void){Radio=true;return ESP_OK;}
esp_err_t esp_wifi_stop(void){Radio=false;return ESP_OK;}
esp_err_t esp_wifi_connect(void){++Connects;return ESP_OK;}
esp_err_t esp_wifi_disconnect(void){return ESP_OK;}
bool WifiCredentials_Valid(const WifiCredentials *C){return C && C->SSID[0];}
esp_err_t WifiCredentials_Read(WifiCredentials *C){if(!HasSaved)return ESP_ERR_NOT_FOUND;*C=Saved;return ESP_OK;}
esp_err_t WifiCredentials_Save(const WifiCredentials *C){if(!WifiCredentials_Valid(C))return ESP_ERR_INVALID_ARG;Saved=*C;HasSaved=true;return ESP_OK;}
esp_err_t WifiCredentials_Forget(void){HasSaved=false;return ESP_OK;}
esp_err_t esp_wifi_scan_start(const void *C,bool Block){assert(!Block);++Scans;return ScanFails?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_scan_stop(void){return ESP_OK;}
esp_err_t esp_wifi_clear_ap_list(void){++Clears;return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *N,wifi_ap_record_t *R){
 assert(*N==WIFI_SCAN_MAX);*N=5;
 R[0]=(wifi_ap_record_t){.ssid="Home",.rssi=-70,.authmode=WIFI_AUTH_WPA2_PSK};
 R[1]=(wifi_ap_record_t){.ssid="Home",.rssi=-40,.authmode=WIFI_AUTH_WPA2_PSK};
 R[2]=(wifi_ap_record_t){.ssid="Cafe",.rssi=-55,.authmode=WIFI_AUTH_OPEN};
 R[3]=(wifi_ap_record_t){.ssid="",.rssi=-30,.authmode=WIFI_AUTH_OPEN};
 R[4]=(wifi_ap_record_t){.ssid="WPA3 only",.rssi=-80,.authmode=WIFI_AUTH_WPA3_PSK};return ESP_OK;
}
static void Connected(void){ip_event_got_ip_t E={.ip_info.ip.addr=0xc0a80122};OnEvent(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&E);}
static void Clean(void){WifiNetwork_Stop();assert(!Nets&&!Registrations&&!Radio);}
int main(void){
 WifiNetworkStatus S;
 assert(WifiNetwork_Start(false)==ESP_OK);WifiNetwork_GetStatus(&S);assert(!S.Hotspot&&!S.Configured&&!S.Connected && Mode==WIFI_MODE_STA && Nets==1);assert(!Connects);
 WifiScanStatus ScanResult;
 assert(WifiNetwork_RequestScan()==ESP_OK);assert(WifiNetwork_RequestScan()==ESP_ERR_INVALID_STATE);
 assert(WifiNetwork_Configure(&Saved)==ESP_ERR_INVALID_STATE);assert(WifiNetwork_Forget()==ESP_ERR_INVALID_STATE);
 assert(WifiNetwork_Poll()==ESP_OK && Scans==1);WifiNetwork_GetScan(&ScanResult);assert(ScanResult.Busy);
 Now+=6000000;assert(WifiNetwork_Poll()==ESP_OK && !Connects);
 OnEvent(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,NULL);assert(WifiNetwork_Poll()==ESP_OK);
 WifiNetwork_GetScan(&ScanResult);assert(!ScanResult.Busy && ScanResult.Count==3);
 assert(!strcmp(ScanResult.Entries[0].SSID,"Home") && ScanResult.Entries[0].RSSI==-40);
 assert(ScanResult.Entries[0].Secure && ScanResult.Entries[0].Supported);
 assert(!ScanResult.Entries[1].Secure && ScanResult.Entries[1].Supported);
 assert(!ScanResult.Entries[2].Supported);
 ScanFails=true;assert(WifiNetwork_RequestScan()==ESP_OK);assert(WifiNetwork_Poll()==ESP_OK);
 WifiNetwork_GetScan(&ScanResult);assert(!ScanResult.Busy && ScanResult.Error[0] && Clears);ScanFails=false;
 assert(WifiNetwork_RequestScan()==ESP_OK);assert(WifiNetwork_Poll()==ESP_OK);Now+=15000001;assert(WifiNetwork_Poll()==ESP_OK);
 WifiNetwork_GetScan(&ScanResult);assert(!ScanResult.Busy && strstr(ScanResult.Error,"timed out"));
 assert(WifiNetwork_Configure(&Saved)==ESP_OK);assert(WifiNetwork_Poll()==ESP_OK);assert(Connects==1);
 Connected();assert(WifiNetwork_Poll()==ESP_OK);WifiNetwork_GetStatus(&S);assert(S.Connected&&!strcmp(S.Address,"192.168.1.34")&&!S.Hotspot);
 OnEvent(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);Now+=20000001;
 int Before=Connects;assert(WifiNetwork_Poll()==ESP_OK && Connects==Before+1);
 WifiNetwork_GetStatus(&S);assert(!S.Hotspot&&!S.Connected);Clean();
 assert(WifiNetwork_Start(false)==ESP_OK);WifiNetwork_GetStatus(&S);assert(!S.Hotspot&&S.Configured && Nets==1);
 assert(WifiNetwork_Forget()==ESP_OK);assert(WifiNetwork_Poll()==ESP_OK);WifiNetwork_GetStatus(&S);assert(!S.Configured&&!S.Hotspot&&!HasSaved);Clean();
 FailInit=true;assert(WifiNetwork_Start(false)==ESP_ERR_NO_MEM);assert(!Nets&&!Registrations&&!Radio);
 puts("PASS: station-only setup, router join, scan, disconnect/retry, saved network, forget, startup cleanup");
}
