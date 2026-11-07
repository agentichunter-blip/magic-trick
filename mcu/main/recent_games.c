#include "recent_games.h"
#include "rom_title.h"
#include "nvs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef RECENT_GAMES_ROOT
#define RECENT_GAMES_ROOT "/sdcard/CHROMAGIC/BACKUPS"
#endif
static const char Root[] = RECENT_GAMES_ROOT "/";
enum { kHeaderSize = 5, kBlobSize = kHeaderSize + RECENT_GAMES_LIMIT * RECENT_GAME_PATH_SIZE };
static const char *Key(RecentSystem_t System) {
    return System==kRecentGB ? "gb" : System==kRecentGBC ? "gbc" : NULL;
}
RecentSystem_t RecentGames_System(const char *Path) {
    if (!Path || strnlen(Path,RECENT_GAME_PATH_SIZE)>=RECENT_GAME_PATH_SIZE ||
        strncmp(Path,Root,sizeof(Root)-1)) return kRecentNone;
    const char *Part=Path+sizeof(Root)-1;
    const size_t First=strcspn(Part,"/");
    RecentSystem_t System=First==2 && !strncasecmp(Part,"GB",2) ? kRecentGB :
        First==3 && !strncasecmp(Part,"GBC",3) ? kRecentGBC : kRecentNone;
    if (!System || !Part[First]) return kRecentNone;
    while (*Part) {
        size_t N=strcspn(Part,"/");
        if (!N || Part[0]=='.') return kRecentNone;
        if (!Part[N]) break;
        Part+=N+1;
        if (!*Part) return kRecentNone;
    }
    if (!strncasecmp(Part,"[BIOS]",6)) return kRecentNone;
    const char *Ext=strrchr(Part,'.');
    return Ext && (!strcasecmp(Ext,".gb") || !strcasecmp(Ext,".gbc")) ? System : kRecentNone;
}

esp_err_t RecentGames_Read(RecentSystem_t System, RecentGames_t *Games) {
    if (!Games || !Key(System)) return ESP_ERR_INVALID_ARG;
    memset(Games,0,sizeof(*Games));
    nvs_handle_t H;
    esp_err_t Result=nvs_open("recent_games",NVS_READONLY,&H);
    if (Result==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (Result!=ESP_OK) return Result;
    size_t Size=0;
    Result=nvs_get_blob(H,Key(System),NULL,&Size);
    uint8_t *Blob=NULL;
    if (Result==ESP_ERR_NVS_NOT_FOUND) Result=ESP_OK;
    else if (Result==ESP_OK) {
        if (Size<kHeaderSize || Size>kBlobSize) Result=ESP_ERR_INVALID_SIZE;
        else if (!(Blob=malloc(Size))) Result=ESP_ERR_NO_MEM;
        else Result=nvs_get_blob(H,Key(System),Blob,&Size);
    }
    nvs_close(H);
    if (Result==ESP_OK && Blob) {
        if (memcmp(Blob,"RC01",4) || Blob[4]>RECENT_GAMES_LIMIT) Result=ESP_ERR_INVALID_ARG;
        size_t At=kHeaderSize;
        for (unsigned I=0; Result==ESP_OK && I<Blob[4]; ++I) {
            size_t N=At<Size ? strnlen((char *)Blob+At,Size-At) : 0;
            if (!N || N>=Size-At || N+sizeof(Root)>RECENT_GAME_PATH_SIZE) { Result=ESP_ERR_INVALID_SIZE; break; }
            char *Path=Games->Paths[Games->Count];
            memcpy(Path,Root,sizeof(Root)-1); memcpy(Path+sizeof(Root)-1,Blob+At,N+1);
            if (RecentGames_System(Path)!=System) { Result=ESP_ERR_INVALID_ARG; break; }
            for (size_t J=0;J<Games->Count;++J)
                if (!strcasecmp(Path,Games->Paths[J])) { Result=ESP_ERR_INVALID_ARG; break; }
            if (Result==ESP_OK) ++Games->Count;
            At+=N+1;
        }
        if (Result==ESP_OK && At!=Size) Result=ESP_ERR_INVALID_SIZE;
    }
    free(Blob);
    if (Result!=ESP_OK) memset(Games,0,sizeof(*Games));
    return Result;
}

esp_err_t RecentGames_Remember(const char *Path) {
    const RecentSystem_t System=RecentGames_System(Path);
    /* Libraries outside the two system folders remain launchable. */
    if (!System || RomTitle_IsDemo(Path)) return ESP_OK;
    RecentGames_t *Games=malloc(sizeof(*Games));
    if (!Games) return ESP_ERR_NO_MEM;
    esp_err_t Result=RecentGames_Read(System,Games);
    if (Result==ESP_ERR_INVALID_SIZE || Result==ESP_ERR_INVALID_ARG) Result=ESP_OK;
    if (Result!=ESP_OK) { free(Games); return Result; }
    if (Games->Count && !strcasecmp(Games->Paths[0],Path)) { free(Games); return ESP_OK; }
    size_t Index=0;
    while (Index<Games->Count && strcasecmp(Games->Paths[Index],Path)) ++Index;
    if (Index==Games->Count && Games->Count<RECENT_GAMES_LIMIT) ++Games->Count;
    if (Index>=RECENT_GAMES_LIMIT) Index=RECENT_GAMES_LIMIT-1;
    memmove(Games->Paths[1],Games->Paths[0],Index*RECENT_GAME_PATH_SIZE);
    strcpy(Games->Paths[0],Path);
    uint8_t *Blob=malloc(kBlobSize);
    if (!Blob) { free(Games); return ESP_ERR_NO_MEM; }
    memcpy(Blob,"RC01",4); Blob[4]=(uint8_t)Games->Count;
    size_t Size=kHeaderSize;
    for (size_t I=0;I<Games->Count;++I) {
        const char *Relative=Games->Paths[I]+sizeof(Root)-1;
        const size_t N=strlen(Relative)+1; memcpy(Blob+Size,Relative,N); Size+=N;
    }
    const unsigned Count=(unsigned)Games->Count;
    free(Games);
    nvs_handle_t H;
    Result=nvs_open("recent_games",NVS_READWRITE,&H);
    if (Result==ESP_OK) {
        Result=nvs_set_blob(H,Key(System),Blob,Size);
        if (Result==ESP_OK) Result=nvs_commit(H);
        nvs_close(H);
    }
    free(Blob);
    if (Result==ESP_OK) printf("RECENT_GAMES stored system=%s count=%u\n",Key(System),Count);
    return Result;
}
