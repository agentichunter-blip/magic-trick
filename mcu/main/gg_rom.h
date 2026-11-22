#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
/* Header reference: https://www.smspower.org/Development/ROMHeader
 * Size comes from the file: the header size nibble can describe only the
 * checksummed prefix (as seen in real cartridge images).
 * This initial path assumes the adapter's Sega mapper, not autodetection.
 * Master System games (.sms, run on the same core in SMS mode) are accepted
 * without a header (many Japanese cartridges have none) and in any region,
 * in whole 8 KiB pages up to 4 MiB; the core mirrors a size that is not a
 * power of two by the MCU's upload (see UploadRomPatched). */
static inline esp_err_t GGRom_InspectSystem(FILE *rom, uint32_t *size, int sms)
{
    if (!rom || !size) return ESP_ERR_INVALID_ARG;
    if (fseek(rom, 0, SEEK_END)) return ESP_FAIL;
    long length=ftell(rom);
    if (length<8192 || length>4*1024*1024 || (sms ? (length & 8191) : (length & (length-1))))
        return ESP_ERR_INVALID_SIZE;
    if (sms) {
        *size=(uint32_t)length;
        return fseek(rom,0,SEEK_SET) ? ESP_FAIL : ESP_OK;
    }
    const long positions[]={0x7ff0,0x3ff0,0x1ff0};
    for (unsigned i=0;i<3;i++) {
        uint8_t header[16];
        if (positions[i]+16>length) continue;
        if (fseek(rom,positions[i],SEEK_SET) || fread(header,1,16,rom)!=16)
            return ESP_FAIL;
        if (memcmp(header,"TMR SEGA",8)) continue;
        unsigned region=header[15]>>4;
        if (region<5 || region>7) return ESP_ERR_NOT_SUPPORTED;
        *size=(uint32_t)length;
        return fseek(rom,0,SEEK_SET) ? ESP_FAIL : ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
static inline esp_err_t GGRom_Inspect(FILE *rom, uint32_t *size) { return GGRom_InspectSystem(rom, size, 0); }
