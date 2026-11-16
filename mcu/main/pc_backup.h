#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum {
    kPCBackupConsoleBaud = 2000000,
    kPCBackupNormalConsoleBaud = 115200,
};

esp_err_t PCBackup_Init(void);

uint32_t PCBackup_GetConsoleBaud(void);
bool PCBackup_IsEnabled(void);

/* Wireless transfer task owns the PC-mode gate until EndWireless. This blocks
 * USB commands and game launches while retaining the existing save/stop path. */
esp_err_t PCBackup_BeginWireless(void);
esp_err_t PCBackup_EndWireless(void);

void PCBackup_TransportReady(void);

esp_err_t PCBackup_ConfigureConsoleInput(void);

void PCBackup_ConsoleReady(void);

esp_err_t PCBackup_RegisterConsoleCommand(void);

esp_err_t PCBackup_ReadUpload(uint8_t *pData, size_t Size);

/* Raw bytes to the console UART, after flushing stdout (stdout turns "\n"
 * into "\r\n", which would corrupt binary data). */
esp_err_t PCBackup_WriteRaw(const void *pData, size_t Size);
void PCBackup_FlushInput(void);

/* Largest burst the PC may send unpaced. The Chromatic's USB serial link
 * loses bytes when more arrives back to back (measured: 128 bytes with a
 * 1 ms gap works, 256 does not; the ESP32's UART RX FIFO holds 128). */
enum { kPCBackupUploadBurst = 128 };

/* Asks the web ChroMagician for Size bytes at Offset with one
 * "CONNECT SEND kind=<Kind> offset= size=" record per burst and reads them. */
esp_err_t PCBackup_ReceiveSends(const char *pKind, uint32_t Offset,
                                uint8_t *pData, size_t Size);

typedef enum {
    kPCBackupGate_Cartridge,  /* cartridge engine on (virtual cart stopped) */
    kPCBackupGate_Storage,    /* SD access (virtual cart stopped) */
} PCBackupGateUse_t;

/* Runs Fn with the PC-mode gate held. ESP_ERR_INVALID_STATE when PC mode
 * (USB TRANSFERS) is off, ESP_ERR_TIMEOUT when another user holds it. */
esp_err_t PCBackup_RunGated(PCBackupGateUse_t Use,
                            esp_err_t (*Fn)(void *pContext), void *pContext);

/* PC mode switched on from the menu (not the wireless transfer). */
bool PCBackup_IsUSBModeOn(void);
/* 1 when a cartridge answers, 0 when none, -1 when unknown. */
int PCBackup_CartridgePresent(void);
