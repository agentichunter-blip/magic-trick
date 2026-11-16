#pragma once
/* Text side of the ChroMagic 2 "connect" serial protocol spoken by the web
 * ChroMagician (chromagician.chromagic.org). Records are single lines
 * "CONNECT <TYPE> key=value ..."; free text is hex-encoded UTF-8. A command's
 * output ends with a console line "> ok" or "> err". Raw binary follows a
 * DATA record (exactly size= bytes) or is sent by the host after a SEND.
 * No device dependencies, so the host tests can check the exact lines. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    kConnectProtoVersion = 1,
    kConnectHeaderSize = 0x50,      /* cartridge bytes 0x100..0x14F */
    kConnectTitleSize = 17,
};

typedef struct {
    bool Enabled;       /* PC mode (USB TRANSFERS) is on */
    bool Active;        /* the cartridge engine is open for the PC */
    bool Transfer;
    bool SD;
    uint8_t Player;
    const char *Version;    /* no spaces */
    const char *Error;      /* plain text, may be NULL */
} ConnectStatus_t;

typedef struct {
    char Title[kConnectTitleSize];
    uint8_t Type;
    bool Color;
    uint32_t RomSize;
    uint32_t SaveSize;
    bool Rtc;
    uint8_t Header[kConnectHeaderSize];
} ConnectCartInfo_t;

/* Lower-case hex of Size bytes; returns the length written (excluding NUL),
 * or 0 when Out is too small. */
size_t ConnectProto_HexEncode(const void *pData, size_t Size,
                              char *Out, size_t OutSize);
/* Decodes an even-length hex string into Out. */
bool ConnectProto_HexDecode(const char *pHex, uint8_t *Out, size_t OutSize,
                            size_t *pLength);
/* Decodes a hex path; it must be a NUL-free string that fits OutSize. */
bool ConnectProto_HexText(const char *pHex, char *Out, size_t OutSize);

/* Value of "Key=value" among argv[First..argc), or NULL. */
const char *ConnectProto_Field(int argc, char **argv, int First,
                               const char *pKey);
bool ConnectProto_ParseU32(const char *pText, int Base, uint32_t *pValue);

/* Path rules of the web client (validatePath): absolute, < 760 bytes, parts
 * 1..255 bytes not ending in '.' or space, none of \ : * ? " < > | or
 * control characters. "/" alone only when AllowRoot. */
bool ConnectProto_ValidPath(const char *pPath, bool AllowRoot);

/* Each formatter writes one full record line ending in "\n" and returns its
 * length, or 0 when Out is too small. */
size_t ConnectProto_FormatStatus(char *Out, size_t OutSize,
                                 const ConnectStatus_t *pStatus);
size_t ConnectProto_FormatInfo(char *Out, size_t OutSize,
                               const ConnectCartInfo_t *pInfo);
size_t ConnectProto_FormatNoCart(char *Out, size_t OutSize);
/* "CONNECT FAIL error=<Code> detail=<hex Detail>" */
size_t ConnectProto_FormatFail(char *Out, size_t OutSize, const char *pCode,
                               const char *pDetail);
size_t ConnectProto_FormatEntry(char *Out, size_t OutSize, const char *pName,
                                bool Directory, uint32_t Size);

/* The text the web client shows for well-known failures. */
#define CONNECT_DETAIL_NO_CART "NO CARTRIDGE"
#define CONNECT_DETAIL_OFF "CONNECT OFF"
#define CONNECT_DETAIL_NO_SD "NO SD CARD INSTALLED"
#define CONNECT_DETAIL_EXISTS "EXISTS"
#define CONNECT_DETAIL_NOT_FOUND "NOT FOUND"
#define CONNECT_DETAIL_SAVE_SIZE "SAVE SIZE"
#define CONNECT_DETAIL_NO_RTC "NO RTC"
#define CONNECT_DETAIL_NO_SAVE "NO SAVE"
#define CONNECT_DETAIL_UPLOAD_TIMEOUT "UPLOAD TIMEOUT"
#define CONNECT_DETAIL_UNKNOWN "UNKNOWN COMMAND"
