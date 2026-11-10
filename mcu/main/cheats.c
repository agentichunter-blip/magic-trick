#include "cheats.h"
#include "sd_card.h"
#include "nvs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef CHEAT_ROM_ROOT
#define CHEAT_ROM_ROOT "/sdcard/CHROMAGIC/BACKUPS/"
#endif
#ifndef CHEAT_PACK_ROOT
#define CHEAT_PACK_ROOT "/sdcard/CHROMAGIC/CHEATS/"
#endif
/* CMCH v1: LE header, ROM size/CRC32, entry count, patch count, payload CRC32.
 * Each entry has 96-byte ASCII name, first patch u32, patch count u16, zero u16.
 * Each patch has physical ROM offset u32, original byte, replacement, zero u16.
 * Static ROM patches only: no RAM writes, no changes to the ROM on SD. */
typedef struct { uint32_t Size, RomCRC, Count, Patches, DataCRC, Version; } Header_t;
static struct { char Path[352]; Header_t Header; uint8_t Bits[CHEAT_SELECTION_BYTES]; bool Open; } Menu;
static uint16_t U16(const uint8_t *P) { return P[0] | (uint16_t)P[1]<<8; }
static uint32_t U32(const uint8_t *P) { return U16(P) | (uint32_t)U16(P+2)<<16; }
static void Put32(uint8_t *P,uint32_t V) { for(unsigned I=0;I<4;++I) P[I]=(uint8_t)(V>>(I*8)); }
static bool PackPath(const char *Rom,char *Path,size_t Size) {
    const size_t Root=strlen(CHEAT_ROM_ROOT);
    if(!Rom || strncmp(Rom,CHEAT_ROM_ROOT,Root) || !Rom[Root]) return false;
    const char *P=Rom+Root;
    while(*P) { size_t N=strcspn(P,"/"); if(!N || (N==1 && P[0]=='.') || (N==2 && P[0]=='.' && P[1]=='.')) return false; P+=N; if(*P) ++P; }
    int N=snprintf(Path,Size,"%s%s.cmc",CHEAT_PACK_ROOT,Rom+Root);
    return N>0 && (size_t)N<Size;
}
static esp_err_t ReadAt(FILE *F,long Offset,void *Data,size_t Size) {
    return fseek(F,Offset,SEEK_SET) || fread(Data,1,Size,F)!=Size ? ESP_FAIL : ESP_OK;
}
static esp_err_t Header(FILE *F,Header_t *H,bool Verify) {
    uint8_t B[CHEAT_HEADER_SIZE];
    if(ReadAt(F,0,B,sizeof(B))!=ESP_OK || memcmp(B,"CMCH",4) || (U32(B+4)!=1 && U32(B+4)!=2) || U32(B+28)) return ESP_ERR_INVALID_ARG;
    *H=(Header_t){U32(B+8),U32(B+12),U32(B+16),U32(B+20),U32(B+24),U32(B+4)};
    if(H->Size<(H->Version==2 ? 8192 : 32768) || H->Size>8*1024*1024 || !H->Count || H->Count>CHEAT_MAX_ENTRIES || !H->Patches || H->Patches>65536) return ESP_ERR_INVALID_SIZE;
    const long End=CHEAT_HEADER_SIZE+H->Count*CHEAT_ENTRY_SIZE+H->Patches*CHEAT_PATCH_SIZE;
    if(fseek(F,0,SEEK_END) || ftell(F)!=End) return ESP_ERR_INVALID_SIZE;
    if(Verify) {
        uint8_t Buffer[512]; uint32_t CRC=0; size_t N;
        if(fseek(F,CHEAT_HEADER_SIZE,SEEK_SET)) return ESP_FAIL;
        while((N=fread(Buffer,1,sizeof(Buffer),F))) CRC=Cheats_CRC32(CRC,Buffer,N);
        if(ferror(F) || CRC!=H->DataCRC) return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
static bool PackMatchesSystem(const Header_t *H,const char *Path) {
    const char *Ext=strrchr(Path,'.');
    return (H->Version==2)==(Ext && !strcasecmp(Ext,".gg"));
}
static bool SameHeader(const Header_t *A,const Header_t *B) {
    return A->Version==B->Version && A->Size==B->Size && A->RomCRC==B->RomCRC && A->Count==B->Count && A->Patches==B->Patches && A->DataCRC==B->DataCRC;
}
static esp_err_t VerifyRom(FILE *F,const Header_t *H) {
    long Position=ftell(F); uint8_t Buffer[1024]; uint32_t CRC=0; size_t Total=0,N;
    if(Position<0 || fseek(F,0,SEEK_SET)) return ESP_FAIL;
    while((N=fread(Buffer,1,sizeof(Buffer),F))) { CRC=Cheats_CRC32(CRC,Buffer,N); Total+=N; }
    bool Valid=!ferror(F) && Total==H->Size && CRC==H->RomCRC;
    if(fseek(F,Position,SEEK_SET)) return ESP_FAIL;
    return Valid ? ESP_OK : ESP_ERR_INVALID_CRC;
}
static bool Enabled(const uint8_t *Bits,unsigned I) { return (Bits[I/8] & (1u<<(I%8)))!=0; }
static unsigned EnabledCount(const Header_t *H,const uint8_t *Bits) {
    unsigned N=0; for(unsigned I=0;I<H->Count;++I) N+=Enabled(Bits,I); return N;
}
static void Key(const Header_t *H,char K[16]) { snprintf(K,16,"c%08lx",(unsigned long)H->RomCRC); }
static esp_err_t Settings(const Header_t *H,uint8_t *Bits,bool Write) {
    char K[16]; Key(H,K); nvs_handle_t Handle; uint8_t B[48]={0}; size_t Size=sizeof(B);
    esp_err_t R=nvs_open("cheats",Write?NVS_READWRITE:NVS_READONLY,&Handle);
    if(!Write) memset(Bits,0,CHEAT_SELECTION_BYTES);
    if(R!=ESP_OK) return !Write && R==ESP_ERR_NVS_NOT_FOUND ? ESP_OK : R;
    if(Write) {
        memcpy(B,"CMS1",4); Put32(B+4,H->Size); Put32(B+8,H->DataCRC);
        memcpy(B+12,Bits,CHEAT_SELECTION_BYTES); Put32(B+44,Cheats_CRC32(0,B,44));
        R=nvs_set_blob(Handle,K,B,sizeof(B)); if(R==ESP_OK) R=nvs_commit(Handle);
    } else {
        R=nvs_get_blob(Handle,K,B,&Size);
        if(R==ESP_ERR_NVS_NOT_FOUND) R=ESP_OK;
        else if(R==ESP_OK && Size==sizeof(B) && !memcmp(B,"CMS1",4) && U32(B+44)==Cheats_CRC32(0,B,44)) {
            /* A replacement pack changes option order: default off instead of
             * applying the previous selection bits to different cheats. */
            if(U32(B+4)==H->Size && U32(B+8)==H->DataCRC) memcpy(Bits,B+12,CHEAT_SELECTION_BYTES);
        }
        else if(R==ESP_OK) R=ESP_ERR_INVALID_CRC;
    }
    nvs_close(Handle); return R;
}
esp_err_t Cheats_Badge(const char *RomPath,CheatBadge_t *Badge) {
    *Badge=(CheatBadge_t){0}; char Path[352];
    if(!PackPath(RomPath,Path,sizeof(Path))) return ESP_ERR_INVALID_ARG;
    FILE *F=fopen(Path,"rb"); if(!F) return errno==ENOENT?ESP_OK:ESP_FAIL;
    Header_t H; uint8_t Bits[CHEAT_SELECTION_BYTES];
    esp_err_t R=Header(F,&H,true);
    if(R==ESP_OK && !PackMatchesSystem(&H,RomPath)) R=ESP_ERR_INVALID_ARG;
    if(R==ESP_OK) R=Settings(&H,Bits,false);
    if(fclose(F) && R==ESP_OK) R=ESP_FAIL;
    if(R==ESP_OK) *Badge=(CheatBadge_t){.Available=true,.Enabled=EnabledCount(&H,Bits)>0};
    return R;
}
static esp_err_t Entry(FILE *F,const Header_t *H,unsigned Index,uint8_t B[CHEAT_ENTRY_SIZE]) {
    if(Index>=H->Count || ReadAt(F,CHEAT_HEADER_SIZE+Index*CHEAT_ENTRY_SIZE,B,CHEAT_ENTRY_SIZE)!=ESP_OK) return ESP_FAIL;
    const uint32_t First=U32(B+96),Count=U16(B+100);
    if(!memchr(B,0,CHEAT_NAME_SIZE) || !B[0] || !Count || First>H->Patches || Count>H->Patches-First || U16(B+102)) return ESP_ERR_INVALID_ARG;
    for(unsigned I=0;B[I];++I) if(B[I]<32 || B[I]>126) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
static int ComparePatch(const void *A,const void *B) {
    uint32_t X=((const CheatPatch_t *)A)->Offset,Y=((const CheatPatch_t *)B)->Offset; return (X>Y)-(X<Y);
}
static esp_err_t Plan(FILE *F,const Header_t *H,const uint8_t *Bits,CheatPlan_t *Out) {
    *Out=(CheatPlan_t){.GameGear=H->Version==2};
    if(!EnabledCount(H,Bits)) return ESP_OK;
    Out->Patches=malloc(CHEAT_MAX_PATCHES*sizeof(*Out->Patches)); if(!Out->Patches) return ESP_ERR_NO_MEM;
    esp_err_t R=ESP_OK;
    for(unsigned I=0;I<H->Count && R==ESP_OK;++I) if(Enabled(Bits,I)) {
        uint8_t B[CHEAT_ENTRY_SIZE]; R=Entry(F,H,I,B); if(R!=ESP_OK) break;
        unsigned First=U32(B+96),Count=U16(B+100);
        if(Count>(Out->GameGear ? 32 : CHEAT_MAX_PATCHES)-Out->Count) { R=ESP_ERR_INVALID_SIZE; break; }
        for(unsigned J=0;J<Count;++J) {
            uint8_t P[8]; R=ReadAt(F,CHEAT_HEADER_SIZE+H->Count*CHEAT_ENTRY_SIZE+(First+J)*8,P,8); if(R!=ESP_OK) break;
            uint32_t Address=U32(P);
            const bool Invalid=Out->GameGear ? (Address>=0xc000 || U16(P+6)>1) :
                (Address>=H->Size || (Address>=0x100 && Address<=0x14f) || U16(P+6) || P[4]==P[5]);
            if(Invalid) { R=ESP_ERR_INVALID_ARG; break; }
            Out->Patches[Out->Count++]=(CheatPatch_t){Address,P[4],P[5],Out->GameGear && U16(P+6)==1};
        }
    }
    if(R==ESP_OK) {
        qsort(Out->Patches,Out->Count,sizeof(*Out->Patches),ComparePatch);
        size_t Kept=0;
        for(size_t I=0;I<Out->Count;++I) {
            if(Kept && Out->Patches[I].Offset==Out->Patches[Kept-1].Offset) {
                if(Out->Patches[I].Value!=Out->Patches[Kept-1].Value || Out->Patches[I].Original!=Out->Patches[Kept-1].Original || Out->Patches[I].Compare!=Out->Patches[Kept-1].Compare) { R=ESP_ERR_INVALID_STATE; break; }
            } else Out->Patches[Kept++]=Out->Patches[I];
        }
        Out->Count=Kept;
    }
    if(R!=ESP_OK) Cheats_Free(Out);
    return R;
}
esp_err_t Cheats_Prepare(const char *RomPath,FILE *Rom,CheatPlan_t *Out) {
    *Out=(CheatPlan_t){0}; char Path[352];
    if(!PackPath(RomPath,Path,sizeof(Path))) return ESP_OK;
    FILE *F=fopen(Path,"rb"); if(!F) return errno==ENOENT ? ESP_OK : ESP_FAIL;
    Header_t H; uint8_t Bits[CHEAT_SELECTION_BYTES];
    esp_err_t R=Header(F,&H,true);
    if(R==ESP_OK && !PackMatchesSystem(&H,RomPath)) R=ESP_ERR_INVALID_ARG;
    if(R==ESP_OK) R=Settings(&H,Bits,false);
    if(R==ESP_OK && EnabledCount(&H,Bits)) {
        R=VerifyRom(Rom,&H);
        if(R==ESP_OK) R=Plan(F,&H,Bits,Out);
    }
    if(fclose(F) && R==ESP_OK) R=ESP_FAIL;
    if(R!=ESP_OK) Cheats_Free(Out);
    if(R==ESP_OK && Out->Count) printf("CHEATS patches=%u\n",(unsigned)Out->Count);
    return R;
}
void Cheats_Close(void) { memset(&Menu,0,sizeof(Menu)); }
esp_err_t Cheats_Menu(CheatAction_t Action,const char *RomPath,size_t Selected,CheatView_t *View) {
    memset(View,0,sizeof(*View));
    if(Action==kCheatOpen) { Cheats_Close(); if(!PackPath(RomPath,Menu.Path,sizeof(Menu.Path))) return ESP_ERR_INVALID_ARG; }
    else if(!Menu.Open) return ESP_ERR_INVALID_STATE;
    sdmmc_card_t *Card=NULL; esp_err_t R=SDCard_Mount(&Card); if(R!=ESP_OK) return R;
    FILE *F=fopen(Menu.Path,"rb"); Header_t H;
    uint8_t NextBits[CHEAT_SELECTION_BYTES]; bool Save=false;
    if(!F) { R=errno==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL; goto Done; }
    R=Header(F,&H,true); if(R!=ESP_OK) goto Done;
    if(Action==kCheatOpen) {
        if(!PackMatchesSystem(&H,RomPath)) { R=ESP_ERR_INVALID_ARG; goto Done; }
        FILE *Rom=fopen(RomPath,"rb"); if(!Rom) { R=ESP_ERR_NOT_FOUND; goto Done; }
        R=VerifyRom(Rom,&H); if(fclose(Rom) && R==ESP_OK) R=ESP_FAIL;
        if(R==ESP_OK) R=Settings(&H,Menu.Bits,false);
        if(R!=ESP_OK) goto Done;
        Menu.Header=H; Menu.Open=true;
    } else if(!SameHeader(&H,&Menu.Header)) { R=ESP_ERR_INVALID_CRC; goto Done; }
    memcpy(NextBits,Menu.Bits,sizeof(NextBits));
    if(Selected>H.Count) Selected=H.Count;
    if(Action==kCheatToggle || Action==kCheatDisableAll) {
        uint8_t *Bits=NextBits;
        if(Action==kCheatDisableAll || !Selected) memset(Bits,0,sizeof(NextBits));
        else Bits[(Selected-1)/8]^=1u<<((Selected-1)%8);
        CheatPlan_t Test; R=Plan(F,&H,Bits,&Test); Cheats_Free(&Test);
        if(R!=ESP_OK) goto Done;
        Save=memcmp(NextBits,Menu.Bits,sizeof(NextBits))!=0;
    }
    View->Count=H.Count+1; View->Selected=Selected; View->Enabled=EnabledCount(&H,NextBits);
    View->First=Selected>0?Selected-1:0;
    if(View->Count>CHEAT_ROWS && View->First>View->Count-CHEAT_ROWS) View->First=View->Count-CHEAT_ROWS;
    for(unsigned I=0;I<CHEAT_ROWS && I+View->First<View->Count;++I) {
        unsigned Index=I+View->First;
        if(!Index) strcpy(View->Rows[I].Name,"Disable all");
        else {
            uint8_t B[CHEAT_ENTRY_SIZE]; R=Entry(F,&H,Index-1,B); if(R!=ESP_OK) goto Done;
            memcpy(View->Rows[I].Name,B,CHEAT_NAME_SIZE); View->Rows[I].Enabled=Enabled(NextBits,Index-1);
        }
    }
Done:
    if(F && fclose(F) && R==ESP_OK) R=ESP_FAIL;
    { esp_err_t U=SDCard_Unmount(Card); if(R==ESP_OK) R=U; }
    /* Do not save a toggle unless all reads and SD cleanup succeeded. */
    if(R==ESP_OK && Save) {
        R=Settings(&H,NextBits,true);
        if(R==ESP_OK) memcpy(Menu.Bits,NextBits,sizeof(NextBits));
    }
    return R;
}
