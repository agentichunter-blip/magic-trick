#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "recent_games.h"
#include "nvs.h"

static unsigned char Saved[2][6500], Pending[6500];
static size_t Sizes[2], PendingSize;
static unsigned Selected, Sets, Commits;
static bool Open, ReadOnly, FailSet, FailCommit, FailOpen;
esp_err_t nvs_open(const char *Name,nvs_open_mode_t Mode,nvs_handle_t *H) {
    assert(!strcmp(Name,"recent_games") && !Open);
    if(FailOpen) return ESP_FAIL;
    Open=true; ReadOnly=Mode==NVS_READONLY; *H=1; return ESP_OK;
}
static unsigned Key(const char *Name) { assert(!strcmp(Name,"gb") || !strcmp(Name,"gbc")); return !strcmp(Name,"gbc"); }
esp_err_t nvs_get_blob(nvs_handle_t H,const char *Name,void *Out,size_t *Size) {
    assert(Open && H==1); unsigned I=Key(Name);
    if(!Sizes[I]) return ESP_ERR_NVS_NOT_FOUND;
    if(!Out) { *Size=Sizes[I]; return ESP_OK; }
    if(*Size<Sizes[I]) return ESP_ERR_INVALID_SIZE;
    memcpy(Out,Saved[I],Sizes[I]); *Size=Sizes[I]; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t H,const char *Name,const void *Data,size_t Size) {
    assert(Open && H==1 && !ReadOnly && Size<=sizeof(Pending)); ++Sets;
    if(FailSet) return ESP_FAIL;
    Selected=Key(Name); memcpy(Pending,Data,Size); PendingSize=Size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t H) {
    assert(Open && H==1 && !ReadOnly); ++Commits;
    if(FailCommit) return ESP_FAIL;
    memcpy(Saved[Selected],Pending,PendingSize); Sizes[Selected]=PendingSize; return ESP_OK;
}
void nvs_close(nvs_handle_t H) { assert(Open && H==1); Open=false; }
static RecentGames_t Games;
static const char Root[]="/sdcard/CHROMAGIC/BACKUPS/";
int main(void) {
    assert(RecentGames_Read(kRecentGB,&Games)==ESP_OK && !Games.Count && !Sets);
    char Path[320];
    for(unsigned I=0;I<25;++I) {
        snprintf(Path,sizeof(Path),"%sGB/0-F/Game %02u.gb",Root,I);
        assert(RecentGames_Remember(Path)==ESP_OK);
    }
    assert(Sets==25 && Commits==25);
    assert(RecentGames_Read(kRecentGB,&Games)==ESP_OK && Games.Count==20);
    assert(strstr(Games.Paths[0],"Game 24.gb") && strstr(Games.Paths[19],"Game 05.gb"));
    assert(RecentGames_Remember(Path)==ESP_OK && Sets==25); /* no redundant flash write */
    assert(RecentGames_Remember("/sdcard/CHROMAGIC/BACKUPS/GB/0-F/Game 10.gb")==ESP_OK);
    assert(RecentGames_Read(kRecentGB,&Games)==ESP_OK && Games.Count==20);
    assert(strstr(Games.Paths[0],"Game 10.gb") && strstr(Games.Paths[1],"Game 24.gb"));
    assert(RecentGames_Remember("/sdcard/CHROMAGIC/BACKUPS/gb/0-f/GAME 10.GB")==ESP_OK && Sets==26);
    assert(RecentGames_Remember("/sdcard/CHROMAGIC/BACKUPS/GBC/M-R/Color.gbc")==ESP_OK);
    assert(RecentGames_Read(kRecentGBC,&Games)==ESP_OK && Games.Count==1);
    assert(strstr(Games.Paths[0],"Color.gbc"));
    assert(RecentGames_Read(kRecentGB,&Games)==ESP_OK && Games.Count==20);
    const char *Bad[]={NULL,"/elsewhere/GB/Game.gb","/sdcard/CHROMAGIC/BACKUPS/GB/../Game.gb",
        "/sdcard/CHROMAGIC/BACKUPS/GB/.hidden/Game.gb","/sdcard/CHROMAGIC/BACKUPS/GB//Game.gb",
        "/sdcard/CHROMAGIC/BACKUPS/GB/Game.sav","/sdcard/CHROMAGIC/BACKUPS/GB/[BIOS] Boot.gb"};
    for(unsigned I=0;I<sizeof(Bad)/sizeof(*Bad);++I) assert(RecentGames_System(Bad[I])==kRecentNone);
    unsigned Writes=Sets;
    assert(RecentGames_Remember("/sdcard/CHROMAGIC/BACKUPS/GB/Game (Demo).gb")==ESP_OK && Sets==Writes);
    assert(RecentGames_Remember("/sdcard/CHROMAGIC/BACKUPS/Other/Game.gb")==ESP_OK && Sets==Writes);
    FailSet=true; assert(RecentGames_Remember(Path)==ESP_FAIL); FailSet=false;
    FailCommit=true; assert(RecentGames_Remember(Path)==ESP_FAIL); FailCommit=false;
    assert(RecentGames_Read(kRecentGB,&Games)==ESP_OK && strstr(Games.Paths[0],"Game 10.gb"));
    FailOpen=true; assert(RecentGames_Remember(Path)==ESP_FAIL); FailOpen=false;
    /* Truncation, bad version/count, trailing data and cross-system paths fail closed. */
    size_t GoodSize=Sizes[0]; unsigned char Good[6500]; memcpy(Good,Saved[0],GoodSize);
    Sizes[0]=4; assert(RecentGames_Read(kRecentGB,&Games)==ESP_ERR_INVALID_SIZE && !Games.Count);
    Sizes[0]=GoodSize; Saved[0][0]='X'; assert(RecentGames_Read(kRecentGB,&Games)!=ESP_OK && !Games.Count);
    memcpy(Saved[0],Good,GoodSize); Saved[0][4]=21; assert(RecentGames_Read(kRecentGB,&Games)!=ESP_OK && !Games.Count);
    memcpy(Saved[0],Good,GoodSize); Sizes[0]=GoodSize-1; assert(RecentGames_Read(kRecentGB,&Games)!=ESP_OK && !Games.Count);
    memcpy(Saved[0],Saved[1],Sizes[1]); Sizes[0]=Sizes[1]; assert(RecentGames_Read(kRecentGB,&Games)!=ESP_OK && !Games.Count);
    assert(RecentGames_Remember(Path)==ESP_OK); /* a successful launch repairs corrupt history */
    assert(RecentGames_Read(kRecentGB,&Games)==ESP_OK && Games.Count==1 && !strcmp(Games.Paths[0],Path));
    assert(!Open);
    puts("PASS: persistent separate 20-game MRU histories; deduplication; bounded parsing; failed writes; no browsing writes");
}
