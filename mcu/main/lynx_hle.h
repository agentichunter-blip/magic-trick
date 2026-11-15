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
