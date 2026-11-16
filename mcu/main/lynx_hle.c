#include "lynx_hle.h"
#include "lynx_hle_boot.h"
#include <stdbool.h>
#include <string.h>

enum { kBlock = 51, kMaxBlocks = 5 };
/* Atari Lynx cartridge public modulus, most significant byte first. */
static const uint8_t Modulus[kBlock] = {
    0x35, 0xB5, 0xA3, 0x94, 0x28, 0x06, 0xD8, 0xA2, 0x26, 0x95, 0xD7, 0x71, 0xB2,
    0x3C, 0xFD, 0x56, 0x1C, 0x4A, 0x19, 0xB6, 0xA3, 0xB0, 0x26, 0x00, 0x36, 0x5A,
    0x30, 0x6E, 0x3C, 0x4D, 0x63, 0x38, 0x1B, 0xD4, 0x1C, 0x13, 0x64, 0x89, 0x36,
    0x4C, 0xF2, 0xBA, 0x2A, 0x58, 0xF4, 0xFE, 0xE1, 0xFD, 0xAC, 0x7E, 0x79,
};

/* Big-endian 51-byte arithmetic modulo Modulus (values stay below 2^408). */
static int Compare(const uint8_t *A, const uint8_t *B)
{
    for (int i = 0; i < kBlock; ++i)
        if (A[i] != B[i]) return A[i] < B[i] ? -1 : 1;
    return 0;
}
static void Subtract(uint8_t *A, const uint8_t *B)
{
    int Borrow = 0;
    for (int i = kBlock - 1; i >= 0; --i) {
        const int D = A[i] - B[i] - Borrow;
        A[i] = (uint8_t)D;
        Borrow = D < 0;
    }
}
static void AddMod(uint8_t *A, const uint8_t *B)
{
    unsigned Carry = 0;
    for (int i = kBlock - 1; i >= 0; --i) {
        const unsigned S = A[i] + B[i] + Carry;
        A[i] = (uint8_t)S;
        Carry = S >> 8;
    }
    if (Compare(A, Modulus) >= 0) Subtract(A, Modulus);
}
/* R = A * B mod Modulus, bit-serial double-and-add. */
static void MulMod(uint8_t *R, const uint8_t *A, const uint8_t *B)
{
    uint8_t Acc[kBlock] = {0};
    for (int i = 0; i < kBlock; ++i)
        for (int Bit = 7; Bit >= 0; --Bit) {
            AddMod(Acc, Acc);
            if ((A[i] >> Bit) & 1) AddMod(Acc, B);
        }
    memcpy(R, Acc, kBlock);
}

esp_err_t LynxHle_Decrypt(const uint8_t *Cart, size_t Size, uint8_t Loader[256], size_t *pLength)
{
    if (!Cart || !Loader || !pLength || Size < 1) return ESP_ERR_INVALID_ARG;
    const unsigned Blocks = 256u - Cart[0];
    if (Blocks < 1 || Blocks > kMaxBlocks || Size < 1 + (size_t)kBlock * Blocks) return ESP_ERR_INVALID_RESPONSE;
    memset(Loader, 0, 256);
    uint8_t Accumulator = 0;
    size_t Out = 0;
    for (unsigned b = 0; b < Blocks; ++b) {
        uint8_t C[kBlock], M[kBlock], P[kBlock];
        /* Blocks are stored least significant byte first. */
        for (int i = 0; i < kBlock; ++i) C[i] = Cart[1 + kBlock * b + (kBlock - 1 - i)];
        if (Compare(C, Modulus) >= 0) return ESP_ERR_INVALID_RESPONSE;
        MulMod(M, C, C);
        MulMod(P, M, C);
        /* 50 bytes per block, least significant first, through a running sum. */
        for (int i = kBlock - 1; i >= 1; --i) {
            Accumulator = (uint8_t)(Accumulator + P[i]);
            Loader[Out++] = Accumulator;
        }
    }
    *pLength = Out;
    return ESP_OK;
}

esp_err_t LynxHle_BuildStage2(const uint8_t *Cart, size_t Size, size_t *pOffset,
                              uint8_t Patch[kLynxHleStage2Max], size_t *pLength)
{
    if (!Cart || !pOffset || !Patch || !pLength) return ESP_ERR_INVALID_ARG;
    uint8_t Loader[256], Second[256];
    size_t Length = 0, SecondLength = 0;
    esp_err_t Result = LynxHle_Decrypt(Cart, Size, Loader, &Length);
    if (Result != ESP_OK) return Result;
    bool Calls = false;
    for (size_t i = 0; i + 2 < Length; ++i)
        if ((Loader[i] == 0x4c || Loader[i] == 0x20) && Loader[i + 1] == 0x4a && Loader[i + 2] == 0xfe) Calls = true;
    if (!Calls) return ESP_ERR_NOT_FOUND;
    /* The second chunk starts where the first ends (the loader reads nothing
     * before calling $FE4A); its count byte is again 256 - blocks. */
    const size_t Offset = 1u + kBlock * (256u - Cart[0]);
    if (Offset >= Size) return ESP_ERR_INVALID_SIZE;
    Result = LynxHle_Decrypt(Cart + Offset, Size - Offset, Second, &SecondLength);
    if (Result != ESP_OK) return Result;
    const unsigned Blocks = 256u - Cart[Offset];
    size_t Out = 0;
    Patch[Out++] = Cart[Offset];
    for (unsigned b = 0; b < Blocks; ++b) {
        memcpy(Patch + Out, Second + 50u * b, 50);
        Out += 50;
        Patch[Out++] = 0;
    }
    *pOffset = Offset;
    *pLength = Out;
    return ESP_OK;
}

esp_err_t LynxHle_BuildBoot(const uint8_t *Cart, size_t Size, uint8_t Boot[512])
{
    if (!Boot) return ESP_ERR_INVALID_ARG;
    uint8_t Loader[256];
    size_t Length = 0;
    const esp_err_t Result = LynxHle_Decrypt(Cart, Size, Loader, &Length);
    if (Result != ESP_OK) return Result;
    memcpy(Boot, kLynxHleBoot, sizeof(kLynxHleBoot));
    memcpy(Boot + kLynxHleLoader, Loader, kLynxHleLoaderSize);
    /* Bytes to skip past: the count byte and the encrypted blocks. The stub's
     * loop reads SKIPLO + 256 * SKIPHI bytes, with SKIPLO 0 meaning 256. */
    const unsigned Skip = 1u + kBlock * (256u - Cart[0]);
    const unsigned Lo = Skip & 0xff;
    Boot[kLynxHleSkipLo] = (uint8_t)Lo;
    Boot[kLynxHleSkipHi] = (uint8_t)((Skip >> 8) - (Lo == 0 ? 1u : 0u));
    /* The boot ROM leaves its copy pointer ($05/$06) at the loader's end;
     * two-stage loaders increment $06 to place the second stage. */
    Boot[kLynxHleLoadLen] = (uint8_t)Length;
    return ESP_OK;
}
