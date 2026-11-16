/* Lines of the web ChroMagician "connect" protocol, checked against what the
 * page's connect.js parses (parseRecord, parseInfo, statusFrom, validatePath,
 * describeFailure). */
#include "pc_connect_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char Out[1024];

static void Hex(void)
{
    uint8_t Bytes[4];
    size_t Length = 0;
    assert(ConnectProto_HexEncode("\x00\x7f\xff/", 4, Out, sizeof(Out)) == 8);
    assert(strcmp(Out, "007fff2f") == 0);
    assert(ConnectProto_HexEncode("ab", 2, Out, 4) == 0);           /* no room for NUL */
    assert(ConnectProto_HexDecode("007FfF2f", Bytes, sizeof(Bytes), &Length));
    assert(Length == 4 && memcmp(Bytes, "\x00\x7f\xff/", 4) == 0);
    assert(!ConnectProto_HexDecode("abc", Bytes, sizeof(Bytes), &Length)); /* odd */
    assert(!ConnectProto_HexDecode("zz", Bytes, sizeof(Bytes), &Length));
    assert(!ConnectProto_HexDecode("0011223344", Bytes, sizeof(Bytes), &Length)); /* too long */

    char Text[8];
    assert(ConnectProto_HexText("2f4742", Text, sizeof(Text)) && strcmp(Text, "/GB") == 0);
    assert(ConnectProto_HexText("", Text, sizeof(Text)) && Text[0] == '\0');
    assert(!ConnectProto_HexText("2f0041", Text, sizeof(Text)));   /* embedded NUL */
    assert(!ConnectProto_HexText("4142434445464748", Text, sizeof(Text))); /* 8 + NUL */
}

static void Fields(void)
{
    char *Argv[] = {"connect", "put", "2f612e6762", "size=1024", "crc=0000abcd", "sizex=1"};
    assert(strcmp(ConnectProto_Field(6, Argv, 3, "size"), "1024") == 0);
    assert(strcmp(ConnectProto_Field(6, Argv, 3, "crc"), "0000abcd") == 0);
    assert(ConnectProto_Field(6, Argv, 3, "rtccrc") == NULL);
    assert(ConnectProto_Field(6, Argv, 5, "size") == NULL);         /* "sizex=" is not "size=" */

    uint32_t Value;
    assert(ConnectProto_ParseU32("4294967295", 10, &Value) && Value == UINT32_MAX);
    assert(!ConnectProto_ParseU32("4294967296", 10, &Value));
    assert(ConnectProto_ParseU32("ffffffff", 16, &Value) && Value == UINT32_MAX);
    assert(!ConnectProto_ParseU32("-1", 10, &Value));
    assert(!ConnectProto_ParseU32("", 10, &Value));
    assert(!ConnectProto_ParseU32(NULL, 10, &Value));
    assert(!ConnectProto_ParseU32("12x", 10, &Value));
}

static void Paths(void)
{
    assert(ConnectProto_ValidPath("/", true));
    assert(!ConnectProto_ValidPath("/", false));
    assert(ConnectProto_ValidPath("/GB/Tetris (World).gb", false));
    assert(ConnectProto_ValidPath("/CHROMAGIC/BACKUPS", false));
    assert(!ConnectProto_ValidPath("GB", false));
    assert(!ConnectProto_ValidPath("/GB/", false));                /* empty part */
    assert(!ConnectProto_ValidPath("//GB", false));
    assert(!ConnectProto_ValidPath("/GB./x", false));               /* ends in '.' */
    assert(!ConnectProto_ValidPath("/GB /x", false));               /* ends in space */
    assert(!ConnectProto_ValidPath("/a:b", false));
    assert(!ConnectProto_ValidPath("/a\\b", false));
    assert(!ConnectProto_ValidPath("/a\x01" "b", false));
    char Long[800];
    Long[0] = '/';
    memset(Long + 1, 'a', 256);
    Long[257] = '\0';
    assert(!ConnectProto_ValidPath(Long, false));                   /* part > 255 */
    Long[256] = '\0';
    assert(ConnectProto_ValidPath(Long, false));                    /* part == 255 */
    /* "/" + 255 + "/" + 255 + "/" + 247 = 759 bytes */
    Long[256] = '/';
    memset(Long + 257, 'b', 255);
    Long[512] = '/';
    memset(Long + 513, 'c', 246);
    Long[759] = '\0';
    assert(strlen(Long) == 759 && ConnectProto_ValidPath(Long, false));
    Long[759] = 'c'; Long[760] = '\0';
    assert(!ConnectProto_ValidPath(Long, false));                   /* >= 760 bytes */
}

