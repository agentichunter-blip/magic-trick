#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "boot_resume.h"
#include "core_switch.h"
#include "last_game.h"
#include "button.h"
#include "cart_link.h"
#include "cart_bulk.h"
#include "fpga_tx.h"
#include "nvs.h"
#include "freertos/task.h"

static char SwitchPath[LAST_GAME_PATH_SIZE];
static esp_err_t SwitchResult=ESP_ERR_NVS_NOT_FOUND;
static bool SwitchTarget=true;
esp_err_t CoreSwitch_TakePending(char *Path) { strcpy(Path,SwitchPath);SwitchPath[0]=0;return SwitchResult; }
bool CoreSwitch_IsTarget(bool GG) { (void)GG;return SwitchTarget; }
bool CoreSwitch_HasPending(void) { return SwitchPath[0]!=0; }
int64_t esp_timer_get_time(void) { return 0; }
void Button_ResetAll(void) {}
static const char Game[]="/sdcard/CHROMAGIC/BACKUPS/GB/0-F/Alleyway (World).gb";
static char Stored[600], Pending[600], StartedPath[LAST_GAME_PATH_SIZE];
static bool HandleOpen, ReadOnly, SetFailed, CommitFailed, OpenFailed;
static unsigned Sets, Commits;
esp_err_t nvs_open(const char *Name, nvs_open_mode_t Mode, nvs_handle_t *Handle) {
    assert(!strcmp(Name,"last_game") && !HandleOpen);
    if(OpenFailed) return ESP_FAIL;
    HandleOpen=true; ReadOnly=Mode==NVS_READONLY; *Handle=1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t H,const char *Key,char *Value,size_t *Size) {
    assert(H==1 && HandleOpen && !strcmp(Key,"path"));
    if(!Stored[0]) return ESP_ERR_NOT_FOUND;
    if(strlen(Stored)+1>*Size) return ESP_ERR_INVALID_SIZE;
    strcpy(Value,Stored); *Size=strlen(Stored)+1; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t H,const char *Key,const char *Value) {
    assert(H==1 && HandleOpen && !ReadOnly && !strcmp(Key,"path"));
    ++Sets; if(SetFailed) return ESP_FAIL; strcpy(Pending,Value); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t H) {
    assert(H==1 && HandleOpen && !ReadOnly); ++Commits;
    if(CommitFailed) return ESP_FAIL;
    strcpy(Stored,Pending); return ESP_OK;
}
void nvs_close(nvs_handle_t H) { assert(H==1 && HandleOpen); HandleOpen=false; }

