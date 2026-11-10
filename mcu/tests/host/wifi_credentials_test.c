#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../main/wifi_credentials.c"
static Record Saved;
static bool Present, FailCommit;
esp_err_t nvs_open(const char *N,nvs_open_mode_t M,nvs_handle_t *H){assert(!strcmp(N,"cm_wifi"));*H=1;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t H,const char *K,const void *D,size_t S){assert(S==sizeof(Saved));memcpy(&Saved,D,S);Present=true;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t H,const char *K,void *D,size_t *S){if(!Present)return ESP_ERR_NVS_NOT_FOUND;assert(*S>=sizeof(Saved));memcpy(D,&Saved,sizeof(Saved));*S=sizeof(Saved);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t H){return FailCommit?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t H){}
esp_err_t nvs_erase_key(nvs_handle_t H,const char *K){Present=false;return ESP_OK;}
int main(void){
 WifiCredentials C={.SSID="Home",.Password="password123"},Read;
 assert(WifiCredentials_Read(&Read)==ESP_ERR_NVS_NOT_FOUND);
 assert(WifiCredentials_Save(&C)==ESP_OK && WifiCredentials_Read(&Read)==ESP_OK && !memcmp(&Read,&C,sizeof(C)));
 Saved.Version=99;assert(WifiCredentials_Read(&Read)==ESP_ERR_INVALID_RESPONSE && !Read.SSID[0]);
 strcpy(C.Password,"short");assert(!WifiCredentials_Valid(&C));
 memset(C.Password,'a',64);C.Password[64]=0;assert(WifiCredentials_Valid(&C));C.Password[63]='z';assert(!WifiCredentials_Valid(&C));
 C.Password[0]=0;assert(WifiCredentials_Valid(&C));
 memset(C.SSID,'s',32);C.SSID[32]=0;assert(WifiCredentials_Valid(&C));C.SSID[32]='x';assert(!WifiCredentials_Valid(&C));
 strcpy(C.SSID,"Home");FailCommit=true;assert(WifiCredentials_Save(&C)==ESP_FAIL);FailCommit=false;
 assert(WifiCredentials_Forget()==ESP_OK && WifiCredentials_Read(&Read)==ESP_ERR_NVS_NOT_FOUND);
 puts("PASS: network credential validation, atomic record, corruption/version rejection, storage errors, forget");
}