static void Status(void)
{
    ConnectStatus_t S = { .Enabled = true, .Active = true, .SD = true, .Player = 2,
                          .Version = "MagicTrick-1.0.0-beta.8" };
    assert(ConnectProto_FormatStatus(Out, sizeof(Out), &S));
    assert(strcmp(Out, "CONNECT STATUS enabled=1 active=1 transfer=0 sd=1 proto=1 "
                       "version=MagicTrick-1.0.0-beta.8 player=2\n") == 0);
    S = (ConnectStatus_t){ .Version = "", .Error = "No cart" };
    assert(ConnectProto_FormatStatus(Out, sizeof(Out), &S));
    assert(strcmp(Out, "CONNECT STATUS enabled=0 active=0 transfer=0 sd=0 proto=1 "
                       "version=unknown player=0 error=4e6f2063617274\n") == 0);
    assert(ConnectProto_FormatStatus(Out, 20, &S) == 0 && Out[0] == '\0');
}

static void Info(void)
{
    ConnectCartInfo_t I = { .Title = "POKEMON_SLV", .Type = 0x10, .Color = true,
                            .RomSize = 2 * 1024 * 1024, .SaveSize = 32768, .Rtc = true };
    for (int i = 0; i < kConnectHeaderSize; ++i) I.Header[i] = (uint8_t)i;
    I.Header[0x4c] = 1; I.Header[0x4d] = 0x8a; I.Header[0x4e] = 0xbe; I.Header[0x4f] = 0xef;
    const size_t Length = ConnectProto_FormatInfo(Out, sizeof(Out), &I);
    assert(Length > 0 && Out[Length - 1] == '\n');
    const char *Expect = "CONNECT INFO cart=1 title=504f4b454d4f4e5f534c56 type=10 color=1 "
        "rom=2097152 sav=32768 rtc=1 version=1 hdrsum=8a gsum=beef header=";
    assert(strncmp(Out, Expect, strlen(Expect)) == 0);
    const char *pHeader = Out + strlen(Expect);
    assert(strncmp(pHeader, "000102030405", 12) == 0);
    assert(strlen(pHeader) == 2 * kConnectHeaderSize + 1);           /* hex + "\n" */
    assert(strncmp(pHeader + 2 * 0x4c, "018abeef\n", 9) == 0);

    I.Title[0] = '\0';
    assert(ConnectProto_FormatInfo(Out, sizeof(Out), &I));
    assert(strncmp(Out, "CONNECT INFO cart=1 title= type=10 ", 35) == 0);
    assert(ConnectProto_FormatInfo(Out, 100, &I) == 0);

    assert(ConnectProto_FormatNoCart(Out, sizeof(Out)));
    assert(strcmp(Out, "CONNECT INFO cart=0\n") == 0);
}

static void Records(void)
{
    /* The page compares detail lower-cased to hex("UNKNOWN COMMAND") to
     * finish a resync. */
    assert(ConnectProto_FormatFail(Out, sizeof(Out), "ESP_ERR_NOT_SUPPORTED", CONNECT_DETAIL_UNKNOWN));
    assert(strcmp(Out, "CONNECT FAIL error=ESP_ERR_NOT_SUPPORTED "
                       "detail=554e4b4e4f574e20434f4d4d414e44\n") == 0);
    assert(ConnectProto_FormatFail(Out, sizeof(Out), "ESP_FAIL", NULL));
    assert(strcmp(Out, "CONNECT FAIL error=ESP_FAIL\n") == 0);
    /* describeFailure: detail.slice(5, 7) is the type, slice(8) the mapper. */
    assert(ConnectProto_FormatFail(Out, sizeof(Out), "ESP_ERR_NOT_SUPPORTED", "TYPE FC Game Boy Camera"));
    assert(strstr(Out, "detail=5459504520464320") != NULL);

    assert(ConnectProto_FormatEntry(Out, sizeof(Out), "Tetris (World).gb", false, 32768));
    assert(strcmp(Out, "CONNECT ENTRY name=5465747269732028576f726c64292e6762 dir=0 size=32768\n") == 0);
    assert(ConnectProto_FormatEntry(Out, sizeof(Out), "GB", true, 0));
    assert(strcmp(Out, "CONNECT ENTRY name=4742 dir=1 size=0\n") == 0);
    assert(ConnectProto_FormatEntry(Out, 40, "a long name that will not fit", false, 0) == 0);
}

int main(void)
{
    Hex();
    Fields();
    Paths();
    Status();
    Info();
    Records();
    puts("pc_connect_proto_test: ok");
    return 0;
}
