#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Boot a Lynx cartridge without the Atari boot ROM: decrypt the encrypted
 * loader at the start of the cartridge (RSA with the published public key,
 * exponent 3, as the boot ROM does) and build a 512-byte replacement boot ROM
 * (lynx_hle_boot.h) that copies it to $0200 and runs it.
 * Cart: the cartridge image from its first byte (after any .lnx header). */
esp_err_t LynxHle_Decrypt(const uint8_t *Cart, size_t Size, uint8_t Loader[256], size_t *pLength);
esp_err_t LynxHle_BuildBoot(const uint8_t *Cart, size_t Size, uint8_t Boot[512]);
/* Two-stage loaders (Alien vs Predator) jump to the boot ROM's $FE4A to decrypt
 * a second encrypted chunk. If the first loader does, decrypt the chunk that
 * follows it and return the bytes to write over it in the uploaded image
 * (count byte, then per block 50 plaintext bytes and one padding byte), which
 * the stub's $FE4A copies. ESP_ERR_NOT_FOUND: no second stage. */
enum { kLynxHleStage2Max = 1 + 51 * 5 };
esp_err_t LynxHle_BuildStage2(const uint8_t *Cart, size_t Size, size_t *pOffset,
                              uint8_t Patch[kLynxHleStage2Max], size_t *pLength);
