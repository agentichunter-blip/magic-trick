#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cheats.h"
#include "sd_card.h"
#include "nvs.h"
static uint8_t ROM[32768], Blob[48], Pending[48];
static bool Stored, NvsFail, Mounted, UnmountFail;
static unsigned Writes;
static sdmmc_card_t Card;
static void Put(uint8_t *P,uint32_t V) { for(unsigned I=0;I<4;++I) P[I]=V>>(I*8); }
esp_err_t SDCard_Mount(sdmmc_card_t **Out) { assert(!Mounted); Mounted=true; *Out=&Card; return ESP_OK; }
esp_err_t SDCard_Unmount(sdmmc_card_t *C) { assert(Mounted && C==&Card); Mounted=false; return UnmountFail?ESP_FAIL:ESP_OK; }
esp_err_t nvs_open(const char *N,nvs_open_mode_t M,nvs_handle_t *H) { assert(!strcmp(N,"cheats")); *H=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t H,const char *Key,void *B,size_t *Size) { assert(H==1 && Key[0]=='c' && *Size>=48); if(!Stored) return ESP_ERR_NVS_NOT_FOUND; memcpy(B,Blob,48); *Size=48; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t H,const char *Key,const void *B,size_t Size) { assert(H==1 && Key[0]=='c' && Size==48 && !Mounted); memcpy(Pending,B,48); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t H) { assert(H==1); if(NvsFail) return ESP_FAIL; memcpy(Blob,Pending,48); Stored=true; ++Writes; return ESP_OK; }
void nvs_close(nvs_handle_t H) { assert(H==1); }
static const char *Rom=CHEAT_ROM_ROOT "Game.gb", *Pack=CHEAT_PACK_ROOT "Game.gb.cmc";
static void Write(const char *Path,const void *Data,size_t N) { FILE *F=fopen(Path,"wb"); assert(F && fwrite(Data,1,N,F)==N && !fclose(F)); }
static bool GGPack;
static void MakePack(void) {
    uint8_t B[32+3*104+3*8]={0}; memcpy(B,"CMCH",4); Put(B+4,GGPack?2:1); Put(B+8,sizeof(ROM)); Put(B+12,Cheats_CRC32(0,ROM,sizeof(ROM))); Put(B+16,3); Put(B+20,3);
    const char *Names[]={"Infinite lives","Start with 99 lives","Infinite health"};
    for(unsigned I=0;I<3;++I) { uint8_t *E=B+32+104*I; strcpy((char *)E,Names[I]); Put(E+96,I); E[100]=1; uint8_t *P=B+32+3*104+8*I; Put(P,I==2?0x2400:0x200); P[4]=0; P[5]=I+1;P[6]=GGPack?1:0; }
    Put(B+24,Cheats_CRC32(0,B+32,sizeof(B)-32)); Write(Pack,B,sizeof(B));
}
int main(void) {
    assert(Cheats_CRC32(0,"123456789",9)==0xcbf43926);
    assert(Cheats_CRC32(Cheats_CRC32(0,"1234",4),"56789",5)==0xcbf43926);
    mkdir("cheat-fixture",0700); mkdir(CHEAT_ROM_ROOT,0700); mkdir(CHEAT_PACK_ROOT,0700);
    Write(Rom,ROM,sizeof(ROM)); unlink(Pack);
    CheatPlan_t P; FILE *F=fopen(Rom,"rb"); assert(F);
    assert(Cheats_Prepare(Rom,F,&P)==ESP_OK && !P.Count); Cheats_Free(&P);
    CheatBadge_t Badge={true,true};
    assert(Cheats_Badge(Rom,&Badge)==ESP_OK && !Badge.Available && !Badge.Enabled);
    MakePack(); CheatView_t V;
    assert(Cheats_Badge(Rom,&Badge)==ESP_OK && Badge.Available && !Badge.Enabled && !Writes);
    assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_OK && V.Count==4 && V.Enabled==0);
    assert(Cheats_Prepare(Rom,F,&P)==ESP_OK && !P.Count); Cheats_Free(&P);
    assert(Cheats_Menu(kCheatToggle,NULL,1,&V)==ESP_OK && V.Enabled==1 && Writes==1);
    assert(Cheats_Badge(Rom,&Badge)==ESP_OK && Badge.Available && Badge.Enabled && Writes==1);
    Cheats_Close(); assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_OK && V.Enabled==1);
    assert(!fseek(F,123,SEEK_SET)); assert(Cheats_Prepare(Rom,F,&P)==ESP_OK && P.Count==1 && ftell(F)==123);
    uint8_t Data[1024]={0}; assert(Cheats_Apply(&P,0,Data,sizeof(Data))==ESP_OK && Data[0x200]==1);
    assert(Cheats_Apply(&P,0,Data,sizeof(Data))==ESP_ERR_INVALID_CRC); Cheats_Free(&P);
    assert(Cheats_Menu(kCheatToggle,NULL,2,&V)==ESP_ERR_INVALID_STATE && Writes==1);
    assert(Cheats_Menu(kCheatView,NULL,1,&V)==ESP_OK && V.Enabled==1);
    UnmountFail=true; assert(Cheats_Menu(kCheatToggle,NULL,3,&V)==ESP_FAIL && Writes==1); UnmountFail=false;
    assert(Cheats_Menu(kCheatView,NULL,1,&V)==ESP_OK && V.Enabled==1);
    NvsFail=true; assert(Cheats_Menu(kCheatToggle,NULL,3,&V)==ESP_FAIL); NvsFail=false;
    assert(Cheats_Menu(kCheatView,NULL,1,&V)==ESP_OK && V.Enabled==1);
    assert(Cheats_Menu(kCheatToggle,NULL,3,&V)==ESP_OK && V.Enabled==2);
    assert(Cheats_Prepare(Rom,F,&P)==ESP_OK && P.Count==2 && P.Patches[0].Offset==0x200 && P.Patches[1].Offset==0x2400); Cheats_Free(&P);
    assert(Cheats_Menu(kCheatDisableAll,NULL,0,&V)==ESP_OK && V.Enabled==0);
    unsigned Prior=Writes; assert(Cheats_Menu(kCheatDisableAll,NULL,0,&V)==ESP_OK && Writes==Prior);
    assert(Cheats_Menu(kCheatToggle,NULL,1,&V)==ESP_OK);
    Blob[44]^=1; assert(Cheats_Prepare(Rom,F,&P)==ESP_ERR_INVALID_CRC && !P.Count); Blob[44]^=1;
    assert(!fclose(F)); ROM[0x200]=99; Write(Rom,ROM,sizeof(ROM)); F=fopen(Rom,"rb"); assert(F);
    assert(Cheats_Prepare(Rom,F,&P)==ESP_ERR_INVALID_CRC && !P.Count); assert(!fclose(F));
    assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_ERR_INVALID_CRC);
    ROM[0x200]=0; Write(Rom,ROM,sizeof(ROM));
    FILE *Bad=fopen(Pack,"r+b"); assert(Bad && !fseek(Bad,-1,SEEK_END) && fputc(1,Bad)!=EOF && !fclose(Bad));
    assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_ERR_INVALID_CRC);
    MakePack(); Bad=fopen(Pack,"r+b"); assert(Bad); uint8_t Huge[4]={255,255,255,255}; assert(!fseek(Bad,16,SEEK_SET) && fwrite(Huge,1,4,Bad)==4 && !fclose(Bad));
    assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_ERR_INVALID_SIZE);
    assert(Cheats_Badge(Rom,&Badge)==ESP_ERR_INVALID_SIZE && !Badge.Available);
    assert(!Mounted); unlink(Pack); unlink(Rom);
    Rom=CHEAT_ROM_ROOT "Game.gg";Pack=CHEAT_PACK_ROOT "Game.gg.cmc";GGPack=true;Stored=false;
    Write(Rom,ROM,sizeof(ROM));MakePack();
    assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_OK);
    assert(Cheats_Menu(kCheatToggle,NULL,1,&V)==ESP_OK);
    F=fopen(Rom,"rb");assert(F);
    assert(Cheats_Prepare(Rom,F,&P)==ESP_OK && P.GameGear && P.Count==1 && P.Patches[0].Compare);
    memset(Data,0,sizeof(Data));assert(Cheats_Apply(&P,0,Data,sizeof(Data))==ESP_ERR_NOT_SUPPORTED && Data[0x200]==0);
    Cheats_Free(&P);fclose(F);
    GGPack=false;MakePack();assert(Cheats_Menu(kCheatOpen,Rom,0,&V)==ESP_ERR_INVALID_ARG);
    unlink(Pack);unlink(Rom);
    puts("PASS: exact ROM identity, grouped/conflicting patches, persistent toggles, disable all, failed commit, corrupt packs/settings, original ROM unchanged");
}
