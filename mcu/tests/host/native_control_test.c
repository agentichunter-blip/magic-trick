#define portTICK_PERIOD_MS 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/native_state_control.c"
static unsigned Clock, Saves, Loads, Pulses, Opens;
static bool Supported=true, PC, Uncertain, Wait, SdFailed;
static uint8_t Mask;
static unsigned LastSlot;
static uint32_t Game=1;
static esp_err_t Result=ESP_OK, SettingResult=ESP_OK;
static bool HasStoredSetting, HasStoredSlot, PendingIsSlot;
static uint8_t StoredSetting, PendingSetting, StoredSlot;
static void (*WaitCallback)(void);
TickType_t xTaskGetTickCount(void){return Clock;}
void vTaskDelay(TickType_t T){Clock+=T;}
int xTaskCreate(void (*F)(void*),const char*N,unsigned S,void*A,unsigned P,TaskHandle_t*H){return pdPASS;}
esp_err_t nvs_open(const char*N,nvs_open_mode_t M,nvs_handle_t*H){assert(!strcmp(N,"native_states"));*H=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t H,const char*K,void*V,size_t*S){
 const bool Slot=!strcmp(K,"slot");assert(Slot || !strcmp(K,"shortcuts"));
 if(!(Slot?HasStoredSlot:HasStoredSetting))return ESP_ERR_NOT_FOUND;
 assert(*S>=1);*(uint8_t*)V=Slot?StoredSlot:StoredSetting;*S=1;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t H,const char*K,const void*V,size_t S){
 PendingIsSlot=!strcmp(K,"slot");assert((PendingIsSlot || !strcmp(K,"shortcuts")) && S==1);PendingSetting=*(const uint8_t*)V;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t H){
 if(SettingResult==ESP_OK){if(PendingIsSlot){StoredSlot=PendingSetting;HasStoredSlot=true;}else{StoredSetting=PendingSetting;HasStoredSetting=true;}}
 return SettingResult;
}
void nvs_close(nvs_handle_t H){}
bool PCBackup_IsEnabled(void){return PC;}
esp_err_t FPGA_Tx_RequestButtons(void){return ESP_OK;}
esp_err_t FPGA_Tx_PulseButtons(uint16_t B){assert(B==0x100);++Pulses;return ESP_OK;}
void NativeStateUI_Register(NativeStateRead R,NativeStateRequest Q,NativeStateShortcuts H){}
void NativeStateUI_RequestOpen(void){++Opens;}
void VirtualCart_RegisterStateWait(void(*F)(void)){WaitCallback=F;}
bool VirtualCart_LastStateSdFailed(void){return SdFailed;}
esp_err_t VirtualCart_NativeSlotInfo(bool*S,uint8_t*M,bool*U,uint32_t*G){*S=Supported;*M=Mask;*U=Uncertain;*G=Game;return ESP_OK;}
esp_err_t VirtualCart_StateCaptureSlot(bool L,unsigned Slot,uint32_t G){
 assert(InFlight);assert(!Request(false,1));assert(Slot>=1 && Slot<=3);LastSlot=Slot;
 if(G!=Game)return ESP_ERR_INVALID_STATE;
 if(Wait){WaitCallback();assert(View.Phase==kStateUIWaiting);}
 if(L)++Loads;else ++Saves;
 if(Result==ESP_OK)Mask|=1u<<(Slot-1);
 return Result;
}
static void Refresh(void){NativeStateView V;Read(&V);Step();}
int main(void){
 assert(NativeStateControl_Init()==ESP_OK);assert(View.Shortcuts); /* default ON with nothing stored */
 SetShortcuts(false);Step();assert(!View.Shortcuts && StoredSetting==0);
 Refresh();assert(View.Supported && !View.Valid);assert(!Request(true,1));
 assert(Request(false,1));assert(!Request(false,1));Step();assert(Saves==1 && View.Phase==kStateUISaved);
 Wait=true;assert(Request(true,1));Step();assert(Loads==1 && View.Phase==kStateUILoaded);
 /* A save that could not reach the SD card is reported, but the state is usable. */
 Wait=false;SdFailed=true;assert(Request(false,1));Step();
 assert(Saves==2 && View.Phase==kStateUIError && View.Valid && strstr(View.Message,"not on"));
 SdFailed=false;
 /* A damaged SD file is named as such. */
 Result=ESP_ERR_INVALID_CRC;assert(Request(true,1));Step();assert(Loads==2 && strstr(View.Message,"damaged"));
 Result=ESP_ERR_NOT_FOUND;assert(Request(true,1));Step();assert(Loads==3 && !strcmp(View.Message,"No state available"));
 Result=ESP_OK;Saves=1;Loads=1;Wait=true;
 Result=ESP_ERR_TIMEOUT;assert(Request(true,1));Step();assert(View.Phase==kStateUIError && View.Valid);
 Result=ESP_OK;assert(Request(true,1));++Game;Step();assert(Loads==2); /* Previous timeout counted, stale session not executed. */
 Refresh();assert(View.Phase==kStateUIIdle);
 PC=true;Refresh();assert(!Request(false,1));PC=false;
 Uncertain=true;Refresh();assert(!Request(true,1));Uncertain=false;Refresh();
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
 /* Three slots: an action names its slot, which becomes the shortcuts' slot and survives power-off. */
 HasStoredSetting=true;StoredSetting=1;HasStoredSlot=false;
 assert(NativeStateControl_Init()==ESP_OK && View.Shortcuts && View.Slot==1);
 Mask=1;Refresh();assert(View.Valid && View.ValidMask==1);
 assert(!Request(true,2) && !Request(false,0) && !Request(false,4) && !Request(true,4));
 unsigned S0=Saves;assert(Request(false,2));assert(View.Slot==2 && !View.Valid);Step();
 assert(Saves==S0+1 && LastSlot==2 && HasStoredSlot && StoredSlot==2 && View.Valid && View.ValidMask==3);
 NativeStateControl_Buttons(0x310);Clock+=40;NativeStateControl_Buttons(0x200);Step();
 assert(Saves==S0+2 && LastSlot==2); /* MENU+UP uses the current slot. */
 unsigned L0=Loads;assert(Request(true,1));Step();assert(Loads==L0+1 && LastSlot==1 && StoredSlot==1);
 assert(Request(true,2));Step();assert(LastSlot==2 && StoredSlot==2);
 Mask=1;Refresh();assert(!View.Valid && View.ValidMask==1); /* slot 2's file went missing */
 portENTER_CRITICAL(&Lock);ShortcutEvent=2;ShortcutSession=Session;portEXIT_CRITICAL(&Lock);
 L0=Loads;Step();assert(Loads==L0 && View.Phase==kStateUIError && !strcmp(View.Message,"Slot 2 is empty"));
 assert(NativeStateControl_Init()==ESP_OK && View.Slot==2);
 StoredSlot=9;assert(NativeStateControl_Init()==ESP_OK && View.Slot==1);
 HasStoredSlot=false;assert(NativeStateControl_Init()==ESP_OK && View.Slot==1);
 /* Menu Load closes the menu. The A press that chose Load may still be held:
  * wait for its release. A new press keeps the menu open; a menu the player
  * already closed is not pulsed (that would reopen it). */
 Mask=3;Refresh();
 NativeStateControl_Buttons(0x08);unsigned P0=Pulses;
 assert(Request(true,1));Step();assert(View.Phase==kStateUILoaded);
 Clock+=300;Step();assert(Pulses==P0);
 NativeStateControl_Buttons(0);Clock+=40;Step();assert(Pulses==P0+1);
 Clock+=1000;Step();assert(Pulses==P0+1);
 assert(Request(true,1));Step();NativeStateControl_Buttons(0x04);NativeStateControl_Buttons(0);Clock+=300;Step();assert(Pulses==P0+1);
 assert(Request(true,1));Step();NativeStateControl_Buttons(0x200);Clock+=300;Step();assert(Pulses==P0+1);
 /* A menu Save leaves the menu open. */
 NativeStateControl_Buttons(0);assert(Request(false,1));Step();Clock+=1500;Step();assert(Pulses==P0+1);
 puts("PASS async native controller: empty slot, duplicate requests, autosave wait, errors, stale game, PC/recovery lockout, persisted shortcuts and feedback, three slots, menu close after Load");
}
