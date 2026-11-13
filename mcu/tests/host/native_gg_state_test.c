// Game Gear save states: the production virtual_cart.c flow against a modelled
// gg_state_engine (commands 10/11/12/14/16) and a temporary directory standing
// in for the SD card. No hardware is opened.
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include <strings.h>
#include "../../main/cart_link.h"
#define NATIVE_TEST_SLOT_HOOKS
#define NATIVE_TEST_RAM_SIZE 8192
static bool NativeCommandHook(uint8_t cmd, uint16_t conf, uint8_t high, CartLinkResponse_t *r);
static bool NativeReadBlockHook(uint8_t *data);
static void NativeWritePsramHook(uint32_t address, const uint8_t *data);
#define main native_fixture_main
#include "native_state_test.c"
#undef main

enum { GGPayload = 57 * 1024, Cart = 32 * 1024, Stage = 0x020000 + 0x3F0000, RomBytes = 64 * 1024 };
static uint8_t Live[GGPayload], Staged[GGPayload], Mirror[GGPayload], Rom[RomBytes];
static bool GGFrozen, GGLoaded, GGFault, NoEngine, FailGGLoad, SlotSource, MirrorWritten;
static bool CartSupport, CartDirty, Armed;
/* Per-1 KiB-block write flags (commands 18/19); DuringMask is written by the
 * game while block 20 is being copied. */
static uint32_t Written, Snap, DuringMask;
static void GameWrites(uint32_t blocks) { Written |= blocks; CartDirty = true; }
static unsigned Freezes, Releases, Loads, CartReads;
static uint8_t Imported[32 * 1024];

