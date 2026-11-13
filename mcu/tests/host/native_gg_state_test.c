// Game Gear save states: the production virtual_cart.c flow against a modelled
// gg_state_engine (commands 10/11/12/14/16) and a temporary directory standing
// in for the SD card. No hardware is opened.
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include "../../main/cart_link.h"
#define NATIVE_TEST_SLOT_HOOKS
#define NATIVE_TEST_RAM_SIZE 8192
static bool NativeCommandHook(uint8_t cmd, uint16_t conf, uint8_t high, CartLinkResponse_t *r);
static bool NativeReadBlockHook(uint8_t *data);
static void NativeWritePsramHook(uint32_t address, const uint8_t *data);
#define main native_fixture_main
#include "native_state_test.c"
#undef main

enum { GGPayload = 41 * 1024, Stage = 0x020000 + 0x3F0000, RomBytes = 64 * 1024 };
static uint8_t Live[GGPayload], Staged[GGPayload], Mirror[GGPayload], Rom[RomBytes];
static bool GGFrozen, GGLoaded, GGFault, NoEngine, FailGGLoad, SlotSource, MirrorWritten;
static unsigned Freezes, Releases, Loads;

static void SetLive(uint8_t seed)
{
    for (unsigned i = 0; i < GGPayload; i++) Live[i] = (uint8_t)(i * 13 + seed + (i >> 10));
    memset(Live, 0, 8); Live[4] = Live[5] = 0x67; Live[6] = 1;
}
static void Status(CartLinkResponse_t *r)
{
    const uint32_t s = 0x53530008u | (GGFrozen ? 2 : 0) | (GGFault ? 4 : 0) | (GGLoaded ? 0x10 : 0);
    for (int i = 0; i < 4; i++) r->Data[i] = s >> (8 * i);
}
static bool NativeCommandHook(uint8_t cmd, uint16_t conf, uint8_t high, CartLinkResponse_t *r)
{
    /* Game Gear never quiesces, pauses or uses the Game Boy import command. */
    assert(cmd != 8 && cmd != 9 && cmd != 15 && cmd != 4);
    if (NoEngine && cmd >= 10) { r->Status = 1; return true; }
    switch (cmd)
    {
    case 12: Status(r); return true;
    case 10: assert(!GGFrozen); GGFrozen = true; GGLoaded = GGFault = false; ++Freezes; Status(r); return true;
    case 14: assert(GGFrozen); Block = high; Sequence = conf; SlotSource = true; Status(r); return true;
    case 11:
        assert(GGFrozen); ++Loads; GGFault = false;
        if (FailGGLoad || Staged[4] != 0x67 || Staged[5] != 0x67) GGFault = true;
        else { memcpy(Live, Staged, sizeof(Live)); GGLoaded = true; }
        Status(r); return true;
    case 16: assert(GGFrozen); GGFrozen = false; ++Releases; Status(r); return true;
    default: return false;
    }
}
static bool NativeReadBlockHook(uint8_t *data)
{
    if (!SlotSource) return false;
    memcpy(data, Live + Block * 1024, 1024);
    return true;
}
static void NativeWritePsramHook(uint32_t address, const uint8_t *data)
{
    assert(GGFrozen);
    assert(address >= Stage && address < Stage + GGPayload);
    if (!GGLoaded) memcpy(Staged + (address - Stage), data, 1024);
    else { memcpy(Mirror + (address - Stage), data, 1024); MirrorWritten = true; }
}
static long FileSize(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }
static void Idle(void) { assert(!Mounted && !Bulk && !Running && !GGFrozen); }

