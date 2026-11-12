#include <assert.h>
#include <stdlib.h>
#include "../../main/virtual_cart.c"
#ifndef NATIVE_TEST_RAM_SIZE
#define NATIVE_TEST_RAM_SIZE 8192
#endif
static uint8_t Ram[NATIVE_TEST_RAM_SIZE], Frozen[NATIVE_TEST_RAM_SIZE], OldState[NATIVE_TEST_RAM_SIZE];
static bool Paused,Dirty=true,Valid,FailLoad,FailStatus,OldFpga,Mounted,Bulk;
static unsigned ResumeFailure;static bool ResumeIssued;
static unsigned LoadCommands,Resumes;static uint8_t Block;static uint16_t Sequence;
static sdmmc_card_t Card;
#ifndef NATIVE_TEST_TASK_CLOCK
void vTaskDelay(TickType_t t){(void)t;}
TickType_t xTaskGetTickCount(void){return 0;}
#endif
esp_err_t SDCard_Mount(sdmmc_card_t **p){assert(!Mounted);Mounted=true;*p=&Card;return ESP_OK;}
esp_err_t SDCard_Unmount(sdmmc_card_t *p){assert(Mounted && p==&Card);
#ifdef NATIVE_TEST_UNMOUNT_HOOK
 NativeUnmountHook();
#endif
 Mounted=false;return ESP_OK;}
esp_err_t CartBulk_Begin(void){assert(Mounted && !Bulk);Bulk=true;return ESP_OK;}
void CartBulk_End(void){assert(Bulk);Bulk=false;}
esp_err_t CartBulk_WritePSRAMBlock(uint32_t a,const uint8_t *d){(void)a;(void)d;assert(Bulk);
#ifdef NATIVE_TEST_SLOT_HOOKS
 NativeWritePsramHook(a,d);
#endif
 return ESP_OK;}
esp_err_t CartBulk_ReadVirtualBlock(uint16_t seq,uint8_t *data){assert(Bulk && seq==Sequence);
#ifdef NATIVE_TEST_SLOT_HOOKS
 if(NativeReadBlockHook(data))return ESP_OK;
#endif
#ifdef NATIVE_TEST_SAVE_BLOCK_HOOK
 NativeSaveBlockHook();
#endif
 memcpy(data,Frozen+Block*1024,1024);return ESP_OK;}
