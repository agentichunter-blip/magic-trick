#pragma once
/* NES game files (.nes): iNES and NES 2.0 headers.
 * https://www.nesdev.org/wiki/INES and https://www.nesdev.org/wiki/NES_2.0
 * The MCU parses the header; the core (nes_board_core.sv) reads the results
 * from the 16-byte info block at the start of the PSRAM ROM window:
 *   0-3   'N','E','S','1'
 *   4-5   mapper number (little endian)
 *   6     submapper (NES 2.0, else 0)
 *   7     bit 0 vertical mirroring, bit 1 four-screen, bit 2 battery,
 *         bit 3 CHR RAM (no CHR ROM), bit 4 NES 2.0 header
 *   8-9   PRG ROM size in KiB
 *   10-11 CHR ROM size in KiB (0: CHR RAM)
 *   12-13 NES 2.0 header bytes 10 and 11 (RAM sizes), else 0
 * PRG ROM follows at window offset 0x1000, CHR ROM right after it. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

enum {
    kNesHeaderSize = 16,
    kNesTrainerSize = 512,
    kNesInfoSize = 16,
    kNesFileAddress = 0x1000,
    /* PRG + CHR must end below the save-state staging area (0x3F0000). */
    kNesMaxRomBytes = 0x3F0000 - 0x1000,
};

typedef struct {
    uint16_t Mapper;
    uint8_t Submapper;
    uint8_t Flags;        /* info block byte 7 */
    uint32_t PrgBytes, ChrBytes;
    uint32_t DataOffset;  /* PRG ROM's offset in the file (header + trainer) */
    uint8_t RamShifts, ChrRamShifts;
    bool Battery;
} NesRomInfo_t;

/* Mappers the core implements (FEASIBILITY.md / nes-work README). */
static inline bool NesRom_MapperSupported(uint16_t Mapper)
{
    static const uint16_t Supported[] = {0, 1, 2, 3, 4, 7, 9, 10, 11, 21, 22, 23, 25, 66, 69, 71};
    for (size_t i = 0; i < sizeof(Supported) / sizeof(Supported[0]); ++i)
        if (Supported[i] == Mapper) return true;
    return false;
}

static inline esp_err_t NesRom_Parse(const uint8_t Header[kNesHeaderSize], long FileSize, NesRomInfo_t *pInfo)
{
    if (!Header || !pInfo) return ESP_ERR_INVALID_ARG;
    memset(pInfo, 0, sizeof(*pInfo));
    if (memcmp(Header, "NES\x1a", 4)) return ESP_ERR_INVALID_RESPONSE;
    const uint8_t F6 = Header[6], F7 = Header[7];
    const bool Nes2 = (F7 & 0x0c) == 0x08;
    uint32_t PrgUnits = Header[4], ChrUnits = Header[5];
    uint16_t Mapper = (uint16_t)(F6 >> 4);
    if (Nes2) {
        Mapper |= (uint16_t)(F7 & 0xf0) | (uint16_t)((Header[8] & 0x0f) << 8);
        pInfo->Submapper = Header[8] >> 4;
        /* Exponent-multiplier sizes (MSB nibble 0xF) are not used by real games. */
        if ((Header[9] & 0x0f) == 0x0f || (Header[9] >> 4) == 0x0f) return ESP_ERR_NOT_SUPPORTED;
        PrgUnits |= (uint32_t)(Header[9] & 0x0f) << 8;
        ChrUnits |= (uint32_t)(Header[9] >> 4) << 8;
        pInfo->RamShifts = Header[10];
        pInfo->ChrRamShifts = Header[11];
    } else if (!Header[12] && !Header[13] && !Header[14] && !Header[15]) {
        /* Old dumps carry text ("DiskDude!") in bytes 7-15: use the upper
         * mapper nibble only when the tail of the header is clean. */
        Mapper |= (uint16_t)(F7 & 0xf0);
    }
    pInfo->Mapper = Mapper;
    pInfo->PrgBytes = PrgUnits * 16384u;
    pInfo->ChrBytes = ChrUnits * 8192u;
    pInfo->DataOffset = kNesHeaderSize + ((F6 & 0x04) ? kNesTrainerSize : 0);
    pInfo->Battery = (F6 & 0x02) != 0;
    pInfo->Flags = (uint8_t)((F6 & 0x01) | ((F6 & 0x08) ? 0x02 : 0) | (pInfo->Battery ? 0x04 : 0) |
                             (ChrUnits == 0 ? 0x08 : 0) | (Nes2 ? 0x10 : 0));
    if (pInfo->PrgBytes == 0) return ESP_ERR_INVALID_SIZE;
    if (pInfo->PrgBytes + pInfo->ChrBytes > kNesMaxRomBytes) return ESP_ERR_INVALID_SIZE;
    if (FileSize >= 0 && (uint32_t)FileSize < pInfo->DataOffset + pInfo->PrgBytes + pInfo->ChrBytes)
        return ESP_ERR_INVALID_SIZE;
    if (!NesRom_MapperSupported(Mapper)) return ESP_ERR_NOT_SUPPORTED;
    return ESP_OK;
}

static inline void NesRom_InfoBlock(const NesRomInfo_t *pInfo, uint8_t Out[kNesInfoSize])
{
    memset(Out, 0, kNesInfoSize);
    memcpy(Out, "NES1", 4);
    Out[4] = (uint8_t)pInfo->Mapper; Out[5] = (uint8_t)(pInfo->Mapper >> 8);
    Out[6] = pInfo->Submapper;
    Out[7] = pInfo->Flags;
    const uint32_t PrgKiB = pInfo->PrgBytes / 1024u, ChrKiB = pInfo->ChrBytes / 1024u;
    Out[8] = (uint8_t)PrgKiB; Out[9] = (uint8_t)(PrgKiB >> 8);
    Out[10] = (uint8_t)ChrKiB; Out[11] = (uint8_t)(ChrKiB >> 8);
    Out[12] = pInfo->RamShifts; Out[13] = pInfo->ChrRamShifts;
}

static inline esp_err_t NesRom_Inspect(FILE *pRom, NesRomInfo_t *pInfo)
{
    uint8_t Header[kNesHeaderSize];
    if (!pRom || fseek(pRom, 0, SEEK_END)) return ESP_FAIL;
    const long Size = ftell(pRom);
    if (fseek(pRom, 0, SEEK_SET) || fread(Header, 1, sizeof(Header), pRom) != sizeof(Header)) return ESP_ERR_INVALID_SIZE;
    return NesRom_Parse(Header, Size, pInfo);
}
