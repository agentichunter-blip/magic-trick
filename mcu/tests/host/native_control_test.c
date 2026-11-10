#define portTICK_PERIOD_MS 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/native_state_control.c"
static unsigned Clock, Saves, Loads, Pulses, Opens;
static bool Valid, Supported=true, PC, Uncertain, Wait;
static uint32_t Game=1;
static esp_err_t Result=ESP_OK, SettingResult=ESP_OK;
static bool HasStoredSetting;
static uint8_t StoredSetting, PendingSetting;
static void (*WaitCallback)(void);
TickType_t xTaskGetTickCount(void){return Clock;}
void vTaskDelay(TickType_t T){Clock+=T;}
int xTaskCreate(void (*F)(void*),const char*N,unsigned S,void*A,unsigned P,TaskHandle_t*H){return pdPASS;}
esp_err_t nvs_open(const char*N,nvs_open_mode_t M,nvs_handle_t*H){assert(!strcmp(N,"native_states"));*H=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t H,const char*K,void*V,size_t*S){
 assert(!strcmp(K,"shortcuts"));if(!HasStoredSetting)return ESP_ERR_NOT_FOUND;
 assert(*S>=1);*(uint8_t*)V=StoredSetting;*S=1;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t H,const char*K,const void*V,size_t S){assert(!strcmp(K,"shortcuts") && S==1);PendingSetting=*(const uint8_t*)V;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t H){if(SettingResult==ESP_OK){StoredSetting=PendingSetting;HasStoredSetting=true;}return SettingResult;}
void nvs_close(nvs_handle_t H){}
bool PCBackup_IsEnabled(void){return PC;}
esp_err_t FPGA_Tx_RequestButtons(void){return ESP_OK;}
esp_err_t FPGA_Tx_PulseButtons(uint16_t B){assert(B==0x100);++Pulses;return ESP_OK;}
void NativeStateUI_Register(NativeStateRead R,NativeStateRequest Q,NativeStateShortcuts H){}
void NativeStateUI_RequestOpen(void){++Opens;}
void VirtualCart_RegisterStateWait(void(*F)(void)){WaitCallback=F;}
esp_err_t VirtualCart_NativeInfo(bool*S,bool*V,bool*U,uint32_t*G){*S=Supported;*V=Valid;*U=Uncertain;*G=Game;return ESP_OK;}
esp_err_t VirtualCart_StateCaptureForSession(bool L,uint32_t G){
 assert(InFlight);assert(!Request(false));
 if(G!=Game)return ESP_ERR_INVALID_STATE;
 if(Wait){WaitCallback();assert(View.Phase==kStateUIWaiting);}
 if(L)++Loads;else ++Saves;
 if(Result==ESP_OK)Valid=true;
 return Result;
}
static void Refresh(void){NativeStateView V;Read(&V);Step();}
int main(void){
 assert(NativeStateControl_Init()==ESP_OK);assert(View.Shortcuts); /* default ON with nothing stored */
 SetShortcuts(false);Step();assert(!View.Shortcuts && StoredSetting==0);
 Refresh();assert(View.Supported && !View.Valid);assert(!Request(true));
 assert(Request(false));assert(!Request(false));Step();assert(Saves==1 && View.Phase==kStateUISaved);
 Wait=true;assert(Request(true));Step();assert(Loads==1 && View.Phase==kStateUILoaded);
 Result=ESP_ERR_TIMEOUT;assert(Request(true));Step();assert(View.Phase==kStateUIError && View.Valid);
 Result=ESP_OK;assert(Request(true));++Game;Step();assert(Loads==2); /* Previous timeout counted, stale session not executed. */
 Refresh();assert(View.Phase==kStateUIIdle);
 PC=true;Refresh();assert(!Request(false));PC=false;
 Uncertain=true;Refresh();assert(!Request(true));Uncertain=false;Refresh();
 SetShortcuts(true);Step();assert(View.Shortcuts);
 assert(NativeStateControl_Buttons(0x310)==0x300);Clock+=40;
 assert(NativeStateControl_Buttons(0x210)==0x200);Clock+=40;
 assert(NativeStateControl_Buttons(0x200)==0x200);Step();assert(Opens==1 && Pulses==1 && Saves==2);
 NativeStateControl_Buttons(0);Clock+=1200;Step();assert(Pulses==2);
 NativeStateControl_Buttons(0x310);Clock+=40;NativeStateControl_Buttons(0x200);Step();
 NativeStateControl_Buttons(0x08);Clock+=1200;Step();assert(Pulses==3); /* A cancels auto-close. */
 NativeStateControl_Buttons(0x200);Clock+=40;
 NativeStateControl_Buttons(0x310);Clock+=40;NativeStateControl_Buttons(0x200);
 unsigned Before=Saves;++Game;Step();assert(Saves==Before && View.Phase==kStateUIError);
 SettingResult=ESP_FAIL;SetShortcuts(false);Step();assert(View.Shortcuts && View.Phase==kStateUIError && StoredSetting==1);
 SettingResult=ESP_OK;SetShortcuts(false);Step();assert(!View.Shortcuts && StoredSetting==0);
 /* Re-read persisted preferences as startup does; failed commits above must
  * not replace the stored value, only a stored 0 disables shortcuts (missing or unknown -> default ON). */
 SetShortcuts(true);Step();assert(StoredSetting==1);
 View.Shortcuts=false;assert(NativeStateControl_Init()==ESP_OK && View.Shortcuts);
 StoredSetting=0;assert(NativeStateControl_Init()==ESP_OK && !View.Shortcuts);
 StoredSetting=7;assert(NativeStateControl_Init()==ESP_OK && View.Shortcuts);
 HasStoredSetting=false;assert(NativeStateControl_Init()==ESP_OK && View.Shortcuts);
 puts("PASS async native controller: empty slot, duplicate requests, autosave wait, errors, stale game, PC/recovery lockout, persisted shortcuts and feedback");
}