esp_err_t CartLink_VirtualCommand(uint8_t cmd,uint16_t conf,uint8_t high,CartLinkResponse_t *r){
 memset(r,0,sizeof(*r));r->Count=4;
#ifdef NATIVE_TEST_SLOT_HOOKS
 if(NativeCommandHook(cmd,conf,high,r))return ESP_OK;
#endif
 if(cmd==8) Paused=true;
 if(cmd==9){ResumeIssued=true;++Resumes;
  if(ResumeFailure==1)return ESP_ERR_TIMEOUT;
  if(ResumeFailure==2){r->Status=3;return ESP_OK;}
  if(ResumeFailure!=3)Paused=false;
 }
 if(cmd==2 && ResumeIssued && ResumeFailure==4)return ESP_ERR_TIMEOUT;
 if(cmd==4){
#ifndef NATIVE_TEST_ALLOW_BACKGROUND_SAVE
 assert(Paused);
#endif
Block=high;Sequence=conf;if(!Block){memcpy(Frozen,Ram,sizeof(Ram));Dirty=false;}}
 if(cmd==10){assert(Paused);memcpy(OldState,Ram,sizeof(Ram));Valid=true;}
 if(cmd==11){assert(Paused && Valid);++LoadCommands;memcpy(Ram,OldState,sizeof(Ram));Dirty=true;if(FailLoad)return ESP_ERR_TIMEOUT;}
 if(cmd>=10){
   if(FailStatus && cmd==12) return ESP_ERR_TIMEOUT;
   uint32_t status=OldFpga?0:(0x53530008 | (Valid?2:0));
   for(int i=0;i<4;i++)r->Data[i]=status>>(8*i);
 }else{r->Data[0]=1|(Dirty?4:0);r->Data[1]=Paused?2:0;}
 return ESP_OK;
}
static void verify(const char *path,uint8_t value){FILE*f=fopen(path,"rb");assert(f);for(unsigned i=0;i<sizeof(Ram);i++)assert(fgetc(f)==value);assert(fgetc(f)==EOF);fclose(f);}
int main(void){
 char dir[]="/tmp/native-state-XXXXXX";assert(mkdtemp(dir));
 Active=true;ActiveInfo.RamSize=sizeof(Ram);ActiveInfo.Mapper.Kind=kCartMapper_Mbc5;ActiveInfo.Mapper.HasBattery=true;
 snprintf(ActiveSavePath,sizeof(ActiveSavePath),"%s/game.sav",dir);
 char canonical[kPathSize];strcpy(canonical,ActiveSavePath);
 memset(Ram,0x11,sizeof(Ram));assert(VirtualCart_StateCapture(false)==ESP_OK);assert(!Paused && Valid);
 memset(Ram,0x77,sizeof(Ram));Dirty=true;
 assert(VirtualCart_StateCapture(true)==ESP_OK);assert(!Paused && LoadCommands==1);verify(canonical,0x77);
 assert(strcmp(ActiveSavePath,canonical));char branch1[kPathSize];strcpy(branch1,ActiveSavePath);
 // Autosave and Stop use exactly this shared persistence path after restoring.
 Paused=true;Mounted=Bulk=true;assert(PersistActiveSave()==ESP_OK);Mounted=Bulk=false;verify(branch1,0x11);verify(canonical,0x77);Paused=false;
 memset(Ram,0x22,sizeof(Ram));Dirty=true;
 assert(VirtualCart_StateCapture(true)==ESP_OK);assert(strcmp(branch1,ActiveSavePath));verify(branch1,0x22);verify(canonical,0x77);
 char branch2[kPathSize];strcpy(branch2,ActiveSavePath);
 memset(Ram,0x33,sizeof(Ram));Dirty=true;FailLoad=true;
 unsigned resumes=Resumes;
 assert(VirtualCart_StateCapture(true)==ESP_ERR_TIMEOUT);assert(StateUncertain && Paused && Resumes==resumes);
 assert(!SetRunning(true));assert(VirtualCart_IsBusy());assert(!SetLoadRequested(true));assert(VirtualCart_Stop()==ESP_ERR_INVALID_STATE);verify(canonical,0x77);verify(branch2,0x33);
 char branch3[kPathSize];strcpy(branch3,ActiveSavePath);
 assert(!strcmp(StateCanonicalPath,canonical));
 // No restoration can start if capability/status cannot be established.
 StateUncertain=false;FailLoad=false;FailStatus=true;unsigned loads=LoadCommands;
 assert(VirtualCart_StateCapture(true)==ESP_ERR_TIMEOUT && LoadCommands==loads);
 FailStatus=false;OldFpga=true;
 assert(VirtualCart_StateCapture(true)==ESP_ERR_NOT_SUPPORTED && LoadCommands==loads);
 OldFpga=false;ActiveInfo.Mapper.HasRtc=true;
 assert(VirtualCart_StateCapture(true)==ESP_ERR_INVALID_STATE && LoadCommands==loads);
 ActiveInfo.Mapper.HasRtc=false;
 // A lost/rejected resume, an acknowledged but stuck pause, and lost status
 // must never turn a completed snapshot into false success.
 for(ResumeFailure=1;ResumeFailure<=4;++ResumeFailure){
  StateUncertain=false;ResumeIssued=false;Paused=false;
  assert(VirtualCart_StateCapture(false)!=ESP_OK);
  assert(StateUncertain && VirtualCart_IsBusy() && !SetRunning(true));
  assert(!Mounted && !Bulk && !Running);verify(canonical,0x77);
 }
 ResumeFailure=0;ResumeIssued=false;StateUncertain=false;Paused=false;
 assert(!Mounted && !Bulk && !Running);
 char extra[kPathSize],tiny[8];
 assert(StateSaveBranch_Create(canonical,canonical,sizeof(Ram)-1,extra,sizeof(extra))!=0);
 assert(StateSaveBranch_Create(canonical,canonical,sizeof(Ram),tiny,sizeof(tiny))!=0);
 assert(StateSaveBranch_Create(canonical,canonical,sizeof(Ram),extra,sizeof(extra))==0);
 assert(strcmp(extra,branch1) && strcmp(extra,branch2) && strcmp(extra,branch3));verify(extra,0x77);verify(canonical,0x77);
 unlink(extra);
 // A queued UI action cannot capture or restore a different game session.
 unsigned before_stale=LoadCommands;
 assert(VirtualCart_StateCaptureForSession(true,NativeSession+1)==ESP_ERR_INVALID_STATE);
 assert(LoadCommands==before_stale && !Running && !NativeRequested);
 bool supported=false,valid=false,uncertain=true;uint32_t session=0;
 assert(VirtualCart_NativeInfo(&supported,&valid,&uncertain,&session)==ESP_OK);
 assert(supported && !uncertain && session==NativeSession && !Running);
 // The normal Stop path persists the restored timeline only to its selected branch.
 FailStatus=false;Dirty=true;assert(VirtualCart_Stop()==ESP_OK);assert(!Active);verify(branch3,0x11);verify(canonical,0x77);
 unlink(canonical);unlink(branch1);unlink(branch2);unlink(branch3);assert(!rmdir(dir));
 puts("PASS firmware: capture, newer-save preservation, repeated branches, autosave, ambiguous restore, unsupported status");
}
void PwrMgr_InhibitSleep(void){}
void PwrMgr_AllowSleep(void){}