static bool PC, PCActive, Visible, Active, Busy, Present, UnknownCart, BadReply;
static bool NoButtons, RequestFailed, FailLoad, FailStart, ManualMenu, ManualLoad;
static unsigned Starts, Pings, ButtonRequests, SleepLocks, MenuOpens, Sequence, Clock;
static unsigned BAt, BReleaseAt, InsertAt, AckAt;
static bool PulseRejected;
static uint16_t Held;
static bool Bulk, Entered, ReadFailed, ExitFailed, UnstableHeader;
static uint8_t EmptyValue;
static unsigned HeaderReads, Enters, Exits;
esp_err_t CartBulk_Begin(void) { assert(!Bulk); Bulk=true; return ESP_OK; }
void CartBulk_End(void) { assert(Bulk); Bulk=false; }
esp_err_t CartLink_Transaction(CartLinkOperation_t Op,uint16_t Address,uint8_t Value,CartLinkResponse_t *Reply) {
    assert(Bulk && Address==0 && Value==0);
    *Reply=(CartLinkResponse_t){0};
    if(Op==kCartLinkOp_Enter) { assert(!Entered); Entered=true; ++Enters; }
    else { assert(Op==kCartLinkOp_Exit); Entered=false; ++Exits; if(ExitFailed) return ESP_ERR_TIMEOUT; }
    return ESP_OK;
}
esp_err_t CartBulk_ReadBlock(uint16_t Address,uint8_t Data[kCartBulkBlockSize]) {
    assert(Bulk && Entered && Address==0); ++HeaderReads;
    if(ReadFailed) return ESP_FAIL;
    memset(Data,EmptyValue,kCartBulkBlockSize);
    if(Present || (UnstableHeader && HeaderReads%2==0)) Data[0x120]=0x42;
    return ESP_OK;
}
void PwrMgr_InhibitSleep(void) { ++SleepLocks; }
void PwrMgr_AllowSleep(void) { assert(SleepLocks); --SleepLocks; }
bool PCBackup_IsEnabled(void) { return PC; }
bool CartBackup_IsPCModeActive(void) { return PCActive; }
bool OSD_IsVisible(void) { return Visible; }
bool VirtualCart_IsActive(void) { return Active; }
bool VirtualCart_IsBusy(void) { return Busy; }
esp_err_t VirtualCart_Start(const char *Path) {
    assert((!Present || SwitchResult==ESP_OK) && !PC && !PCActive && !Busy && !Visible && !(Held & kButtonBits_B));
    ++Starts; strcpy(StartedPath,Path);
    if(FailStart) return ESP_ERR_NO_MEM;
    Busy=true; return ESP_OK;
}
void vTaskDelay(TickType_t Ticks) {
    Clock+=Ticks;
    if(Busy && Starts) { Busy=false; Active=!FailLoad; }
}
uint32_t Button_GetSnapshot(uint16_t *Out) { *Out=Held; return Sequence; }
esp_err_t FPGA_Tx_RequestButtons(void) {
    ++ButtonRequests;
    if(RequestFailed) return ESP_FAIL;
    if(!NoButtons) {
        ++Sequence;
        if(MenuOpens && ButtonRequests>=AckAt) Visible=true;
        Held=(ButtonRequests>=BAt && ButtonRequests<BReleaseAt ? kButtonBits_B : 0)
            | (Visible ? 0 : kButtonBits_MenuEn);
        if(ManualMenu) Visible=true;
        if(ManualLoad) Busy=true;
    }
    return ESP_OK;
}
esp_err_t FPGA_Tx_PulseButtons(uint16_t Buttons) {
    assert(Buttons==kButtonBits_MenuEnAlt && !Visible && !Busy && !Active && !PC && !PCActive);
    /* Model the real core: a held game button cancels a Menu toggle. */
    assert(!(Held & 0x1ffu));
    ++MenuOpens;
    if(PulseRejected) return ESP_FAIL;
    if(!AckAt) Visible=true;
    return ESP_OK;
}
esp_err_t CartLink_TransactionOnce(CartLinkOperation_t Op,uint16_t Address,uint8_t Value,CartLinkResponse_t *Reply) {
    assert(Op==kCartLinkOp_Ping && Address==0 && Value==0); ++Pings;
    if(UnknownCart) return ESP_ERR_TIMEOUT;
    if(Pings>=InsertAt) Present=true;
    *Reply=(CartLinkResponse_t){.Count=BadReply ? 3 : 4,.Data={0,2,3,10}};
    return ESP_OK;
}
static void Reset(void) {
    assert(!SleepLocks && !HandleOpen && !Bulk && !Entered);
    SwitchPath[0]=0;SwitchResult=ESP_ERR_NVS_NOT_FOUND;SwitchTarget=true;
    PC=PCActive=Visible=Active=Busy=Present=UnknownCart=BadReply=false;
    NoButtons=RequestFailed=FailLoad=FailStart=ManualMenu=ManualLoad=false;
    Starts=Pings=ButtonRequests=MenuOpens=Sequence=Clock=0; Held=0;
    BAt=InsertAt=~0u; BReleaseAt=15; AckAt=0; PulseRejected=false; ReadFailed=ExitFailed=UnstableHeader=false; EmptyValue=0xf0; HeaderReads=Enters=Exits=0; strcpy(Stored,Game); StartedPath[0]=0;
}
static void Run(void) {
    const BootResumeCartridge_t Cartridge=BootResume_CheckCartridge();
    BootResume_Run(&Cartridge);
}
static void Skipped(void) { Run(); assert(!Starts && !SleepLocks && !HandleOpen && !Bulk && !Entered); }
int main(void) {
    char Path[LAST_GAME_PATH_SIZE];
    assert(LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/GG/Game.GG"));
    assert(LastGame_Read(Path)==ESP_ERR_NOT_FOUND && !Path[0]);
    assert(LastGame_Remember(Game)==ESP_OK && Sets==1 && Commits==1);
    assert(LastGame_Read(Path)==ESP_OK && !strcmp(Path,Game));
    assert(LastGame_Remember(Game)==ESP_OK && Sets==1 && Commits==1);
    assert(!LastGame_ValidPath(NULL));
    assert(!LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/GBC/0-F/Donkey Kong Country (Demo) (Kiosk).gbc"));
    assert(LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/GB/Demolition Man (World).gb"));
    assert(!LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/../private.gb"));
    assert(!LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS//Game.gb"));
    assert(!LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/GB/./Game.gb"));
    assert(!LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/GB/Game.sav"));
    assert(!LastGame_ValidPath("/sdcard/OTHER/Game.gb"));
    assert(LastGame_ValidPath("/sdcard/CHROMAGIC/BACKUPS/GBC/Game.GBC"));
    memset(Path,'x',sizeof(Path)); assert(!LastGame_ValidPath(Path));
    SetFailed=true; assert(LastGame_Remember("/sdcard/CHROMAGIC/BACKUPS/New.gb")==ESP_FAIL);
    assert(!strcmp(Stored,Game) && Commits==1); SetFailed=false;
    CommitFailed=true; assert(LastGame_Remember("/sdcard/CHROMAGIC/BACKUPS/New.gb")==ESP_FAIL);
    assert(!strcmp(Stored,Game)); CommitFailed=false;
    OpenFailed=true; assert(LastGame_Read(Path)==ESP_FAIL && !Path[0]);
    assert(LastGame_Remember(Game)==ESP_FAIL); OpenFailed=false;

    Reset(); Run(); assert(Starts==1 && Active && !MenuOpens && !strcmp(StartedPath,Game));
    assert(Clock>=1000 && Pings==2 && ButtonRequests==11 && !SleepLocks);
    assert(HeaderReads==4 && Enters==2 && Exits==2);
    Reset(); EmptyValue=0xff; Run(); assert(Starts==1 && Active);
    Reset(); EmptyValue=0; Run(); assert(Starts==1 && Active);
    Reset(); EmptyValue=0xaa; Skipped(); assert(!MenuOpens && HeaderReads==1);
    Reset(); UnstableHeader=true; Skipped(); assert(!MenuOpens);
    Reset(); ReadFailed=true; Skipped(); assert(MenuOpens==1 && Enters==Exits);
    Reset(); ExitFailed=true; Skipped(); assert(MenuOpens==1 && Enters==Exits);
    Reset(); Present=true; Skipped(); assert(!MenuOpens && !ButtonRequests);
    Reset(); InsertAt=2; Skipped(); assert(Present && !MenuOpens);
    Reset(); UnknownCart=true; Skipped(); assert(MenuOpens==1 && Clock<=1000);
    Reset(); BadReply=true; Skipped(); assert(MenuOpens==1);
    Reset(); BAt=1; Skipped(); assert(MenuOpens==1);
    Reset(); BAt=8; Skipped(); assert(MenuOpens==1);
    Reset(); BAt=11; Skipped(); assert(MenuOpens==1);
    Reset(); BAt=1; BReleaseAt=200; Skipped(); assert(MenuOpens==1 && Visible && ButtonRequests>BReleaseAt && Clock>=3980);
    Reset(); BAt=1; AckAt=25; Skipped(); assert(MenuOpens==1 && Visible && ButtonRequests>=AckAt);
    Reset(); BAt=1; AckAt=~0u; Skipped(); assert(MenuOpens==1 && !Visible);
    Reset(); BAt=1; PulseRejected=true; Skipped(); assert(MenuOpens==1 && !Visible);
    Reset(); NoButtons=true; Skipped(); assert(!MenuOpens && Clock<=1000);
    Reset(); RequestFailed=true; Skipped(); assert(!MenuOpens);
    Reset(); PC=true; Skipped(); assert(!Pings && !MenuOpens);
    Reset(); PCActive=true; Skipped(); assert(!Pings && !MenuOpens);
    Reset(); Visible=true; Skipped(); assert(!Pings && !MenuOpens);
    Reset(); ManualMenu=true; Skipped(); assert(!MenuOpens);
    Reset(); ManualLoad=true; Skipped(); assert(!MenuOpens);
    Reset(); Stored[0]=0; Skipped(); assert(MenuOpens==1);
    Reset(); strcpy(Stored,"/sdcard/CHROMAGIC/BACKUPS/GB/Game (Demo).gb"); Skipped(); assert(MenuOpens==1 && !Starts);
    Reset(); strcpy(Stored,"/other/Game.gb"); Skipped(); assert(MenuOpens==1);
    Reset(); memset(Stored,'x',sizeof(Stored)-1); Stored[sizeof(Stored)-1]=0; Skipped(); assert(MenuOpens==1);
    Reset(); FailStart=true; Run(); assert(Starts==1 && MenuOpens==1 && !SleepLocks);
    Reset(); FailLoad=true; Run(); assert(Starts==1 && MenuOpens==1 && !SleepLocks);
    Reset();strcpy(SwitchPath,"/sdcard/CHROMAGIC/BACKUPS/GG/Test.gg");SwitchResult=ESP_OK;Present=true;
    Run();assert(Starts==1 && Active && !SwitchPath[0] && !strcmp(StartedPath,"/sdcard/CHROMAGIC/BACKUPS/GG/Test.gg"));
    Reset();strcpy(SwitchPath,Game);SwitchResult=ESP_OK;SwitchTarget=false;
    Skipped();assert(MenuOpens==1 && !SwitchPath[0]);
    Reset();SwitchResult=ESP_FAIL;Skipped();assert(MenuOpens==1);
    /* A physical game may already be running after UI setup. Use the early
     * result without entering maintenance/reset again, even if the bus would
     * now fail. This reproduces the former late Tetris copyright-screen reset. */
    Reset(); Present=true;
    BootResumeCartridge_t Cartridge=BootResume_CheckCartridge();
    assert(Cartridge.Result==ESP_OK && Cartridge.Present && Enters==1 && Exits==1);
    vTaskDelay(2000); ReadFailed=true;
    BootResume_Run(&Cartridge);
    assert(Pings==1 && Enters==1 && HeaderReads==1 && Exits==1);
    assert(!Starts && !MenuOpens && !ButtonRequests && !SleepLocks);

    /* Don't retry an unsuccessful early probe late into cartridge startup. */
    Reset(); UnknownCart=true; Cartridge=BootResume_CheckCartridge();
    assert(Cartridge.Result==ESP_ERR_TIMEOUT && Pings==5);
    UnknownCart=false; Present=true; BootResume_Run(&Cartridge);
    assert(Pings==5 && !Enters && !Starts && MenuOpens==1 && !SleepLocks);
    Reset(); BootResume_Run(NULL); assert(!Pings && !Starts && MenuOpens==1 && !SleepLocks);

    /* Manual/PC activity that starts between the phases keeps priority. */
    Reset(); Cartridge=BootResume_CheckCartridge(); PC=true;
    BootResume_Run(&Cartridge); assert(Pings==1 && Enters==1 && !Starts && !MenuOpens && !SleepLocks);
    Reset(); Cartridge=BootResume_CheckCartridge(); Visible=true;
    BootResume_Run(&Cartridge); assert(Pings==1 && Enters==1 && !Starts && !MenuOpens && !SleepLocks);
    Reset(); PC=true; Cartridge=BootResume_CheckCartridge(); PC=false;
    BootResume_Run(&Cartridge); assert(!Pings && !Enters && !Starts && MenuOpens==1 && !SleepLocks);

    puts("PASS: persistent last game; early cartridge probe avoids late reset; cartridge priority and recheck; fresh B bypass; missing/corrupt settings; failed load fallback; PC/manual interaction priority");
}
