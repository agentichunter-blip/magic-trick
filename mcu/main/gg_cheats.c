#include "gg_cheats.h"
#include "fpga_tx.h"
#include "cart_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_err_t Status(uint8_t *Sequence,uint8_t *Count,bool *Busy)
{
    CartLinkResponse_t Reply={0};
    esp_err_t Result=CartLink_VirtualCommand(0x30,0,0,&Reply);
    if(Result!=ESP_OK)return Result;
    if(Reply.Status || Reply.Count!=4 || Reply.Data[3]!=0xc7)return ESP_ERR_NOT_SUPPORTED;
    *Sequence=Reply.Data[2];*Count=Reply.Data[1];*Busy=Reply.Data[0]&1;
    return ESP_OK;
}
static esp_err_t Send(const uint8_t Payload[6],uint8_t *Sequence,uint8_t ExpectedCount)
{
    esp_err_t Result=FPGA_Tx_GGCheat(Payload);
    if(Result!=ESP_OK)return Result;
    const uint8_t ExpectedSequence=*Sequence+1;
    for(unsigned Try=0;Try<20;++Try) {
        uint8_t ActualSequence,ActualCount;bool Busy;
        Result=Status(&ActualSequence,&ActualCount,&Busy);
        if(Result!=ESP_OK)return Result;
        if(!Busy && ActualSequence==ExpectedSequence) {
            if(ActualCount!=ExpectedCount)return ESP_ERR_INVALID_RESPONSE;
            *Sequence=ActualSequence;return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}
esp_err_t GGCheats_Install(const CheatPlan_t *Plan)
{
    if(!Plan || Plan->Count>32 || (Plan->Count && (!Plan->GameGear || !Plan->Patches)))return ESP_ERR_INVALID_ARG;
    /* Validate before clearing the old list. */
    for(size_t I=0;I<Plan->Count;++I)
        if(Plan->Patches[I].Offset>=0xc000)return ESP_ERR_INVALID_ARG;
    uint8_t Sequence,Count;bool Busy;
    esp_err_t Result=Status(&Sequence,&Count,&Busy);
    if(Result!=ESP_OK)return Result;
    if(Busy)return ESP_ERR_INVALID_STATE;
    const uint8_t Clear[6]={0};
    Result=Send(Clear,&Sequence,0);
    for(size_t I=0;Result==ESP_OK && I<Plan->Count;++I) {
        const CheatPatch_t *P=&Plan->Patches[I];
        uint8_t Payload[6]={1,P->Offset>>8,P->Offset,P->Original,P->Value,P->Compare};
        Result=Send(Payload,&Sequence,I+1);
    }
    return Result;
}
