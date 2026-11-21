#pragma once

/* Magic Trick carts: Game Gear and Lynx games stored on a reflashable Game
 * Boy cartridge (ModRetro carts and other AMD-flash MBC5 carts), loaded into
 * PSRAM like an SD game.
 *
 * Layout (1 KiB blocks, MBC5 linear offsets). Version 2 (written by the
 * installer's Cartridge tab, tools/mtcart.py):
 *  - Directory at the same places as version 1 (below): a 64-byte header
 *    ("MAGICTRICKCART02", entry count, CRC-32 of the entries, directory
 *    bytes, CRC-32 of the header's first 28 bytes) followed by 128-byte
 *    entries ("MTE2", system, flags, offset, size, CRC, art offset, art
 *    size, art CRC, 72-byte name), as many as fit the cart.
 * Version 1 (console cartadd, appended without erasing):
 *  - A directory block with kCartStoreMagic, at 0x4000 (a dedicated Magic
 *    Trick cart, whose bank 0 is a small Game Boy program explaining it) or
 *    else right after the Game Boy game named by the header's ROM size (games
 *    added to the free flash of an ordinary cart).
 *  - Up to kCartStoreSlots 96-byte entries in that block; empty slots stay
 *    0xFF so a new entry can be programmed later without erasing.
 *  - Game data 1 KiB aligned, after the directory.
 * tools/mtcart.py builds and reads the same format.
 *
 * Games appear as files "/cart/<name>" (read-only VFS). Saves and states for
 * them live in /sdcard/CHROMAGIC/CART/: on the microSD card, or with no card
 * in the MCU's internal store (sd_card.h, SDCard_IsVirtual).
 *
 * Only the GB/GBC core reads ModRetro carts (the Game Gear core reads 0xFF,
 * the Lynx core has no cartridge engine). So a Game Gear or Lynx game is
 * staged into PSRAM by the GB core, which then switches cores. The copy
 * survives the FPGA reload, except the 1 KiB bursts each core's PSRAM
 * self-test writes; those blocks and the file's first 1 KiB are kept in NVS
 * and replayed by the target core. Choosing a cart game in another core goes
 * back through the GB core (CoreSwitch_RequestVia). */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CART_STORE_ROOT "/cart"
#define CART_STORE_SIDECAR_ROOT "/sdcard/CHROMAGIC/CART"
/* The cartridge's own Game Boy game (an ordinary cart with games added to its
 * free flash): "/cart/.cartridge/<TITLE> (CARTRIDGE).gb[c]". Choosing it
 * restarts the cartridge in the GB core instead of loading a file. */
#define CART_STORE_OWN_ROOT CART_STORE_ROOT "/.cartridge"
/* Box art of cartridge games, copied off the cartridge by the GB core so the
 * menu can show it in any core: <root>/<game file name>.cma */
#define CART_STORE_ART_ROOT CART_STORE_SIDECAR_ROOT "/ART"
static inline bool CartStore_IsOwnGamePath(const char *pPath)
{
    return pPath && strncmp(pPath, CART_STORE_OWN_ROOT "/", sizeof(CART_STORE_OWN_ROOT)) == 0;
}

enum {
    kCartStoreSlots = 10, /* version 1 directory; version 2 has no fixed limit */
    kCartStoreNameSize = 72,
    kCartStoreHeadSize = 1024,
};

typedef struct {
    char Name[kCartStoreNameSize];
    uint8_t System; /* 0 = GB, 1 = GG, 2 = Lynx (informational; the extension decides) */
    uint32_t Offset;
    uint32_t Size;
    uint32_t Crc;
    uint32_t ArtOffset; /* box art (.cma) on the cartridge, 0 for none */
    uint32_t ArtSize;
    uint32_t ArtCrc;    /* (not kept in the other cores' copy) */
} CartStoreEntry_t;

/* Power-on boot of a dedicated cart: "/cart/<name>" when the cartridge's
 * directory holds exactly one game (ESP_ERR_INVALID_SIZE for several,
 * ESP_ERR_NOT_FOUND for none). Reads the cartridge, so GB core only.
 * (Host tests that link boot_resume.c provide their own.) */
esp_err_t CartStore_AutoBootPath(char *pPath, size_t Size);

#ifdef ESP_PLATFORM
esp_err_t CartStore_Init(void);
esp_err_t CartStore_RegisterConsoleCommands(void);

bool CartStore_IsCartPath(const char *pPath);
/* "/cart/<name>" -> "/sdcard/CHROMAGIC/CART/<name>" (saves, states). */
esp_err_t CartStore_SidecarBase(const char *pPath, char *pOut, size_t Size);

/* Re-read the inserted cartridge's directory (Force) or use the cached one.
 * Returns the entry count (0: no cartridge or no Magic Trick directory). */
size_t CartStore_Refresh(bool Force);
size_t CartStore_Count(void);
/* Path of the cartridge's own game, from the directory last read: a cart with
 * Magic Trick games added after its game, or a plain cart whose header is
 * MBC5 or unbanked (false for a dedicated Magic Trick cart, or a plain cart
 * with another mapper: reading it into memory assumes MBC5 banking). */