static void SetLive(uint8_t seed)
{
    for (unsigned i = 0; i < GGPayload; i++) Live[i] = (uint8_t)(i * 13 + seed + (i >> 10));
    memset(Live, 0, 8); Live[4] = Live[5] = 0x67; Live[6] = 2;
}
static void Status(CartLinkResponse_t *r)
{
    const uint32_t s = 0x53530008u | (GGFrozen ? 2 : 0) | (GGFault ? 4 : 0) | (GGLoaded ? 0x10 : 0) |
        (CartDirty ? 0x20 : 0) | (CartSupport ? 0x40 : 0);
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
    case 14:
        /* Cartridge RAM (blocks 9-24) can be read while the game runs. */
        assert(GGFrozen || (high >= 9 && high <= 40));
        if (!GGFrozen && high == 9) { Snap = Written; Written = 0; CartDirty = false; }
        if (!GGFrozen && high == 20 && DuringMask) { GameWrites(DuringMask); DuringMask = 0; }
        if (high >= 9 && high <= 40) ++CartReads;
        Block = high; Sequence = conf; SlotSource = true; Status(r); return true;
    case 17: assert(!GGFrozen && CartSupport && high == 1); Armed = true; Status(r); return true;
    case 18: case 19:
    {
        const uint32_t m = cmd == 18 ? Written : Snap;
        for (int i = 0; i < 4; i++) r->Data[i] = (uint8_t)(m >> (8 * i));
        return true;
    }
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
    assert(address >= Stage && address < Stage + GGPayload);
    if (!GGFrozen)
    {
        /* Battery save import staging: only the cartridge RAM blocks. */
        assert(address >= Stage + 9 * 1024 && address < Stage + 41 * 1024);
        memcpy(Imported + (address - Stage - 9 * 1024), data, 1024);
        return;
    }
    if (!GGLoaded) memcpy(Staged + (address - Stage), data, 1024);
    else { memcpy(Mirror + (address - Stage), data, 1024); MirrorWritten = true; }
}
static long FileSize(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }
static void Idle(void) { assert(!Mounted && !Bulk && !Running && !GGFrozen); }
/* Game start and the autosave task hold the SD card and the bulk link. */
static sdmmc_card_t *Sd;
static void Open(void) { assert(SDCard_Mount(&Sd) == ESP_OK && CartBulk_Begin() == ESP_OK); }
static void Close(void) { CartBulk_End(); assert(SDCard_Unmount(Sd) == ESP_OK); }

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

    /* Battery saves. An older core without support: nothing armed or saved. */
    {
        char sel[kPathSize]; bool armed = true, enabled = true; uint32_t size = 0;
        Open();
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_OK && !armed && !enabled && !Armed);
        CartSupport = true;
        /* No save yet: a new 32 KiB .SAV, nothing imported. */
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_OK && !armed && enabled && size == Cart && !Armed);
        snprintf(ActiveSavePath, sizeof(ActiveSavePath), "%s", sel); GGCartSave = true; GGCartFileSize = size;
        /* Clean RAM is not written; a game write is (blocks 9-40 while running). */
        ActiveSavePending = false; CartDirty = false;
        assert(PersistActiveSave() == ESP_OK && FileSize(ActiveSavePath) == -1);
        SetLive(0x50); GameWrites(0xffffffffu); CartReads = 0;
        assert(PersistActiveSave() == ESP_OK && !GGFrozen && CartReads == 33 && !CartDirty && !ActiveSavePending);
        static uint8_t file[Cart];
        FILE *f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, Live + 9 * 1024, Cart));
        /* A write during the copy: not published, retried later. */
        static uint8_t prev[Cart]; memcpy(prev, file, sizeof(prev));
        SetLive(0x51); GameWrites(0xffffffffu); DuringMask = 1u << 11;
        assert(PersistActiveSave() == ESP_ERR_NOT_FINISHED && GGPendingBlocks);
        f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, prev, Cart));
        assert(PersistActiveSave() == ESP_OK && !ActiveSavePending);
        f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, Live + 9 * 1024, Cart));

        /* Saves from other emulators keep their size; smaller ones repeat
         * through the 32 KiB like a mirrored chip. 8 KiB: */
        for (unsigned i = 0; i < 8192; i++) file[i] = (uint8_t)(i ^ 0x5a);
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(file, 1, 8192, f2) == 8192); fclose(f2);
        Armed = false;
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_OK && armed && enabled && size == 8192 && Armed);
        assert(!strcasecmp(sel, ActiveSavePath));
        for (unsigned k = 0; k < 4; k++) assert(!memcmp(Imported + k * 8192, file, 8192));
        GGCartFileSize = size; GameWrites(0xffu); CartReads = 0;
        assert(PersistActiveSave() == ESP_OK && FileSize(ActiveSavePath) == 8192 && CartReads == 9);
        f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == 8192); fclose(f2);
        assert(!memcmp(file, Live + 9 * 1024, 8192));
        /* 16 KiB (also this firmware's earlier saves): both banks. */
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(Live, 1, 16384, f2) == 16384); fclose(f2);
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_OK && armed && size == 16384 &&
               !memcmp(Imported, Live, 16384) && !memcmp(Imported + 16384, Live, 16384));
        /* 32 KiB: both banks as stored, written back whole. */
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(Live, 1, Cart, f2) == Cart); fclose(f2);
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_OK && armed && size == Cart && !memcmp(Imported, Live, Cart));
        GGCartFileSize = size; GameWrites(0xffffffffu);
        assert(PersistActiveSave() == ESP_OK && FileSize(ActiveSavePath) == Cart);
        f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, Live + 9 * 1024, Cart));
        /* 128 B EEPROM saves fill the start. */
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(Live, 1, 128, f2) == 128); fclose(f2);
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_OK && armed && size == 128 &&
               !memcmp(Imported, Live, 128) && Imported[128] == 0xff && Imported[Cart - 1] == 0xff);
        /* Any other size is refused (the game does not start), never overwritten. */
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(Live, 1, 1000, f2) == 1000); fclose(f2);
        armed = enabled = true;
        assert(PrepareGGCartSave(rom, sel, &armed, &enabled, &size) == ESP_ERR_INVALID_SIZE && !armed && !enabled);
        assert(FileSize(ActiveSavePath) == 1000);

        /* A state load keeps the in-game save as a numbered backup, and the
         * loaded state's cartridge RAM continues in the .SAV. */
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(Live, 1, Cart, f2) == Cart); fclose(f2);
        GGCartFileSize = Cart; ActiveSavePending = false; GameWrites(0xffffffffu);
        SetLive(0x60); static uint8_t ingame[Cart]; memcpy(ingame, Live + 9 * 1024, sizeof(ingame));
        Close();
        assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_OK); Idle();
        char backup[kPathSize]; snprintf(backup, sizeof(backup), "%s.before-load-0001.sav", ActiveSavePath);
        f2 = fopen(backup, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, ingame, Cart));
        assert(ActiveSavePending && !memcmp(Live, before, GGPayload));
        CartDirty = false;
        Open();
        assert(PersistActiveSave() == ESP_OK && !ActiveSavePending);
        Close();
        f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, before + 9 * 1024, Cart));
        /* Scratch memory: a block written before each of three copies stops
         * triggering saves, and a real save goes out although it keeps being
         * rewritten during the copy. No pause is ever needed. */
        Open();
        GGPendingBlocks = 0; memset(GGHistory, 0, sizeof(GGHistory)); ActiveSavePending = false; CartDirty = false;
        bool due = false;
        for (int k = 0; k < 3; k++)
        {
            GameWrites(1u << 8);
            assert(GGCartSaveDue(false, &due) == ESP_OK);
            if (due) assert(GGCartCopy() == ESP_OK);
        }
        GameWrites(1u << 8);
        assert(GGCartSaveDue(false, &due) == ESP_OK && !due && GGChronicBlocks() == (1u << 8));
        SetLive(0x70); GameWrites((1u << 8) | (1u << 3));
        assert(GGCartSaveDue(false, &due) == ESP_OK && due);
        DuringMask = 1u << 8;
        assert(GGCartCopy() == ESP_OK && !GGPendingBlocks);
        f2 = fopen(ActiveSavePath, "rb"); assert(f2 && fread(file, 1, sizeof(file), f2) == Cart); fclose(f2);
        assert(!memcmp(file, Live + 9 * 1024, Cart));
        /* A save written into a block that is not scratch memory during the
         * copy is retried, not published torn. */
        GameWrites(1u << 2);
        assert(GGCartSaveDue(false, &due) == ESP_OK && due);
        DuringMask = 1u << 2;
        assert(GGCartCopy() == ESP_ERR_NOT_FINISHED && GGPendingBlocks);
        assert(GGCartSaveDue(false, &due) == ESP_OK && due && GGCartCopy() == ESP_OK);
        /* A file smaller than the RAM (16 KiB from earlier builds) grows to
         * 32 KiB once the game writes the second bank. */
        f2 = fopen(ActiveSavePath, "wb"); assert(f2 && fwrite(Live, 1, 16384, f2) == 16384); fclose(f2);
        GGCartFileSize = 16384; GameWrites(1u << 20);
        assert(GGCartSaveDue(false, &due) == ESP_OK && due && GGCartCopy() == ESP_OK);
        assert(GGCartFileSize == Cart && FileSize(ActiveSavePath) == Cart);
        Close();
        GGCartSave = false; CartSupport = false;
    }

    DIR *d = opendir(dir); assert(d);
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') { char p[512]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name); unlink(p); }
    closedir(d); assert(!rmdir(dir));
    puts("PASS Game Gear save states: no-engine fallback, slot files, exact save/load, ROM mirror repair, damage/identity/engine refusal, FPGA fault lockout, battery saves (32 KiB: live copy, torn retry, 128 B/8/16/32 KiB files, start import, state-load backup, scratch blocks without pausing, growth)");
    return 0;
}
