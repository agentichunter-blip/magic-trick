#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "core_switch.h"
#include "nvs.h"
#include "freertos/task.h"

static const char GG[]="/sdcard/CHROMAGIC/BACKUPS/GG/Test.gg";
static const char LYNX[]="/sdcard/CHROMAGIC/BACKUPS/LYNX/Test.lnx";
static const char *Expected=GG; static unsigned ExpectedTarget=1;
static int StoredHops=-1, StagedHops=-1;
static char Stored[400], Staged[400];
static bool Open, ReadOnly, FailSet, FailCommit, FailErase, ResetOnReload;
static unsigned Sends, FailSend, Commits;
static jmp_buf ResetPoint;
uint32_t esp_random(void) { return 0xabcd; }
esp_err_t nvs_open(const char *Name,nvs_open_mode_t Mode,nvs_handle_t *H) {
 assert(!Open && !strcmp(Name,"core_switch"));Open=true;ReadOnly=Mode==NVS_READONLY;
 strcpy(Staged,Stored);StagedHops=StoredHops;*H=1;return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t H,const char *K,char *P,size_t *N) {
 assert(Open && H==1 && !strcmp(K,"path"));if(!Stored[0])return ESP_ERR_NVS_NOT_FOUND;
 if(strlen(Stored)+1>*N)return ESP_ERR_INVALID_SIZE;
 strcpy(P,Stored);*N=strlen(P)+1;return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t H,const char *K,const char *P) {
 assert(Open && !ReadOnly && H==1 && !strcmp(K,"path"));if(FailSet)return ESP_FAIL;
 strcpy(Staged,P);return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t H,const char *K) {
 assert(Open && !ReadOnly && H==1);
 if(!strcmp(K,"hops")) { bool Had=StagedHops>=0;StagedHops=-1;return Had?ESP_OK:ESP_ERR_NVS_NOT_FOUND; }
 assert(!strcmp(K,"path"));if(FailErase)return ESP_FAIL;
 Staged[0]=0;return Stored[0]?ESP_OK:ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_get_u8(nvs_handle_t H,const char *K,uint8_t *V) {
 assert(Open && H==1 && !strcmp(K,"hops"));if(StoredHops<0)return ESP_ERR_NVS_NOT_FOUND;*V=(uint8_t)StoredHops;return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t H,const char *K,uint8_t V) {
 assert(Open && !ReadOnly && H==1 && !strcmp(K,"hops"));StagedHops=V;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t H) {
 assert(Open && !ReadOnly && H==1);if(FailCommit)return ESP_FAIL;
 strcpy(Stored,Staged);StoredHops=StagedHops;++Commits;return ESP_OK;
}
void nvs_close(nvs_handle_t H) { assert(Open && H==1);Open=false; }
esp_err_t FPGA_Tx_CoreReload(uint8_t Op,uint8_t Target,uint16_t Nonce) {
 assert(!Open && !strcmp(Stored,Expected) && Commits && Target==ExpectedTarget && Nonce==0xabcd);
 assert(Op==(Sends?0xa2:0xa1));++Sends;return Sends==FailSend?ESP_FAIL:ESP_OK;
}
void vTaskDelay(TickType_t T) {
 assert(T==20 || T==5000);
 if(T==5000 && ResetOnReload)longjmp(ResetPoint,1);
}
static void Reset(void) {
 assert(!Open);Stored[0]=Staged[0]=0;Sends=FailSend=Commits=0;StoredHops=StagedHops=-1;Expected=GG;ExpectedTarget=1;
 FailSet=FailCommit=FailErase=ResetOnReload=false;CoreSwitch_ObserveVersion(50,1);
}
int main(void) {
 char Path[LAST_GAME_PATH_SIZE];Reset();
 assert(CoreSwitch_TakePending(Path)==ESP_ERR_NVS_NOT_FOUND && !Path[0]);
 assert(CoreSwitch_Request("/bad.gg")==ESP_ERR_INVALID_ARG && !Sends);
 CoreSwitch_ObserveVersion(18,38);assert(!CoreSwitch_IsSupported());
 assert(CoreSwitch_Request(GG)==ESP_ERR_NOT_SUPPORTED && !Sends);
 CoreSwitch_ObserveVersion(50,2);assert(CoreSwitch_Request(GG)==ESP_ERR_INVALID_STATE && !Sends);
 Reset();FailSet=true;assert(CoreSwitch_Request(GG)==ESP_FAIL && !Sends && !Stored[0]);
 Reset();FailCommit=true;assert(CoreSwitch_Request(GG)==ESP_FAIL && !Sends && !Stored[0]);
 Reset();FailSend=1;assert(CoreSwitch_Request(GG)==ESP_FAIL && Sends==1 && !Stored[0]);
 Reset();FailSend=2;assert(CoreSwitch_Request(GG)==ESP_FAIL && Sends==2 && !Stored[0]);
 Reset();assert(CoreSwitch_Request(GG)==ESP_ERR_TIMEOUT && Sends==2 && !Stored[0]);
 Reset();ResetOnReload=true;
 if(!setjmp(ResetPoint)) { (void)CoreSwitch_Request(GG);assert(false); }
 assert(Sends==2 && !strcmp(Stored,GG));
 CoreSwitch_ObserveVersion(50,2);assert(CoreSwitch_IsTarget(true));
 assert(CoreSwitch_TakePending(Path)==ESP_OK && !strcmp(Path,GG) && !Stored[0]);
 assert(CoreSwitch_TakePending(Path)==ESP_ERR_NVS_NOT_FOUND && !Path[0]);
 strcpy(Stored,GG);FailErase=true;assert(CoreSwitch_TakePending(Path)==ESP_FAIL && !Path[0] && Stored[0]);
 FailErase=false;FailCommit=true;assert(CoreSwitch_TakePending(Path)==ESP_FAIL && !Path[0] && Stored[0]);
 FailCommit=false;strcpy(Stored,"/bad.gg");assert(CoreSwitch_TakePending(Path)==ESP_ERR_INVALID_ARG && !Stored[0]);
 memset(Stored,'x',sizeof(Stored)-1);Stored[sizeof(Stored)-1]=0;
 assert(CoreSwitch_TakePending(Path)==ESP_ERR_INVALID_SIZE && !Stored[0] && !Path[0]);
 /* Lynx: GB -> GG -> Lynx (two reloads), Lynx -> GB (one), GG -> Lynx (one). */
 Reset();Expected=LYNX;ExpectedTarget=1;ResetOnReload=true;
 assert(CoreSwitch_SystemForPath(LYNX)==kCoreSystem_Lynx && CoreSwitch_SystemForPath("/x/a.LYX")==kCoreSystem_Lynx);
 if(!setjmp(ResetPoint)) { (void)CoreSwitch_Request(LYNX);assert(false); }
 assert(!strcmp(Stored,LYNX) && StoredHops==-1);
 CoreSwitch_ObserveVersion(50,2);  /* landed on Game Gear */
 uint8_t Hops=0;assert(CoreSwitch_TakePendingHops(Path,&Hops)==ESP_OK && Hops==1 && !Stored[0]);
 assert(!CoreSwitch_IsCore(kCoreSystem_Lynx));
 Sends=0;ExpectedTarget=0;
 if(!setjmp(ResetPoint)) { (void)CoreSwitch_Continue(Path,Hops);assert(false); }
 assert(!strcmp(Stored,LYNX) && StoredHops==2);
 CoreSwitch_ObserveVersion(50,3);assert(CoreSwitch_IsCore(kCoreSystem_Lynx));
 assert(CoreSwitch_TakePendingHops(Path,&Hops)==ESP_OK && Hops==2 && StoredHops==-1);
 /* hop budget: a third reload is refused (no loops) */
 assert(CoreSwitch_Continue(Path,2)==ESP_ERR_INVALID_STATE);
 CoreSwitch_ObserveVersion(50,3);Sends=0;Expected=GG;ExpectedTarget=0;
 if(!setjmp(ResetPoint)) { (void)CoreSwitch_Request(GG);assert(false); }
 assert(CoreSwitch_NextCore(kCoreSystem_Lynx)==kCoreSystem_GB && CoreSwitch_NextCore(kCoreSystem_GG)==kCoreSystem_Lynx);
 Stored[0]=0;StoredHops=-1;
 puts("PASS: durable handoff, reset consumption, no retry loops, NVS and UART failures");
}
