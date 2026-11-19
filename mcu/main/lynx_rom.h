#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

/* Atari Lynx game files, as the FPGA core (lynx_board_core.sv) accepts them:
 * - .lnx: 64-byte "LYNX" header (bank 0 page size at 4-5, little endian;
 *   cartridge name at 10, 32 bytes), then the cartridge image;
 * - BS93 homebrew (.o, also seen as .lnx): 10-byte header with "BS93" at 6,
 *   loaded into RAM and started without the boot ROM;
 * - .lyx: a headerless cartridge image (128, 256 or 512 KiB).
 * The core addresses at most 1 MiB. */
enum { kLynxMaxFile = 1024 * 1024, kLynxHeaderSize = 64, kLynxBiosSize = 512 };

typedef struct {
    uint32_t Size;     /* whole file */
    bool Bs93;         /* runs without the boot ROM */
    bool Header;       /* .lnx header present */
    uint16_t Bank0;    /* cartridge bank 0 page size (0 for BS93) */
    uint16_t Load;     /* BS93 load and start address */
    uint8_t Rotation;  /* header byte 58: 0 none, 1 left, 2 right */
    char Title[33];
} LynxRomInfo_t;

/* Sideways games (played with the Lynx on its side) whose dumps have no
 * header to say so, by the CRC-32 of the cartridge image's first 4 KiB (after
 * any .lnx header): unique across the No-Intro set. 1 = left (the picture is
 * turned clockwise to stand upright), 2 = right (anticlockwise), as in the
 * header's byte 58. */
enum { kLynxRotationNone = 0, kLynxRotationLeft = 1, kLynxRotationRight = 2, kLynxRotationProbe = 4096 };

static inline uint8_t LynxRom_KnownRotation(uint32_t crc_first_4k)
{
    switch (crc_first_4k) {
    case 0x20aa76d2u: /* Gauntlet - The Third Encounter (USA, Europe) */
    case 0x20af0f2eu: /* Klax (USA, Europe) */
        return kLynxRotationLeft;
    case 0x33c4180au: /* Centipede (USA) (Proto) */
    case 0x61d92e3bu: /* Lexis (USA, Europe) */
    case 0x0cf623e5u: /* NFL Football (USA, Europe) */
    case 0x6b29440du: /* Raiden (USA) (v3.0) */
        return kLynxRotationRight;
    default:
        return kLynxRotationNone;
    }
}

static inline esp_err_t LynxRom_Inspect(FILE *rom, LynxRomInfo_t *info)
{
    if (!rom || !info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (fseek(rom, 0, SEEK_END)) return ESP_FAIL;
    const long length = ftell(rom);
    if (length < 16 || length > kLynxMaxFile) return ESP_ERR_INVALID_SIZE;
    uint8_t header[kLynxHeaderSize] = {0};
    const size_t want = length < kLynxHeaderSize ? (size_t)length : kLynxHeaderSize;
    if (fseek(rom, 0, SEEK_SET) || fread(header, 1, want, rom) != want) return ESP_FAIL;
    info->Size = (uint32_t)length;
    if (!memcmp(header + 6, "BS93", 4)) {
        info->Bs93 = true;
        info->Load = (uint16_t)(header[2] << 8 | header[3]);
        strcpy(info->Title, "LYNX HOMEBREW");
    } else if (!memcmp(header, "LYNX", 4)) {
        if (length <= kLynxHeaderSize) return ESP_ERR_INVALID_SIZE;
        info->Header = true;
        info->Bank0 = (uint16_t)(header[4] | header[5] << 8);
        if (info->Bank0 != 0x100 && info->Bank0 != 0x200 && info->Bank0 != 0x400 && info->Bank0 != 0x800)
            return ESP_ERR_NOT_SUPPORTED;
        info->Rotation = header[58];
        memcpy(info->Title, header + 10, 32);
        info->Title[32] = 0;
    } else {
        /* Headerless cartridge: the page size follows from the size. */
        if (length != 128 * 1024 && length != 256 * 1024 && length != 512 * 1024)
            return ESP_ERR_INVALID_RESPONSE;
        /* 256 pages per bank: 128 KiB -> 512-byte pages, 256 KiB -> 1 KiB, 512 KiB -> 2 KiB. */
        info->Bank0 = (uint16_t)(length / 256);
        strcpy(info->Title, "LYNX");
    }
    for (char *p = info->Title; *p; ++p)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = ' ';
    return fseek(rom, 0, SEEK_SET) ? ESP_FAIL : ESP_OK;
}
