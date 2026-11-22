#pragma once
#include "esp_err.h"
#include "last_game.h"
#include <stdbool.h>
#include <stdint.h>

/* FPGA cores, by multiboot version minor (major 50). */
typedef enum {
    kCoreSystem_None = 0,
    kCoreSystem_GB = 1,   /* Game Boy / Game Boy Color, flash 0x000000 */
    kCoreSystem_GG = 2,   /* Game Gear, flash 0x100000 */
    kCoreSystem_Lynx = 3, /* Atari Lynx, flash 0x200000 */
} CoreSystem_t;

/* Only the explicitly qualified multiboot protocol advertises major 50. */
void CoreSwitch_ObserveVersion(uint8_t Major, uint8_t Minor);
bool CoreSwitch_IsSupported(void);
bool CoreSwitch_IsTarget(bool GameGear);
bool CoreSwitch_IsCore(CoreSystem_t System);
CoreSystem_t CoreSwitch_Current(void);
/* The core a game file needs (.gg/.sms Game Gear; .lnx/.lyx/.o Lynx; else GB). */
CoreSystem_t CoreSwitch_SystemForPath(const char *Path);
/* A reload boots the running image's static next slot, so the cores form a
 * cycle GB -> GG -> Lynx -> GB; reaching a core can take two reloads. */
CoreSystem_t CoreSwitch_NextCore(CoreSystem_t System);
/* Caller must have saved active battery data, stopped the core, closed files,
 * released bulk transport and unmounted SD. Success resets the device. */
esp_err_t CoreSwitch_Request(const char *Path);
/* Land on Target instead of the game's own core first (the game is then
 * launched there, e.g. GB to stage a cartridge game; see cart_store.h). */
esp_err_t CoreSwitch_RequestVia(const char *Path, CoreSystem_t Target);
/* After a reload landed on an intermediate core: reload again, at most
 * kCoreSwitchMaxHops reloads per request. */
enum { kCoreSwitchMaxHops = 2 };
esp_err_t CoreSwitch_Continue(const char *Path, uint8_t Hops);
esp_err_t CoreSwitch_ContinueTo(const char *Path, uint8_t Hops, CoreSystem_t Target);
/* Consume before launching, including failure paths, to prevent reboot loops. */
esp_err_t CoreSwitch_TakePending(char Path[LAST_GAME_PATH_SIZE]);
/* Same, and how many reloads the request has used so far (1 after the first). */
esp_err_t CoreSwitch_TakePendingHops(char Path[LAST_GAME_PATH_SIZE], uint8_t *pHops);
/* Same, and the core the request is for (its via core, or the game's own). */
esp_err_t CoreSwitch_TakePendingTarget(char Path[LAST_GAME_PATH_SIZE], uint8_t *pHops, CoreSystem_t *pTarget);
/* Diagnostics: store a pending launch without reloading (taken at the next
 * boot if that core is running, e.g. after loading a core over JTAG). */
esp_err_t CoreSwitch_SetPending(const char *Path);
/* True when a switch request is stored (does not consume it). */
bool CoreSwitch_HasPending(void);
/* Reads the pending game path without consuming it (start-up loading card). */
bool CoreSwitch_PeekPending(char *Path, size_t Size);
