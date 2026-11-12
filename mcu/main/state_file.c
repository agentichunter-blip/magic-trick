#include "state_file.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#endif

static const uint8_t Magic[4] = {'C', 'M', 'S', 'T'};

static void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t *p, uint32_t v) { Put16(p, (uint16_t)v); Put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t Get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Get32(const uint8_t *p) { return (uint32_t)Get16(p) | ((uint32_t)Get16(p + 2) << 16); }

/* Standard CRC-32 (reflected, 0xEDB88320), chainable like esp_rom_crc32_le. */
uint32_t StateFile_Crc32(uint32_t Crc, const uint8_t *pData, size_t Size)
{
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(Crc, pData, Size);
#else
    Crc = ~Crc;
    while (Size--)
    {
        Crc ^= *pData++;
        for (unsigned Bit = 0; Bit < 8; ++Bit)
            Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u)));
    }
    return ~Crc;
#endif
}

uint32_t StateFile_PayloadLength(uint32_t RamSize)
{
    return kStateFileCoreBytes + RamSize;
}

/* Layout:
 *  0 magic "CMST"         4 version          6 header size
 *  8 payload length      12 payload CRC-32  16 engine signature
 * 20 ROM size            24 RAM size        28 global checksum
 * 30 cartridge type      31 reserved        32 title (16 bytes)
 * 48 reserved (12)       60 header CRC-32 over bytes 0..59 */
void StateFile_EncodeHeader(const StateFileGame_t *pGame, uint32_t PayloadLength,
                            uint32_t PayloadCrc,
                            uint8_t Header[kStateFileHeaderSize])
{
    memset(Header, 0, kStateFileHeaderSize);
    memcpy(Header, Magic, sizeof(Magic));
    Put16(Header + 4, kStateFileVersion);
    Put16(Header + 6, kStateFileHeaderSize);
    Put32(Header + 8, PayloadLength);
    Put32(Header + 12, PayloadCrc);
    Put32(Header + 16, kStateFileEngineSignature);
    Put32(Header + 20, pGame->RomSize);
    Put32(Header + 24, pGame->RamSize);
    Put16(Header + 28, pGame->GlobalChecksum);
    Header[30] = pGame->CartType;
    memcpy(Header + 32, pGame->Title, sizeof(pGame->Title));
    Put32(Header + 60, StateFile_Crc32(0, Header, 60));
}

StateFileResult_t StateFile_DecodeHeader(const uint8_t Header[kStateFileHeaderSize],
                                         const StateFileGame_t *pGame,
                                         uint32_t *pPayloadLength,
                                         uint32_t *pPayloadCrc)
{
    if (memcmp(Header, Magic, sizeof(Magic)) != 0) return kStateFile_BadMagic;
    if (Get16(Header + 4) != kStateFileVersion ||
        Get16(Header + 6) != kStateFileHeaderSize)
        return kStateFile_BadVersion;
    if (Get32(Header + 60) != StateFile_Crc32(0, Header, 60))
        return kStateFile_BadHeaderCrc;
    if (Get32(Header + 16) != kStateFileEngineSignature) return kStateFile_BadEngine;
    if (Get32(Header + 20) != pGame->RomSize || Get32(Header + 24) != pGame->RamSize ||
        Get16(Header + 28) != pGame->GlobalChecksum || Header[30] != pGame->CartType ||
        memcmp(Header + 32, pGame->Title, sizeof(pGame->Title)) != 0)
        return kStateFile_WrongGame;
    const uint32_t Length = Get32(Header + 8);
    if (pGame->RamSize > kStateFileMaxRamSize ||
        Length != StateFile_PayloadLength(pGame->RamSize) || Length > kStateFileSlotSize)
        return kStateFile_BadLength;
    *pPayloadLength = Length;
    *pPayloadCrc = Get32(Header + 12);
    return kStateFile_Ok;
}

bool StateFile_PayloadSignatureOk(const uint8_t *pPayload, size_t Size)
{
    return Size >= 8 && Get32(pPayload + 4) == kStateFileEngineSignature;
}

const char *StateFile_ResultName(StateFileResult_t Result)
{
    switch (Result)
    {
    case kStateFile_Ok: return "ok";
    case kStateFile_BadMagic: return "bad_magic";
    case kStateFile_BadVersion: return "bad_version";
    case kStateFile_BadHeaderCrc: return "bad_header_crc";
    case kStateFile_WrongGame: return "wrong_game";
    case kStateFile_BadLength: return "bad_length";
    case kStateFile_BadEngine: return "bad_engine";
    }
    return "unknown";
}
