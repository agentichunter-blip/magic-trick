#pragma once

#include "esp_err.h"

#include <stdint.h>

typedef enum {
    kFPGA_TxConsts_BufferSize = 1024,    // [bytes]
} FPGA_TxConsts_t;

void FPGA_TxTask(void *arg);
void FPGA_Tx_Resume(void);
void FPGA_Tx_Pause(void);
void FPGA_Tx_SendAll(void);
void FPGA_Tx_WriteBrightness(void);
void FPGA_Tx_SendSysCtl(void);
void FPGA_Tx_PokeButtons(void);
void FPGA_Tx_WritePaletteStyle(void);
esp_err_t FPGA_Tx_CartRequest(const uint8_t Request[8]);
esp_err_t FPGA_Tx_QueueCartRequest(const uint8_t Request[8]);
esp_err_t FPGA_Tx_CartStreamAck(uint8_t Sequence);
esp_err_t FPGA_Tx_PulseButtons(uint16_t Buttons);
esp_err_t FPGA_Tx_RequestButtons(void);

esp_err_t FPGA_Tx_CoreReload(uint8_t Operation, uint8_t Target, uint16_t Nonce);

esp_err_t FPGA_Tx_GGCheat(const uint8_t Payload[6]);
