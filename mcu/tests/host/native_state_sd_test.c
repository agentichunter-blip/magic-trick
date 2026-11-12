// SD persistence of native save states: the production virtual_cart.c flow with
// a modelled FPGA state slot (commands 10/11/14/15) and a temporary directory
// standing in for the SD card. No hardware is opened.
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include "../../main/cart_link.h"
#define NATIVE_TEST_SLOT_HOOKS
#define NATIVE_TEST_RAM_SIZE 32768
static bool NativeCommandHook(uint8_t cmd, uint16_t conf, uint8_t high, CartLinkResponse_t *r);
static bool NativeReadBlockHook(uint8_t *data);
static void NativeWritePsramHook(uint32_t address, const uint8_t *data);
#define main native_fixture_main
#include "native_state_test.c"
#undef main

static uint8_t Slot[256 * 1024];
static bool SlotSource, NoStateCommands;
static unsigned Imports, StateBlockReads;

static void SlotStatus(CartLinkResponse_t *r)
{
    const uint32_t status = 0x53530008 | (Valid ? 2 : 0);
    for (int i = 0; i < 4; i++) r->Data[i] = status >> (8 * i);
}
static bool NativeCommandHook(uint8_t cmd, uint16_t conf, uint8_t high, CartLinkResponse_t *r)
{
    switch (cmd)
    {
    case 4: SlotSource = false; return false;
    case 10: /* Engine capture: signature, core bytes, then cartridge RAM. */
        assert(Paused);
        memset(Slot, 0xa5, sizeof(Slot));
        memset(Slot, 0, 8); Slot[4] = 0xce; Slot[5] = 0xb0;
        for (unsigned i = 8; i < kStateFileCoreBytes; i++) Slot[i] = (uint8_t)(i * 7 + Ram[0]);
        memcpy(Slot + kStateFileCoreBytes, Ram, sizeof(Ram));
        Valid = true; SlotStatus(r); return true;
    case 11: /* Engine load refuses a slot without its signature. */
        assert(Paused && Valid);
        ++LoadCommands;
        assert(Slot[4] == 0xce && Slot[5] == 0xb0);
        memcpy(Ram, Slot + kStateFileCoreBytes, sizeof(Ram)); Dirty = true;
        SlotStatus(r); return true;
    case 14:
        if (NoStateCommands) { r->Status = 1; return true; }
        assert(Paused);
        Block = high; Sequence = conf; SlotSource = true; ++StateBlockReads;
        SlotStatus(r); return true;
    case 15:
        if (NoStateCommands) { r->Status = 1; return true; }
        assert(Paused); /* Also accepted over a valid slot (another save-state slot). */
        Valid = true; ++Imports; SlotStatus(r); return true;
    default:
        return false;
    }
}
static bool NativeReadBlockHook(uint8_t *data)
{
    if (!SlotSource) return false;
    memcpy(data, Slot + Block * 1024, 1024);
    return true;
}
static void NativeWritePsramHook(uint32_t address, const uint8_t *data)
{
    assert(Paused);
    if (address >= 0x440000 && address < 0x480000) memcpy(Slot + (address - 0x440000), data, 1024);
}

static long FileSize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}
static unsigned CountFiles(const char *dir)
{
    unsigned n = 0; DIR *d = opendir(dir); assert(d);
    for (struct dirent *e; (e = readdir(d));) n += e->d_name[0] != '.';
    closedir(d); return n;
}
static void PowerCycle(uint8_t current)
{
    /* PSRAM and the FPGA's valid flag do not survive power-off. */
    Valid = false; memset(Slot, 0, sizeof(Slot));
    memset(Ram, current, sizeof(Ram)); Dirty = true;
}
static void Idle(void) { assert(!Paused && !Mounted && !Bulk && !Running && !StateUncertain); }

