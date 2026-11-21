#pragma once

/* Magic Trick carts: Game Gear and Lynx games stored on a reflashable Game
 * Boy cartridge (ModRetro carts and other AMD-flash MBC5 carts), loaded into
 * PSRAM like an SD game.
 *
 * Layout (1 KiB blocks, MBC5 linear offsets):
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
static inline bool CartStore_IsOwnGamePath(const char *pPath)
{
    return pPath && strncmp(pPath, CART_STORE_OWN_ROOT "/", sizeof(CART_STORE_OWN_ROOT)) == 0;
}

enum {
    kCartStoreSlots = 10,
    kCartStoreNameSize = 72,
    kCartStoreHeadSize = 1024,
};

typedef struct {
    char Name[kCartStoreNameSize];
    uint8_t System; /* 0 = GB, 1 = GG, 2 = Lynx (informational; the extension decides) */
    uint32_t Offset;
    uint32_t Size;
    uint32_t Crc;
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
/* Path of the cartridge's own game, from the directory last read (false for a
 * dedicated Magic Trick cart or a cart without added games). */
bool CartStore_OwnGame(char *pPath, size_t Size);
/* GB core: restart the cartridge so its own game boots (taking over the
 * start-up hold when there is one). */
esp_err_t CartStore_RestartCart(void);
/* Whether the cartridge carries Magic Trick games, from the directory last
 * read (or the copy in NVS). Never touches the cartridge. */
bool CartStore_HasGames(void);
bool CartStore_Entry(size_t Index, CartStoreEntry_t *pEntry);

/* True in the core that reads the cartridge (GB/GBC). */
bool CartStore_CanReadCart(void);

/* Staging across the core switch: Stage* run in the GB core alongside the
 * normal PSRAM upload; Replay* in the target core instead of the upload. */
bool CartStore_PsramHazard(uint32_t Address);
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
    const int Length = snprintf(pOut, Size, "%s/%s", CART_STORE_SIDECAR_ROOT, pPath + sizeof(CART_STORE_ROOT));
    return Length > 0 && (size_t)Length < Size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static inline size_t CartStore_Refresh(bool Force) { (void)Force; return 0; }
static inline size_t CartStore_Count(void) { return 0; }
#ifndef CART_STORE_TEST_OWN_GAME
static inline bool CartStore_OwnGame(char *pPath, size_t Size) { (void)pPath; (void)Size; return false; }
#endif
static inline esp_err_t CartStore_RestartCart(void) { return ESP_ERR_NOT_SUPPORTED; }
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
