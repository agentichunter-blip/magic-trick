#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../main/virtual_cart.c"

static bool Mounted, Bulk, Started;
static bool GGCore, GGNoFrame, GGReset, GGShortReply;
/* Game Gear battery saves: a core reporting support (status bit 6), the
 * import armed by command 17 before start, the staged save and mirror repair. */
static bool GGCartCore, GGArmed, GGImportFail;
static unsigned GGStageWrites, GGMirrorWrites;
static uint8_t GGStaged[32768];
static unsigned Writes, Prepares, Starts, GGFailWrite;
static FILE *ExpectedRom;
static uint32_t ExpectedRomSize;

static bool SwitchCapable;
static unsigned Switches;
bool CoreSwitch_IsSupported(void) { return SwitchCapable; }
bool CoreSwitch_IsTarget(bool GG) { return SwitchCapable && GG==GGCore; }
esp_err_t CoreSwitch_Request(const char *Path) {
 assert(Path && !Mounted && !Bulk && !Active && !Started);++Switches;return ESP_ERR_TIMEOUT;
}
esp_err_t GGCheats_Install(const CheatPlan_t *Plan) {
 assert(Plan && Bulk && Mounted && !Started && !Active);return ESP_OK;
}
static bool UseCheats, FailCheats, SawPatch;
esp_err_t Cheats_Prepare(const char *Path,FILE *Rom,CheatPlan_t *Plan) {
    assert(Path && Rom && Mounted && !Bulk); *Plan=(CheatPlan_t){0};
    if(FailCheats) return ESP_ERR_INVALID_CRC;
    if(UseCheats) { Plan->Patches=malloc(sizeof(*Plan->Patches)); assert(Plan->Patches); Plan->Count=1; Plan->Patches[0]=(CheatPatch_t){0x200,0,42}; }
    return ESP_OK;
}
static unsigned Completed, Errors, Closed, Remembered, RecentRemembered;
static esp_err_t RecentError;
esp_err_t RecentGames_Remember(const char *Path) { assert(Path && Active && !Mounted && !Bulk); ++RecentRemembered; return RecentError; }
esp_err_t LastGame_Remember(const char *Path) { assert(Path && Active && !Mounted && !Bulk); ++Remembered; return ESP_OK; }
static esp_err_t UnmountResult;
static sdmmc_card_t Card;
static unsigned SleepLocks;
void PwrMgr_InhibitSleep(void) { ++SleepLocks; }
void PwrMgr_AllowSleep(void) { assert(SleepLocks); --SleepLocks; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelay(TickType_t Ticks) { (void)Ticks; }
esp_err_t SDCard_Mount(sdmmc_card_t **Out) { assert(!Mounted); Mounted=true; *Out=&Card; return ESP_OK; }
esp_err_t SDCard_Unmount(sdmmc_card_t *Value) { assert(Mounted && Value==&Card); Mounted=false; return UnmountResult; }
esp_err_t CartBulk_Begin(void) { assert(Mounted && !Bulk); Bulk=true; return ESP_OK; }
void CartBulk_End(void) { assert(Bulk); Bulk=false; }
esp_err_t CartBulk_WritePSRAMBlock(uint32_t Address, const uint8_t *Data) {
 assert(Bulk);
 if(GGCore && Writes>=4096) {
  /* After the ROM: the staged battery save (before start), then the ROM
   * mirror under it put back (after start). */
  const uint32_t Cart=kRomBase+kGGStateRomAddress+kGGCartFirstBlock*kCartBulkBlockSize;
  if(!Started) {
   assert(GGCartCore && Address>=Cart && Address<Cart+sizeof(GGStaged) && (Address-Cart)%kCartBulkBlockSize==0);
   memcpy(GGStaged+(Address-Cart),Data,kCartBulkBlockSize);++GGStageWrites;
  } else {
   assert(Address>=kRomBase+kGGStateRomAddress && Address<kRomBase+kGGStateRomAddress+kStateFileGameGearPayload);
   uint8_t expected[kCartBulkBlockSize];
   assert(!fseek(ExpectedRom,(Address-kRomBase)%ExpectedRomSize,SEEK_SET));
   assert(fread(expected,1,sizeof(expected),ExpectedRom)==sizeof(expected) && !memcmp(expected,Data,sizeof(expected)));
   ++GGMirrorWrites;
  }
  return ESP_OK;
 }
 if(GGCore) {
  assert(Address==kRomBase+Writes*kCartBulkBlockSize && Address+kCartBulkBlockSize<=kSaveBase);
  uint8_t expected[kCartBulkBlockSize];
  assert(ExpectedRom && !fseek(ExpectedRom,(Address-kRomBase)%ExpectedRomSize,SEEK_SET));
  assert(fread(expected,1,sizeof(expected),ExpectedRom)==sizeof(expected));
  assert(!memcmp(expected,Data,sizeof(expected)));
  ++Writes;
  return GGFailWrite && Writes==GGFailWrite ? ESP_FAIL : ESP_OK;
 }
 ++Writes; if(Address==kRomBase) { assert(Data[0x200]==(UseCheats?42:0)); SawPatch=UseCheats; } return ESP_OK; }
esp_err_t CartBulk_ReadVirtualBlock(uint16_t Sequence, uint8_t *Data) { (void)Sequence; (void)Data; assert(false); return ESP_FAIL; }
esp_err_t CartLink_VirtualCommand(uint8_t Command, uint16_t Config, uint8_t High, CartLinkResponse_t *Out) {
    (void)Config; (void)High;
    if(Command==kVirtualCommandPrepare) ++Prepares;
    if(Command==kVirtualCommandStart) ++Starts;
    if(Command==kVirtualCommandPrepare || Command==kVirtualCommandStop) { Started=false; GGArmed=false; }
    if(GGCore && Command==kVirtualCommandGGCartImport) { assert(GGCartCore && !Started && High==1); GGArmed=true; }
    if(GGCore && GGCartCore && Command==12) {
        const uint32_t S=0x53530048u | (Started && GGArmed && !GGImportFail ? 0x80u : 0u);
        memset(Out,0,sizeof(*Out)); Out->Count=4;
        for(int I=0;I<4;I++) Out->Data[I]=(uint8_t)(S>>(8*I));
        return ESP_OK;
    }
    if(Command==kVirtualCommandStart) Started=true;
    memset(Out,0,sizeof(*Out)); Out->Count=4;
    Out->Data[0]=Started ? 0xff : kVirtualStatusDisableSeen|kVirtualStatusResetAsserted;
    Out->Data[1]=Started ? kVirtualStatusPostExitFrame>>8 : 0;
    if(GGCore) {
        Out->Data[3]=0x47;Out->Data[2]=1;
        Out->Data[0]=Started ? 0x3b : 0x18;
        Out->Data[1]=Started ? (GGReset ? 0x20 : GGNoFrame ? 0 : 0x40) : 0;
        if(GGShortReply) Out->Count=3;
    }
    return ESP_OK;
}
void RomBrowserUI_SetProgress(uint32_t Value, uint32_t Total) { assert(Value<=Total); }
bool RomBrowserUI_SetComplete(void) { assert(!Mounted && !Bulk); ++Completed; return true; }
void RomBrowserUI_SetError(esp_err_t Error) { assert(Error!=ESP_OK && !Mounted && !Bulk); ++Errors; }
esp_err_t RomBrowser_CloseMenu(void) { assert(Completed && !Mounted && !Bulk); ++Closed; return ESP_OK; }

int main(void) {
    char Path[]="/tmp/chromagic-load-XXXXXX"; int FD=mkstemp(Path); assert(FD>=0);
    uint8_t ROM[32768]={0}; memcpy(ROM+0x104,NintendoLogo,sizeof(NintendoLogo));
    memcpy(ROM+0x134,"LOAD TEST",9);
    for(unsigned I=0x134;I<=0x14c;++I) ROM[0x14d]=(uint8_t)(ROM[0x14d]-ROM[I]-1);
    uint16_t Sum=0; for(unsigned I=0;I<sizeof(ROM);++I) Sum+=ROM[I]; ROM[0x14e]=Sum>>8; ROM[0x14f]=Sum;
    FILE *File=fdopen(FD,"wb"); assert(File); assert(fwrite(ROM,1,sizeof(ROM),File)==sizeof(ROM)); assert(!fclose(File));
    assert(RunLoad(Path)==ESP_OK);
    assert(Completed==1 && Closed==1 && Errors==0 && Active && Remembered==1 && RecentRemembered==1);
    SetActive(false); ClearActiveSave(); Completed=Closed=Errors=0;
    UnmountResult=ESP_FAIL;
    assert(RunLoad(Path)==ESP_FAIL);
    assert(Completed==0 && Closed==0 && Errors==1 && !Active && Remembered==1 && RecentRemembered==1);
    assert(!Mounted && !Bulk && !Started);
    assert(!SleepLocks);
    UnmountResult=ESP_OK; UseCheats=true; assert(RunLoad(Path)==ESP_OK && SawPatch);
    File=fopen(Path,"rb"); assert(File && !fseek(File,0x200,SEEK_SET) && fgetc(File)==0); assert(!fclose(File));
    unsigned OldRemembered=Remembered; FailCheats=true;
    assert(RunLoad(Path)==ESP_ERR_INVALID_CRC && Active && Remembered==OldRemembered && RecentRemembered==OldRemembered);
    SetActive(false); ClearActiveSave();
    FailCheats=false; RecentError=ESP_FAIL; unsigned OldCompleted=Completed;
    assert(RunLoad(Path)==ESP_OK && Active && Completed==OldCompleted+1); /* history failure must not fail a game */
    SetActive(false);ClearActiveSave();GGCore=true;SwitchCapable=true;
    unsigned BeforeWrites=Writes,BeforePrepares=Prepares;
    assert(RunLoad(Path)==ESP_ERR_TIMEOUT && Switches==1 && Writes==BeforeWrites && Prepares==BeforePrepares);
    UnmountResult=ESP_FAIL;
    assert(RunLoad(Path)==ESP_FAIL && Switches==1 && !Mounted && !Bulk);
    assert(!unlink(Path));
    puts("PASS: load success follows SD cleanup; cleanup errors retain the menu and stop the failed launch");
    return 0;
}