int main(void)
{
    char path[kPathSize];
    assert(MakeStatePath("/sdcard/CHROMAGIC/BACKUPS/GBC/P/Pokemon - Crystal (USA).gbc", 1, path) == ESP_OK);
    assert(!strcmp(path, "/sdcard/CHROMAGIC/BACKUPS/GBC/P/Pokemon - Crystal (USA).state"));
    assert(MakeStatePath("/sdcard/CHROMAGIC/BACKUPS/GBC/P/Pokemon - Crystal (USA).gbc", 3, path) == ESP_OK);
    assert(!strcmp(path, "/sdcard/CHROMAGIC/BACKUPS/GBC/P/Pokemon - Crystal (USA).state3"));
    assert(MakeStatePath("/sdcard/a.gbc", 0, path) != ESP_OK && !path[0]);
    assert(MakeStatePath("/sdcard/a.gbc", 4, path) != ESP_OK && !path[0]);
    assert(MakeStatePath("/sdcard/no-extension", 1, path) != ESP_OK && !path[0]);

    char dir[] = "/tmp/native-state-sd-XXXXXX"; assert(mkdtemp(dir));
    Active = true;
    ActiveInfo.RamSize = sizeof(Ram); ActiveInfo.RomSize = 2u << 20; ActiveInfo.GlobalChecksum = 0x1234;
    ActiveInfo.Mapper.Kind = kCartMapper_Mbc3; ActiveInfo.Mapper.Type = 0x10;
    ActiveInfo.Mapper.HasBattery = ActiveInfo.Mapper.HasRam = ActiveInfo.Mapper.HasRtc = true;
    memcpy(ActiveInfo.Title, "POKEMON_CRYSTAL", 15);
    snprintf(ActiveSavePath, sizeof(ActiveSavePath), "%s/game.sav", dir);
    for (unsigned i = 0; i < 3; i++)
        snprintf(ActiveStatePath[i], sizeof(ActiveStatePath[i]), "%s/game.state%s", dir, i ? (i == 1 ? "2" : "3") : "");
    char *const StatePath1 = ActiveStatePath[0];
    char canonical[kPathSize]; strcpy(canonical, ActiveSavePath);
    const uint32_t length = StateFile_PayloadLength(sizeof(Ram));

    /* MBC3 with a clock is supported; MBC30 and oversized RAM are not. */
    bool supported = false, valid = true, uncertain = true; uint32_t session = 0;
    assert(VirtualCart_NativeInfo(&supported, &valid, &uncertain, &session) == ESP_OK);
    assert(supported && !valid && !uncertain);
    ActiveInfo.Mapper.IsMbc30 = true;
    assert(VirtualCart_NativeInfo(&supported, &valid, &uncertain, &session) == ESP_OK && !supported);
    assert(VirtualCart_StateCapture(false) == ESP_ERR_INVALID_STATE);
    ActiveInfo.Mapper.IsMbc30 = false;

    /* Nothing saved yet: no load, nothing imported. */
    assert(VirtualCart_StateCapture(true) == ESP_ERR_INVALID_STATE && Imports == 0); Idle();

    /* 1. Save writes "<rom>.state": header + exact slot payload. */
    memset(Ram, 0x11, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCapture(false) == ESP_OK); Idle();
    assert(!VirtualCart_LastStateSdFailed() && StateOnSdMask == 1 && FpgaStateOwner == 1);
    assert(StateBlockReads == (length + 1023) / 1024);
    assert(FileSize(StatePath1) == kStateFileHeaderSize + (long)length);
    {
        FILE *f = fopen(StatePath1, "rb"); assert(f);
        uint8_t header[kStateFileHeaderSize]; assert(fread(header, 1, sizeof(header), f) == sizeof(header));
        static uint8_t payload[256 * 1024]; assert(fread(payload, 1, length, f) == length); fclose(f);
        StateFileGame_t game; ActiveStateGame(&game);
        uint32_t len = 0, crc = 0;
        assert(StateFile_DecodeHeader(header, &game, &len, &crc) == kStateFile_Ok && len == length);
        assert(crc == StateFile_Crc32(0, payload, length) && !memcmp(payload, Slot, length));
    }
    char tmp[kPathSize]; snprintf(tmp, sizeof(tmp), "%s.tmp", StatePath1);
    assert(FileSize(tmp) < 0);

    /* 2. Power cycle: the slot is gone, the SD copy makes Load available. */
    PowerCycle(0x77);
    assert(VirtualCart_NativeInfo(&supported, &valid, &uncertain, &session) == ESP_OK && supported && valid);
    unsigned loads = LoadCommands;
    assert(VirtualCart_StateCapture(true) == ESP_OK); Idle();
    assert(Imports == 1 && LoadCommands == loads + 1);
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x11);
    verify(canonical, 0x77);                       /* newer battery save kept */
    assert(strcmp(ActiveSavePath, canonical));      /* restored timeline on a branch */
    char branch1[kPathSize]; strcpy(branch1, ActiveSavePath);

    /* 3. A damaged payload is refused before anything changes; the game resumes. */
    PowerCycle(0x22);
    {
        FILE *f = fopen(StatePath1, "r+b"); assert(f);
        assert(!fseek(f, kStateFileHeaderSize + 40000, SEEK_SET));
        int c = fgetc(f); assert(c != EOF); assert(!fseek(f, -1, SEEK_CUR)); fputc(c ^ 0x01, f); fclose(f);
    }
    unsigned files = CountFiles(dir); loads = LoadCommands;
    assert(VirtualCart_StateCapture(true) == ESP_ERR_INVALID_CRC); Idle();
    assert(LoadCommands == loads && Imports == 1 && !Valid && CountFiles(dir) == files);
    assert(!strcmp(ActiveSavePath, branch1));
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x22);

    /* 4. Save again (replaces the damaged file), then the wrong game is refused. */
    memset(Ram, 0x33, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCapture(false) == ESP_OK && !VirtualCart_LastStateSdFailed()); Idle();
    PowerCycle(0x44);
    ActiveInfo.GlobalChecksum ^= 1;
    assert(VirtualCart_StateCapture(true) == ESP_ERR_INVALID_VERSION); Idle();
    ActiveInfo.GlobalChecksum ^= 1;

    /* 5. Interrupted publication (only ".old" left) is recovered and loads. */
    char old[kPathSize]; snprintf(old, sizeof(old), "%s.old", StatePath1);
    assert(!rename(StatePath1, old));
    loads = LoadCommands;
    assert(VirtualCart_StateCapture(true) == ESP_OK); Idle();
    assert(LoadCommands == loads + 1 && FileSize(old) < 0 && FileSize(StatePath1) > 0);
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x33);

    /* 6. Truncated file. */
    PowerCycle(0x55);
    assert(!truncate(StatePath1, kStateFileHeaderSize + length - 1));
    assert(VirtualCart_StateCapture(true) == ESP_ERR_INVALID_SIZE); Idle();

    /* 7. An FPGA without commands 14/15: the native save still succeeds, the SD
     * copy is reported missing and the previous file is left untouched. */
    long before = FileSize(StatePath1);
    NoStateCommands = true;
    memset(Ram, 0x66, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCapture(false) == ESP_OK); Idle();
    assert(VirtualCart_LastStateSdFailed() && Valid);
    assert(FileSize(StatePath1) == before && FileSize(tmp) < 0);
    /* With the slot still valid, Load uses it without touching the SD copy. */
    memset(Ram, 0x67, sizeof(Ram)); Dirty = true; loads = LoadCommands;
    assert(VirtualCart_StateCapture(true) == ESP_OK); Idle();
    assert(LoadCommands == loads + 1 && Imports == 2);
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x66);
    NoStateCommands = false;

    /* 8. Three slots. Save 0x81 to slot 2 and 0x82 to slot 3, then power off. */
    uint8_t mask = 0;
    memset(Ram, 0x81, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCaptureSlot(false, 2, 0) == ESP_OK); Idle();
    memset(Ram, 0x82, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCaptureSlot(false, 3, 0) == ESP_OK); Idle();
    assert(FileSize(ActiveStatePath[1]) == kStateFileHeaderSize + (long)length);
    assert(FileSize(ActiveStatePath[2]) == kStateFileHeaderSize + (long)length);
    assert(StateOnSdMask == 7 && FpgaStateOwner == 3);
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && mask == 7);
    assert(VirtualCart_StateCaptureSlot(true, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(VirtualCart_StateCaptureSlot(true, 4, 0) == ESP_ERR_INVALID_ARG);
    /* The FPGA slot holds slot 3: loading it needs no SD read. */
    unsigned imports = Imports;
    memset(Ram, 0x90, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCaptureSlot(true, 3, 0) == ESP_OK); Idle();
    assert(Imports == imports);
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x82);
    /* Slot 2 is read from the SD card although the FPGA slot is valid. */
    assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_OK); Idle();
    assert(Imports == imports + 1 && FpgaStateOwner == 2);
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x81);
    /* After power-off every slot comes back from its own file. */
    PowerCycle(0x91);
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && mask == 7);
    assert(VirtualCart_StateCaptureSlot(true, 3, 0) == ESP_OK); Idle();
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x82);
    /* A damaged slot 2 fails after overwriting the FPGA slot (still flagged valid).
     * Slot 3 must then come from its file again, not from the damaged FPGA slot. */
    {
        FILE *f = fopen(ActiveStatePath[1], "r+b"); assert(f);
        assert(!fseek(f, kStateFileHeaderSize + 60000, SEEK_SET));
        int c = fgetc(f); assert(c != EOF); assert(!fseek(f, -1, SEEK_CUR)); fputc(c ^ 0x10, f); fclose(f);
    }
    imports = Imports;
    assert(VirtualCart_StateCaptureSlot(true, 2, 0) == ESP_ERR_INVALID_CRC); Idle();
    assert(Valid && FpgaStateOwner == 0);
    assert(VirtualCart_NativeSlotInfo(&supported, &mask, &uncertain, &session) == ESP_OK && mask == 7);
    memset(Ram, 0x92, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCaptureSlot(true, 3, 0) == ESP_OK); Idle();
    assert(Imports == imports + 1);
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x82);
    /* Slot 1's file is still the one truncated in step 6: refused, game unchanged. */
    assert(VirtualCart_StateCaptureSlot(true, 1, 0) == ESP_ERR_INVALID_SIZE); Idle();
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x82);
    /* Saving slot 1 again repairs it without touching slots 2 and 3. */
    memset(Ram, 0x83, sizeof(Ram)); Dirty = true;
    assert(VirtualCart_StateCaptureSlot(false, 1, 0) == ESP_OK); Idle();
    PowerCycle(0x93);
    assert(VirtualCart_StateCaptureSlot(true, 1, 0) == ESP_OK); Idle();
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x83);
    assert(VirtualCart_StateCaptureSlot(true, 3, 0) == ESP_OK); Idle();
    for (unsigned i = 0; i < sizeof(Ram); i++) assert(Ram[i] == 0x82);

    /* Clean up everything the test created. */
    DIR *d = opendir(dir); assert(d);
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') { char p[kPathSize * 2]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name); unlink(p); }
    closedir(d); assert(!rmdir(dir));
    puts("PASS SD save states: export, power-cycle import, MBC3+RTC, three slots, damage/identity/truncation refusal, publish recovery, old FPGA");
    return 0;
}