int main(void)
{
    char dir[] = "/tmp/native-gg-state-XXXXXX"; assert(mkdtemp(dir));
    char rom[256]; snprintf(rom, sizeof(rom), "%s/game.gg", dir);
    for (unsigned i = 0; i < RomBytes; i++) Rom[i] = (uint8_t)(i * 7 + (i >> 9));
    FILE *f = fopen(rom, "wb"); assert(f && fwrite(Rom, 1, RomBytes, f) == RomBytes); fclose(f);

    Active = true;
    ActiveInfo.GameGear = true; ActiveInfo.RomSize = RomBytes; ActiveInfo.GlobalChecksum = 0xbeef;
    snprintf(ActiveInfo.Title, sizeof(ActiveInfo.Title), "GAME GEAR");
    snprintf(ActiveRomPath, sizeof(ActiveRomPath), "%s", rom);
    for (unsigned i = 0; i < 3; i++) assert(MakeStatePath(rom, i + 1, ActiveStatePath[i]) == ESP_OK);
    assert(!strcmp(ActiveStatePath[1] + strlen(ActiveStatePath[1]) - 7, ".state2"));

    /* An older Game Gear core without the engine: not supported, no error. */
    bool supported = true, uncertain = true; uint8_t mask = 0xff; uint32_t session = 0;
    NoEngine = true;
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && !supported && !mask);
    assert(VirtualCart_StateCaptureSlot(false, 1, 0) != ESP_OK); Idle();
    NoEngine = false;
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && supported && !mask);

    /* No file yet: Load is refused without freezing. */
    assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_ERR_INVALID_STATE && Freezes == 0); Idle();

    /* Save slot 2: freeze, 41 blocks to "<rom>.state2", release. */
    SetLive(0x10);
    uint8_t saved[GGPayload]; memcpy(saved, Live, sizeof(saved));
    assert(VirtualCart_StateCaptureSlot(false, 2, 0) == ESP_OK); Idle();
    assert(Freezes == 1 && Releases == 1 && !VirtualCart_LastStateSdFailed());
    assert(FileSize(ActiveStatePath[1]) == kStateFileHeaderSize + GGPayload);
    {
        FILE *s = fopen(ActiveStatePath[1], "rb"); assert(s);
        uint8_t header[kStateFileHeaderSize]; assert(fread(header, 1, sizeof(header), s) == sizeof(header));
        static uint8_t payload[GGPayload]; assert(fread(payload, 1, GGPayload, s) == GGPayload); fclose(s);
        StateFileGame_t game; ActiveStateGame(&game);
        assert(game.Engine == kStateEngine_GameGear && game.RamSize == 0);
        uint32_t len = 0, crc = 0;
        assert(StateFile_DecodeHeader(header, &game, &len, &crc) == kStateFile_Ok && len == GGPayload);
        assert(crc == StateFile_Crc32(0, payload, GGPayload) && !memcmp(payload, saved, GGPayload));
        /* The same identity on the Game Boy engine is a different engine. */
        game.Engine = kStateEngine_GameBoy;
        assert(StateFile_DecodeHeader(header, &game, &len, &crc) == kStateFile_BadEngine);
    }
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && mask == 2);

    /* Play on, then load slot 2: the saved state returns and the ROM mirror under
     * the staging area is rewritten from the ROM file. */
    SetLive(0x20);
    assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_OK); Idle();
    assert(Loads == 1 && !memcmp(Live, saved, GGPayload) && !StateUncertain);
    assert(MirrorWritten);
    for (unsigned i = 0; i < GGPayload; i++) assert(Mirror[i] == Rom[(0x3F0000 + i) % RomBytes]);

    /* A damaged file is refused before the FPGA is asked to load; the game is released unchanged. */
    {
        FILE *s = fopen(ActiveStatePath[1], "r+b"); assert(s);
        assert(!fseek(s, kStateFileHeaderSize + 30000, SEEK_SET));
        int c = fgetc(s); assert(c != EOF); assert(!fseek(s, -1, SEEK_CUR)); fputc(c ^ 0x02, s); fclose(s);
    }
    SetLive(0x30); uint8_t before[GGPayload]; memcpy(before, Live, sizeof(before));
    unsigned loads = Loads;
    assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_ERR_INVALID_CRC); Idle();
    assert(Loads == loads && !memcmp(Live, before, GGPayload) && !StateUncertain);

    /* Save again into slot 2 (repairs it) and into slot 3; a different ROM is refused. */
    assert(VirtualCart_StateCaptureSlot(false, 2, 0) == ESP_OK); Idle();
    SetLive(0x40);
    assert(VirtualCart_StateCaptureSlot(false, 3, 0) == ESP_OK); Idle();
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && mask == 6);
    ActiveInfo.GlobalChecksum ^= 1;
    assert(VirtualCart_StateCaptureSlot(true, 3, 0) == ESP_ERR_INVALID_VERSION); Idle();
    ActiveInfo.GlobalChecksum ^= 1;
    assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_OK); Idle();
    assert(!memcmp(Live, before, GGPayload));

    /* If the FPGA faults after the payload was submitted, the game stays frozen
     * and the session is marked for recovery (power off), as on Game Boy. */
    FailGGLoad = true;
    assert(VirtualCart_StateCaptureSlot(true, 3, 0) == ESP_FAIL);
    assert(GGFrozen && StateUncertain && !Mounted && !Bulk && !Running);
    assert(VirtualCart_StateCaptureSlot(false, 1, 0) == ESP_ERR_INVALID_STATE);
    FailGGLoad = false; StateUncertain = false; GGFrozen = false;

    DIR *d = opendir(dir); assert(d);
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') { char p[512]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name); unlink(p); }
    closedir(d); assert(!rmdir(dir));
    puts("PASS Game Gear save states: no-engine fallback, slot files, exact save/load, ROM mirror repair, damage/identity/engine refusal, FPGA fault lockout");
    return 0;
}
