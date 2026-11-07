#include "cheats.h"
#include <stdlib.h>

uint32_t Cheats_CRC32(uint32_t CRC,const void *Data,size_t Size) {
    /* Four-bit table keeps flash/DRAM use small without a bit-at-a-time ROM scan. */
    static const uint32_t T[16]={0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
    const uint8_t *P=Data; CRC=~CRC;
    while(Size--) { CRC^=*P++; CRC=(CRC>>4)^T[CRC&15]; CRC=(CRC>>4)^T[CRC&15]; }
    return ~CRC;
}
void Cheats_Free(CheatPlan_t *Plan) { free(Plan->Patches); *Plan=(CheatPlan_t){0}; }
esp_err_t Cheats_Apply(const CheatPlan_t *Plan,uint32_t Offset,uint8_t *Data,size_t Size) {
    /* Check all originals before writing: a bad block is never half-patched. */
    for(size_t I=0;I<Plan->Count;++I) { const CheatPatch_t *P=&Plan->Patches[I]; if(P->Offset>=Offset && P->Offset-Offset<Size && Data[P->Offset-Offset]!=P->Original) return ESP_ERR_INVALID_CRC; }
    for(size_t I=0;I<Plan->Count;++I) { const CheatPatch_t *P=&Plan->Patches[I]; if(P->Offset>=Offset && P->Offset-Offset<Size) Data[P->Offset-Offset]=P->Value; }
    return ESP_OK;
}
