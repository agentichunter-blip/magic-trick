#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gg_cheats.h"
#include "cart_link.h"
#include "freertos/task.h"
static uint8_t Sequence=255,Count;static bool Busy,Drop,BadMagic,BadCount,SendFail;
static unsigned Sends,Delays;
static const CheatPatch_t Patches[]={{0x9234,0x3a,0x99,true},{0x4567,0,0x42,false}};
esp_err_t CartLink_VirtualCommand(uint8_t C,uint16_t Config,uint8_t High,CartLinkResponse_t *R) {
 assert(C==0x30 && !Config && !High);
 *R=(CartLinkResponse_t){.Count=4,.Data={Busy,BadCount?99:Count,Sequence,BadMagic?0:0xc7}};return ESP_OK;
}
esp_err_t FPGA_Tx_GGCheat(const uint8_t Payload[6]) {
 ++Sends;if(SendFail)return ESP_FAIL;
 if(!Payload[0]) { for(unsigned i=0;i<6;++i)assert(Payload[i]==0);if(!Drop)Count=0; }
 else { assert(Payload[0]==1 && Count<2);const CheatPatch_t *P=&Patches[Count];
  assert(((unsigned)Payload[1]<<8|Payload[2])==P->Offset && Payload[3]==P->Original && Payload[4]==P->Value && Payload[5]==P->Compare);
  if(!Drop)++Count;
 }
 if(!Drop)++Sequence;return ESP_OK;
}
void vTaskDelay(TickType_t T) { assert(T==5);++Delays; }
int main(void) {
 CheatPlan_t P={.Patches=(CheatPatch_t*)Patches,.Count=2,.GameGear=true};
 assert(GGCheats_Install(&P)==ESP_OK && Sends==3 && Sequence==2 && Count==2);
 P.Count=0;assert(GGCheats_Install(&P)==ESP_OK && Count==0 && Sends==4);
 BadMagic=true;assert(GGCheats_Install(&P)==ESP_ERR_NOT_SUPPORTED && Sends==4);BadMagic=false;
 Busy=true;assert(GGCheats_Install(&P)==ESP_ERR_INVALID_STATE && Sends==4);Busy=false;
 Drop=true;assert(GGCheats_Install(&P)==ESP_ERR_TIMEOUT && Delays==20);Drop=false;
 BadCount=true;assert(GGCheats_Install(&P)==ESP_ERR_INVALID_RESPONSE);BadCount=false;
 SendFail=true;assert(GGCheats_Install(&P)==ESP_FAIL);SendFail=false;
 P.Count=33;assert(GGCheats_Install(&P)==ESP_ERR_INVALID_ARG);
 P.Count=1;P.GameGear=false;assert(GGCheats_Install(&P)==ESP_ERR_INVALID_ARG);
 puts("PASS: GG clear/append acknowledgement, sequence rollover, wrong core, busy, dropped UART, bad count and bounds");
}
