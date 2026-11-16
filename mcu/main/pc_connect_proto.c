#include "pc_connect_proto.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ConnectProto_HexEncode(const void *pData, size_t Size,
                              char *Out, size_t OutSize)
{
    static const char Digits[] = "0123456789abcdef";
    const uint8_t *p = pData;
    if (Out == NULL || OutSize < Size * 2 + 1) return 0;
    for (size_t i = 0; i < Size; ++i)
    {
        Out[i * 2] = Digits[p[i] >> 4];
        Out[i * 2 + 1] = Digits[p[i] & 15];
    }
    Out[Size * 2] = '\0';
    return Size * 2;
}

static int Nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ConnectProto_HexDecode(const char *pHex, uint8_t *Out, size_t OutSize,
                            size_t *pLength)
{
    if (pHex == NULL || Out == NULL) return false;
    const size_t Length = strlen(pHex);
    if (Length % 2 != 0 || Length / 2 > OutSize) return false;
    for (size_t i = 0; i < Length / 2; ++i)
    {
        const int High = Nibble(pHex[i * 2]), Low = Nibble(pHex[i * 2 + 1]);
        if (High < 0 || Low < 0) return false;
        Out[i] = (uint8_t)(High << 4 | Low);
    }
    if (pLength != NULL) *pLength = Length / 2;
    return true;
}

bool ConnectProto_HexText(const char *pHex, char *Out, size_t OutSize)
{
    size_t Length;
    if (OutSize == 0 ||
        !ConnectProto_HexDecode(pHex, (uint8_t *)Out, OutSize - 1, &Length) ||
        memchr(Out, 0, Length) != NULL)
        return false;
    Out[Length] = '\0';
    return true;
}

const char *ConnectProto_Field(int argc, char **argv, int First,
                               const char *pKey)
{
    const size_t KeyLength = strlen(pKey);
    for (int i = First; i < argc; ++i)
        if (strncmp(argv[i], pKey, KeyLength) == 0 && argv[i][KeyLength] == '=')
            return argv[i] + KeyLength + 1;
    return NULL;
}

bool ConnectProto_ParseU32(const char *pText, int Base, uint32_t *pValue)
{
    if (pText == NULL || *pText == '\0' || *pText == '-' || *pText == '+')
        return false;
    errno = 0;
    char *pEnd = NULL;
    const unsigned long long Value = strtoull(pText, &pEnd, Base);
    if (errno != 0 || *pEnd != '\0' || Value > UINT32_MAX) return false;
    *pValue = (uint32_t)Value;
    return true;
}

bool ConnectProto_ValidPath(const char *pPath, bool AllowRoot)
{
    if (pPath == NULL || pPath[0] != '/') return false;
    const size_t Length = strlen(pPath);
    if (Length >= 760) return false;
    if (Length == 1) return AllowRoot;
    size_t Start = 1;
    for (size_t i = 1; i <= Length; ++i)
    {
        if (i == Length || pPath[i] == '/')
        {
            const size_t Part = i - Start;
            if (Part == 0 || Part > 255 || pPath[i - 1] == '.' ||
                pPath[i - 1] == ' ' || pPath[i - 1] == '\t' ||
                pPath[i - 1] == '\r' || pPath[i - 1] == '\n')
                return false;
            Start = i + 1;
            continue;
        }
        const unsigned char c = (unsigned char)pPath[i];
        if (c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c) != NULL)
            return false;
    }
    return true;
}

static size_t Finish(char *Out, size_t OutSize, int Written)
{
    if (Written < 0 || (size_t)Written >= OutSize)
    {
        if (OutSize) Out[0] = '\0';
        return 0;
    }
    return (size_t)Written;
}

size_t ConnectProto_FormatStatus(char *Out, size_t OutSize,
                                 const ConnectStatus_t *p)
{
    char Error[2 * 96 + 1] = "";
    if (p->Error != NULL && p->Error[0] != '\0')
    {
        const size_t Length = strnlen(p->Error, 96);
        if (!ConnectProto_HexEncode(p->Error, Length, Error, sizeof(Error)))
            Error[0] = '\0';
    }
    return Finish(Out, OutSize, snprintf(Out, OutSize,
        "CONNECT STATUS enabled=%u active=%u transfer=%u sd=%u proto=%u "
        "version=%s player=%u%s%s\n",
        p->Enabled ? 1u : 0u, p->Active ? 1u : 0u, p->Transfer ? 1u : 0u,
        p->SD ? 1u : 0u, (unsigned)kConnectProtoVersion,
        p->Version != NULL && p->Version[0] != '\0' ? p->Version : "unknown",
        (unsigned)p->Player, Error[0] ? " error=" : "", Error));
}

size_t ConnectProto_FormatInfo(char *Out, size_t OutSize,
                               const ConnectCartInfo_t *p)
{
    char Title[2 * (kConnectTitleSize - 1) + 1];
    char Header[2 * kConnectHeaderSize + 1];
    if (!ConnectProto_HexEncode(p->Title, strnlen(p->Title, kConnectTitleSize - 1),
                                Title, sizeof(Title)) && p->Title[0] != '\0')
        return Finish(Out, OutSize, -1);
    if (p->Title[0] == '\0') Title[0] = '\0';
    ConnectProto_HexEncode(p->Header, sizeof(p->Header), Header, sizeof(Header));
    /* Header offsets are relative to 0x100: version 0x14C, header checksum
     * 0x14D, global checksum 0x14E..0x14F (big endian). */
    return Finish(Out, OutSize, snprintf(Out, OutSize,
        "CONNECT INFO cart=1 title=%s type=%02x color=%u rom=%lu sav=%lu rtc=%u "
        "version=%u hdrsum=%02x gsum=%04x header=%s\n",
        Title, p->Type, p->Color ? 1u : 0u, (unsigned long)p->RomSize,
        (unsigned long)p->SaveSize, p->Rtc ? 1u : 0u, p->Header[0x4c],
        p->Header[0x4d], (unsigned)(p->Header[0x4e] << 8 | p->Header[0x4f]),
        Header));
}

size_t ConnectProto_FormatNoCart(char *Out, size_t OutSize)
{
    return Finish(Out, OutSize, snprintf(Out, OutSize, "CONNECT INFO cart=0\n"));
}

size_t ConnectProto_FormatFail(char *Out, size_t OutSize, const char *pCode,
                               const char *pDetail)
{
    char Detail[2 * 120 + 1] = "";
    if (pDetail != NULL)
        ConnectProto_HexEncode(pDetail, strnlen(pDetail, 120), Detail,
                               sizeof(Detail));
    return Finish(Out, OutSize, snprintf(Out, OutSize,
        "CONNECT FAIL error=%s%s%s\n", pCode != NULL ? pCode : "ESP_FAIL",
        Detail[0] ? " detail=" : "", Detail));
}

size_t ConnectProto_FormatEntry(char *Out, size_t OutSize, const char *pName,
                                bool Directory, uint32_t Size)
{
    const size_t NameLength = strlen(pName);
    if (OutSize < NameLength * 2 + 64) return Finish(Out, OutSize, -1);
    int Written = snprintf(Out, OutSize, "CONNECT ENTRY name=");
    Written += (int)ConnectProto_HexEncode(pName, NameLength, Out + Written,
                                           OutSize - (size_t)Written);
    Written += snprintf(Out + Written, OutSize - (size_t)Written,
                        " dir=%u size=%lu\n", Directory ? 1u : 0u,
                        (unsigned long)Size);
    return Finish(Out, OutSize, Written);
}
