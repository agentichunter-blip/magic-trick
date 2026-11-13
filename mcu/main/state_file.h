#pragma once
/* SD card file for one native save state ("<rom>.state", ".state2", ".state3").
 *
 * A 64-byte little-endian header followed by the engine payload:
 * - Game Boy / Game Boy Color: the FPGA snapshot slot exactly as gb_savestates
 *   wrote it to PSRAM (8-byte engine header, 128 internal 64-bit registers,
 *   WRAM, VRAM, OAM, HRAM, then cartridge RAM).
 * - Game Gear: 41 blocks of 1 KiB from gg_state_engine (block 0 header and
 *   registers, then work RAM, cartridge RAM and VRAM). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    kStateFileHeaderSize = 64,
    kStateFileVersion = 1,
    /* Engine header signature in payload bytes 4..7 (gb_savestates STATESIZE). */
    kStateFileEngineSignature = 0xB0CE,
    kStateFileCoreBytes = 8 + 128 * 8 + 32768 + 16384 + 160 + 128,
    kStateFileMaxRamSize = 128 * 1024,
    kStateFileSlotSize = 256 * 1024,
    /* Game Gear payload: bytes 4,5 = 'g','g', byte 6 = layout version 1. */
    kStateFileGameGearSignature = 0x00016767,
    kStateFileGameGearPayload = 41 * 1024,
};

typedef enum {
    kStateEngine_GameBoy = 0,
    kStateEngine_GameGear = 1,
} StateEngine_t;

typedef struct {
    char Title[16];
    uint32_t RomSize;
    uint32_t RamSize;
    uint16_t GlobalChecksum;
    uint8_t CartType;
    uint8_t Engine; /* StateEngine_t; 0 (Game Boy) when left unset */
} StateFileGame_t;

typedef enum {
    kStateFile_Ok,
    kStateFile_BadMagic,
    kStateFile_BadVersion,
    kStateFile_BadHeaderCrc,
    kStateFile_WrongGame,
    kStateFile_BadLength,
    kStateFile_BadEngine,
} StateFileResult_t;

/* Bytes the Game Boy engine writes for a game with this much cartridge RAM. */
uint32_t StateFile_PayloadLength(uint32_t RamSize);
/* Payload bytes for this game's engine. */
uint32_t StateFile_GamePayloadLength(const StateFileGame_t *pGame);

void StateFile_EncodeHeader(const StateFileGame_t *pGame, uint32_t PayloadLength,
                            uint32_t PayloadCrc,
                            uint8_t Header[kStateFileHeaderSize]);

/* Checks everything except the payload CRC, which the caller verifies while
 * streaming. On success *pPayloadLength and *pPayloadCrc are filled in. */
StateFileResult_t StateFile_DecodeHeader(const uint8_t Header[kStateFileHeaderSize],
                                         const StateFileGame_t *pGame,
                                         uint32_t *pPayloadLength,
                                         uint32_t *pPayloadCrc);

/* The first payload block must carry the engine signature. */
bool StateFile_PayloadSignatureOk(const uint8_t *pPayload, size_t Size);
bool StateFile_GamePayloadSignatureOk(const StateFileGame_t *pGame,
                                      const uint8_t *pPayload, size_t Size);

uint32_t StateFile_Crc32(uint32_t Crc, const uint8_t *pData, size_t Size);

const char *StateFile_ResultName(StateFileResult_t Result);