bool CartStore_OwnGame(char *pPath, size_t Size);
/* The cart's own game played from the Chromatic's memory (picked in the
 * menu, so it has save states; power-on still boots the cartridge itself).
 * The cartridge's save chip stays the game's real save:
 *  - Import (GB core, storage mounted, before the save is loaded): the chip's
 *    save into pSavePath, unless that file holds progress not written back
 *    yet. Records the pending write-back in NVS.
 *  - Export (GB core, storage mounted): pSavePath back onto the chip, if
 *    the cartridge is still the same game; clears the record. Called when
 *    the game stops.
 *  - BootSync: the start-up hold is on (main, before releasing it): export a
 *    write-back left over from a game switched off mid-play, and copy any
 *    box art the storage now mounted is missing (a card just put in). */
esp_err_t CartStore_OwnSaveImport(const char *pSavePath, uint32_t Size);
esp_err_t CartStore_OwnSaveExport(const char *pSavePath);
void CartStore_BootSync(void);
/* GB core: restart the cartridge so its own game boots (taking over the
 * start-up hold when there is one). */
esp_err_t CartStore_RestartCart(void);
/* Whether the cartridge has anything for the Cartridge list (Magic Trick
 * games, or a game of its own: CartStore_OwnGame), from the directory last
 * read (or the copy in NVS). Never touches the cartridge. With no microSD
 * card, this gives it the internal store for saves and states. */
bool CartStore_HasGames(void);
bool CartStore_Entry(size_t Index, CartStoreEntry_t *pEntry);

/* True in the core that reads the cartridge (GB/GBC). */
bool CartStore_CanReadCart(void);

/* Staging across the core switch: Stage* run in the GB core alongside the
 * normal PSRAM upload; Replay* in the target core instead of the upload. */
bool CartStore_PsramHazard(uint32_t Address);
/* Installer access over USB (pc_sd.c), GB core: one cartridge session from
 * Begin to End (the cartridge's game is held in reset meanwhile). Offsets
 * are 1 KiB aligned; Erase takes one 64 KiB aligned block. End re-reads the
 * directory and restarts the cartridge. */
esp_err_t CartStore_FlashBegin(uint32_t *pCapacity);
esp_err_t CartStore_FlashRead(uint32_t Offset, uint8_t *pData, uint32_t Blocks);
/* Reads up to *pBlocks (lowered to what fits) into the session's own buffer,
 * valid until the next call: no extra RAM (Wi-Fi Files has little). */
esp_err_t CartStore_FlashReadShared(uint32_t Offset, uint32_t *pBlocks, const uint8_t **ppData);
esp_err_t CartStore_FlashErase(uint32_t Offset);
esp_err_t CartStore_FlashProgram(uint32_t Offset, const uint8_t *pData);
esp_err_t CartStore_FlashCrc(uint32_t Offset, uint32_t Length, uint32_t *pCrc);
/* The cartridge's save memory (battery RAM / FRAM), 1 KiB at a time; the
 * size comes from the header (32 KiB when it names none). Writes are read
 * back. */
uint32_t CartStore_FlashSaveSize(void);
esp_err_t CartStore_FlashSaveRead(uint32_t Offset, uint8_t *pData);
esp_err_t CartStore_FlashSaveWrite(uint32_t Offset, const uint8_t *pData);
size_t CartStore_FlashEnd(bool StorageMounted);
/* True from Begin until End: PC-mode owners must not re-take the cartridge. */
bool CartStore_FlashIsOpen(void);
/* GB core, storage mounted: copy box art the menu doesn't have yet off the
 * cartridge (holding it unless the caller already does). */
void CartStore_SyncArt(void);
/* One cartridge session (core held in reset) for all of a staging's reads,
 * so the cartridge's own game doesn't run between them; takes over the hold
 * from power-on/core-switch start-up (BootResume) when there is one.
 * Release (Exit true) before a core reload: the FPGA ignores one while the
 * cartridge is held. */
esp_err_t CartStore_HoldCart(void);
void CartStore_ReleaseCart(bool Exit);
esp_err_t CartStore_StageBegin(const char *pPath);
void CartStore_StageBlock(uint32_t Address, const uint8_t *pData);
/* With no microSD card the GB core keeps no full copy of the game; for a
 * Game Gear game it keeps the bytes at file offsets (Start + k) % Size,
 * k < Length (what a save or state import overwrites in the PSRAM ROM
 * mirror), and the target core reads them back with CartStore_OpenMirror. */
esp_err_t CartStore_StageMirror(uint32_t Start, uint32_t Length);
/* Lynx rotation found while staging (the target core can't read the game). */
void CartStore_StageSetRotation(uint8_t Rotation);
esp_err_t CartStore_StageCommit(uint32_t RomCrc);
uint8_t CartStore_ReplayRotation(void);
/* The kept mirror bytes of the staged game (positioned at the first), or
 * NULL; *pLength is the byte count. */
FILE *CartStore_OpenMirror(uint32_t Start, uint32_t *pLength);
bool CartStore_ReplayReady(const char *pPath, uint32_t *pRomCrc, uint32_t *pSize);
size_t CartStore_ReplayCount(void);
bool CartStore_ReplayBlock(size_t Index, uint32_t *pAddress, const uint8_t **ppData);
void CartStore_ReplayDone(void);
#else
/* Host tests: no cartridge. */
#include <stdio.h>
#include <string.h>
static inline bool CartStore_IsCartPath(const char *pPath)
{ return pPath && strncmp(pPath, CART_STORE_ROOT "/", sizeof(CART_STORE_ROOT)) == 0; }
static inline esp_err_t CartStore_SidecarBase(const char *pPath, char *pOut, size_t Size)
{
    if (!CartStore_IsCartPath(pPath)) return ESP_ERR_INVALID_ARG;
    const char *pName = CartStore_IsOwnGamePath(pPath) ? pPath + sizeof(CART_STORE_OWN_ROOT) : pPath + sizeof(CART_STORE_ROOT);
    const int Length = snprintf(pOut, Size, "%s/%s", CART_STORE_SIDECAR_ROOT, pName);
    return Length > 0 && (size_t)Length < Size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static inline size_t CartStore_Refresh(bool Force) { (void)Force; return 0; }
static inline size_t CartStore_Count(void) { return 0; }
#ifndef CART_STORE_TEST_OWN_GAME
static inline bool CartStore_OwnGame(char *pPath, size_t Size) { (void)pPath; (void)Size; return false; }
#endif
static inline esp_err_t CartStore_RestartCart(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_OwnSaveImport(const char *pSavePath, uint32_t Size) { (void)pSavePath; (void)Size; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_OwnSaveExport(const char *pSavePath) { (void)pSavePath; return ESP_OK; }
static inline void CartStore_BootSync(void) {}
static inline esp_err_t CartStore_FlashBegin(uint32_t *pCapacity) { (void)pCapacity; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_FlashRead(uint32_t Offset, uint8_t *pData, uint32_t Blocks)
{ (void)Offset; (void)pData; (void)Blocks; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_FlashErase(uint32_t Offset) { (void)Offset; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_FlashReadShared(uint32_t Offset, uint32_t *pBlocks, const uint8_t **ppData)
{ (void)Offset; (void)pBlocks; (void)ppData; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_FlashProgram(uint32_t Offset, const uint8_t *pData)
{ (void)Offset; (void)pData; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_FlashCrc(uint32_t Offset, uint32_t Length, uint32_t *pCrc)
{ (void)Offset; (void)Length; (void)pCrc; return ESP_ERR_NOT_SUPPORTED; }
static inline size_t CartStore_FlashEnd(bool StorageMounted) { (void)StorageMounted; return 0; }
static inline uint32_t CartStore_FlashSaveSize(void) { return 0; }
static inline esp_err_t CartStore_FlashSaveRead(uint32_t Offset, uint8_t *pData) { (void)Offset; (void)pData; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t CartStore_FlashSaveWrite(uint32_t Offset, const uint8_t *pData) { (void)Offset; (void)pData; return ESP_ERR_NOT_SUPPORTED; }
static inline bool CartStore_FlashIsOpen(void) { return false; }
static inline void CartStore_SyncArt(void) {}
#ifndef CART_STORE_TEST_HAS_GAMES
static inline bool CartStore_HasGames(void) { return false; }
#endif
static inline esp_err_t CartStore_StageMirror(uint32_t Start, uint32_t Length) { (void)Start; (void)Length; return ESP_ERR_NOT_SUPPORTED; }
static inline void CartStore_StageSetRotation(uint8_t Rotation) { (void)Rotation; }
static inline uint8_t CartStore_ReplayRotation(void) { return 0; }
static inline FILE *CartStore_OpenMirror(uint32_t Start, uint32_t *pLength) { (void)Start; (void)pLength; return NULL; }
static inline bool CartStore_Entry(size_t Index, CartStoreEntry_t *pEntry) { (void)Index; (void)pEntry; return false; }
static inline bool CartStore_CanReadCart(void) { return true; }
static inline esp_err_t CartStore_HoldCart(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void CartStore_ReleaseCart(bool Exit) { (void)Exit; }
static inline esp_err_t CartStore_StageBegin(const char *pPath) { (void)pPath; return ESP_ERR_NOT_SUPPORTED; }
static inline void CartStore_StageBlock(uint32_t Address, const uint8_t *pData) { (void)Address; (void)pData; }
static inline esp_err_t CartStore_StageCommit(uint32_t RomCrc) { (void)RomCrc; return ESP_ERR_NOT_SUPPORTED; }
static inline bool CartStore_ReplayReady(const char *pPath, uint32_t *pRomCrc, uint32_t *pSize)
{ (void)pPath; (void)pRomCrc; (void)pSize; return false; }
static inline size_t CartStore_ReplayCount(void) { return 0; }
static inline bool CartStore_ReplayBlock(size_t Index, uint32_t *pAddress, const uint8_t **ppData)
{ (void)Index; (void)pAddress; (void)ppData; return false; }
static inline void CartStore_ReplayDone(void) {}
#endif
