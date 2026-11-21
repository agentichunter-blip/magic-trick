#include "cart_store.h"

#include "boot_resume.h"
#include "cart_backup.h"
#include "cart_bulk.h"
#include "core_switch.h"
#include "sd_card.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "diag_log.h"
#include "esp_console.h"
#include "esp_rom_crc.h"
#include "esp_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "cart_store";

enum {
    kBlock = kCartBulkBlockSize,
    kSlotBase = 64,
    kSlotSize = 96,
    kCacheBlocks = 4,
    kMaxFiles = 2,
    kMaxAddresses = 16,
    kMaxUnique = 6,
    kRecordMagic = 0x3147544d, /* "MTG1" */
};

static const char kDirMagic[16] = "MAGICTRICKCART01";
static const char kDir2Magic[16] = "MAGICTRICKCART02";
static const char kEntry2Magic[4] = {'M', 'T', 'E', '2'};
enum { kDir2Header = 64, kEntry2Size = 128, kEntry2Name = 32, kMaxEntries2 = 4096 };
static const char kEntryMagic[4] = {'M', 'T', 'E', '1'};
static const char kNvsNamespace[] = "cartstore";
static const char kNvsKey[] = "stage";
static const char kNvsDirKey[] = "dir2";    /* packed entries (PackDirectory) */
static const char kNvsOldDirKey[] = "dir";  /* version 1 cache, removed on save */
static const char kNvsOwnKey[] = "own"; /* file name of the cart's own game */
/* A save of the cart's own game not yet written back to its chip: the file,
 * its size, and the CRC-32 of the cartridge header it belongs to. */
static const char kNvsOwnSaveKey[] = "osave", kNvsOwnSaveSizeKey[] = "osize", kNvsOwnSaveHeadKey[] = "ohead";
/* Which storage held it (1 = the internal store, 0 = the microSD card): the
 * same path names a different file on the other one. */
static const char kNvsOwnSaveKindKey[] = "okind";
/* 1 from staging until the target core has replayed it (the record itself
 * stays, so that core can still open the game for later reads). */
static const char kNvsFreshKey[] = "fresh";
/* Full copy of the staged game, for reads the target core needs past the
 * first 1 KiB (e.g. restoring the Game Gear ROM mirror after a save or
 * state import). Written by the GB core while staging. */
static const char kStagedCopy[] = CART_STORE_SIDECAR_ROOT "/.staged.bin";
/* With no microSD card: only the Game Gear ROM-mirror bytes (StageMirror). */
static const char kStagedMirror[] = CART_STORE_SIDECAR_ROOT "/.mirror.bin";
enum { kMirrorMagic = 0x314d544d /* "MTM1" */ };
typedef struct { uint32_t Magic, EntryCrc, Size, Start, Length; } MirrorHeader_t;

static SemaphoreHandle_t Lock;
static StaticSemaphore_t LockStorage;

static CartStoreEntry_t *Entries;
static size_t EntryCount, EntryCapacity;
static bool Scanned;
static char OwnName[kCartStoreNameSize]; /* "" for none (CartStore_OwnGame) */
/* Box art of the cart's own game: version 2 header bytes 32-47 (offset, size,
 * CRC, then a CRC-32 of those 12 bytes); size 0 for none. */
static uint32_t OwnArtOffset, OwnArtSize, OwnArtCrc;
static uint32_t OwnSize; /* bytes the own game spans from offset 0 (its header's ROM size) */

static uint8_t Cache[kCacheBlocks * kBlock];
static uint32_t CacheOffset;
static bool CacheValid;

typedef struct {
    bool Used;
    bool HeadOnly; /* core without cartridge access: NVS head + SD copy */
    FILE *pCopy;   /* the SD copy, opened on the first read past the head */
    CartStoreEntry_t Entry;
    uint32_t Position;
} OpenFile_t;
static OpenFile_t Files[kMaxFiles];

/* A cartridge game staged into PSRAM by the GB core for another core: the
 * blocks the next cores' PSRAM self-tests overwrite (identical blocks kept
 * once) and the file's first 1 KiB (headers), kept in NVS across the reset. */
typedef struct {
    uint32_t Magic;
    char Name[kCartStoreNameSize];
    uint32_t Size;
    uint32_t EntryCrc;
    uint32_t RomCrc;
    uint8_t Fresh;
    uint8_t Overflow;
    uint8_t AddressCount;
    uint8_t UniqueCount;
    uint8_t Rotation; /* Lynx: info block rotation bits (0 none) */
    uint8_t Reserved[3];
    struct { uint32_t Address; uint8_t Unique; uint8_t Pad[3]; } Addresses[kMaxAddresses];
    uint8_t Head[kCartStoreHeadSize];
    uint8_t Unique[kMaxUnique][kBlock];
} StageRecord_t;

static StageRecord_t *Record;      /* staging or replay in progress */
static CartStoreEntry_t StagedEntry; /* the entry being staged (GB core) */
static bool LoadRecord(void);
static bool RecordLoaded;

/* ---------------------------------------------------------------- format */

static uint32_t EntryCrc(const uint8_t *pSlot)
{
    uint8_t Copy[kSlotSize];
    memcpy(Copy, pSlot, sizeof(Copy));
    memset(&Copy[20], 0, 4);
    return esp_rom_crc32_le(0, Copy, sizeof(Copy));
}

static uint32_t Read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Write32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool SlotEmpty(const uint8_t *pSlot)
{
    for (size_t i = 0; i < kSlotSize; ++i)
        if (pSlot[i] != 0xff) return false;
    return true;
}

static bool ParseSlot(const uint8_t *pSlot, CartStoreEntry_t *pEntry)
{
    if (memcmp(pSlot, kEntryMagic, 4) != 0 || Read32(&pSlot[20]) != EntryCrc(pSlot)) return false;
    const char *pName = (const char *)&pSlot[24];
    const size_t Length = strnlen(pName, kCartStoreNameSize);
    if (Length == 0 || Length >= kCartStoreNameSize || strchr(pName, '/') || pName[0] == '.') return false;
    const uint32_t Offset = Read32(&pSlot[8]), Size = Read32(&pSlot[12]);
    if ((Offset & (kBlock - 1u)) || Size == 0 || Offset + Size > (8u << 20) || Offset + Size < Offset)
        return false;
    memcpy(pEntry->Name, pName, Length + 1);
    pEntry->ArtOffset = pEntry->ArtSize = pEntry->ArtCrc = 0;
    pEntry->System = pSlot[4];
    pEntry->Offset = Offset;
    pEntry->Size = Size;
    pEntry->Crc = Read32(&pSlot[16]);
    return true;
}

static bool ReserveEntries(size_t Count)
{
    if (Count <= EntryCapacity) return true;
    CartStoreEntry_t *p = realloc(Entries, Count * sizeof(*p));
    if (!p) return false;
    Entries = p;
    EntryCapacity = Count;
    return true;
}

/* Version 2: the directory at Offset whose first block is pBlock (read the
 * rest from the cartridge). Fills Entries; -1 if it is not a version 2
 * directory or doesn't check out. Cartridge session open. */
static int ParseDirectory2(uint32_t Offset, const uint8_t *pBlock)
{
    if (memcmp(pBlock, kDir2Magic, sizeof(kDir2Magic)) != 0) return -1;
    if (Read32(&pBlock[28]) != esp_rom_crc32_le(0, pBlock, 28)) return -1;
    const uint32_t Count = Read32(&pBlock[16]), Bytes = Read32(&pBlock[24]);
    if (Read32(&pBlock[44]) == esp_rom_crc32_le(0, &pBlock[32], 12) && Read32(&pBlock[36]) &&
        Read32(&pBlock[36]) <= 64u * 1024u && Read32(&pBlock[32]) < (8u << 20))
    {
        OwnArtOffset = Read32(&pBlock[32]); OwnArtSize = Read32(&pBlock[36]); OwnArtCrc = Read32(&pBlock[40]);
    }
    if (Count > kMaxEntries2 || Bytes < kDir2Header + Count * kEntry2Size || Bytes > (1u << 20)) return -1;
    const uint32_t Blocks = (kDir2Header + Count * kEntry2Size + kBlock - 1u) / kBlock;
    uint8_t *pDir = malloc(Blocks * kBlock);
    if (!pDir || !ReserveEntries(Count ? Count : 1)) { free(pDir); return -1; }
    memcpy(pDir, pBlock, kBlock);
    esp_err_t Result = Blocks > 1 ? CartBulk_ReadCart(Offset + kBlock, pDir + kBlock, Blocks - 1) : ESP_OK;
    int Parsed = -1;
    if (Result == ESP_OK && esp_rom_crc32_le(0, pDir + kDir2Header, Count * kEntry2Size) == Read32(&pBlock[20]))
    {
        Parsed = 0;
        for (uint32_t i = 0; i < Count; ++i)
        {
            const uint8_t *p = pDir + kDir2Header + i * kEntry2Size;
            const char *pName = (const char *)&p[kEntry2Name];
            const size_t Length = strnlen(pName, kCartStoreNameSize);
            const uint32_t GameOffset = Read32(&p[8]), Size = Read32(&p[12]);
            if (memcmp(p, kEntry2Magic, 4) || Length == 0 || Length >= kCartStoreNameSize || strchr(pName, '/') ||
                pName[0] == '.' || (GameOffset & (kBlock - 1u)) || Size == 0 || GameOffset + Size > (8u << 20) ||
                GameOffset + Size < GameOffset) continue;
            CartStoreEntry_t *e = &Entries[Parsed++];
            memcpy(e->Name, pName, Length + 1);
            e->System = p[4];
            e->Offset = GameOffset;
            e->Size = Size;
            e->Crc = Read32(&p[16]);
            e->ArtOffset = Read32(&p[24]) ? Read32(&p[20]) : 0;
            e->ArtSize = e->ArtOffset ? Read32(&p[24]) : 0;
            e->ArtCrc = Read32(&p[28]);
        }
    }
    free(pDir);
    return Parsed;
}

/* Fills Out (may be NULL) and returns the valid entry count, or -1 if the
 * block is not a directory. *pFreeSlot: first empty slot or -1; *pEnd: end
 * of the last game (block aligned). */
static int ParseDirectory(const uint8_t *pBlock, CartStoreEntry_t *pOut, int *pFreeSlot, uint32_t *pEnd)
{
    if (memcmp(pBlock, kDirMagic, sizeof(kDirMagic)) != 0) return -1;
    int Count = 0, FreeSlot = -1;
    uint32_t End = 0;
    for (int Slot = 0; Slot < kCartStoreSlots; ++Slot)
    {
        const uint8_t *pSlot = &pBlock[kSlotBase + Slot * kSlotSize];
        CartStoreEntry_t Entry;
        if (SlotEmpty(pSlot)) { if (FreeSlot < 0) FreeSlot = Slot; continue; }
        if (!ParseSlot(pSlot, &Entry)) continue; /* damaged slot: skipped, never reused */
        const uint32_t EntryEnd = (Entry.Offset + Entry.Size + kBlock - 1u) & ~(kBlock - 1u);
        if (EntryEnd > End) End = EntryEnd;
        if (pOut) pOut[Count] = Entry;
        ++Count;
    }
    if (pFreeSlot) *pFreeSlot = FreeSlot;
    if (pEnd) *pEnd = End;
    return Count;
}

/* "<TITLE> (CARTRIDGE).gb[c]" for a Game Boy header with a valid checksum,
 * else "". */
static void OwnNameFromHeader(const uint8_t Block0[kBlock], char *pOut, size_t Size)
{
    pOut[0] = 0;
    uint8_t Sum = 0;
    for (unsigned I = 0x134; I <= 0x14c; ++I) Sum = (uint8_t)(Sum - Block0[I] - 1);
    if (Sum != Block0[0x14d]) return;
    const bool Color = (Block0[0x143] & 0x80) != 0;
    char Title[17];
    size_t Used = 0;
    for (unsigned I = 0x134; I < (Color ? 0x143u : 0x144u) && Block0[I]; ++I)
    {
        const char C = (char)Block0[I];
        const bool Keep = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') ||
            C == '-' || C == '!' || C == '&' || C == '.' || C == '\'';
        Title[Used++] = Keep ? C : ' ';
    }
    while (Used && Title[Used - 1] == ' ') --Used;
    Title[Used] = 0;
    snprintf(pOut, Size, "%s (CARTRIDGE).%s", Used ? Title : "GAME BOY", Color ? "gbc" : "gb");
}

/* Directory candidates: 0x4000 (dedicated cart), then the end of the GB game. */
static size_t DirectoryCandidates(const uint8_t Block0[kBlock], uint32_t Out[2])
{
    size_t Count = 0;
    Out[Count++] = 0x4000;
    const uint8_t Code = Block0[0x148];
    if (Code <= 8)
    {
        const uint32_t GameSize = 32u * 1024u << Code;
        if (GameSize >= 0x8000u && GameSize < (8u << 20)) Out[Count++] = GameSize;
    }
    return Count;
}

/* ------------------------------------------------------------ cart access */

/* Runs Fn inside a CartBulk session (reusing the caller's) with the
 * cartridge bus taken. */
static bool CartHeld; /* CartStore_HoldCart: already in a cartridge session */

static esp_err_t WithCart(esp_err_t (*Fn)(void *), void *pContext)
{
    const bool Own = !CartBulk_IsSessionActive();
    esp_err_t Result = Own ? CartBulk_Begin() : ESP_OK;
    if (Result != ESP_OK) return Result;
    if (CartHeld)
    {
        Result = Fn(pContext);
        if (Own) CartBulk_End();
        return Result;
    }
    Result = CartBulk_CartEnter(NULL);
    if (Result == ESP_OK)
    {
        Result = Fn(pContext);
        const esp_err_t ExitResult = CartBulk_CartExit();
        if (Result == ESP_OK) Result = ExitResult;
    }
    if (Own) CartBulk_End();
    return Result;
}

esp_err_t CartStore_HoldCart(void)
{
    if (CartHeld) return ESP_OK;
    if (BootResume_TakeHeldCartridge()) { CartHeld = true; return ESP_OK; }
    const bool Own = !CartBulk_IsSessionActive();
    esp_err_t Result = Own ? CartBulk_Begin() : ESP_OK;
    if (Result == ESP_OK)
    {
        Result = CartBulk_CartEnter(NULL);
        if (Own) CartBulk_End();
    }
    CartHeld = Result == ESP_OK;
    return Result;
}

void CartStore_ReleaseCart(bool Exit)
{
    if (!CartHeld) return;
    CartHeld = false;
    if (!Exit) return;
    const bool Own = !CartBulk_IsSessionActive();
    if (Own && CartBulk_Begin() != ESP_OK) return;
    const esp_err_t Result = CartBulk_CartExit();
    if (Own) CartBulk_End();
    if (Result != ESP_OK) printf("CARTSTORE release=%s\n", esp_err_to_name(Result));
}

typedef struct {
    uint32_t Offset;
    uint8_t *pData;
    uint32_t Blocks;
} ReadRequest_t;

static esp_err_t ReadFn(void *pContext)
{
    ReadRequest_t *p = pContext;
    return CartBulk_ReadCart(p->Offset, p->pData, p->Blocks);
}

/* A cart transaction now and then times out while other start-up work
 * shares the FPGA link (seen at power-on auto-boot). Each attempt opens a
 * fresh cart session; callers check the data's CRC either way. */
static esp_err_t ReadCart(uint32_t Offset, uint8_t *pData, uint32_t Blocks)
{
    ReadRequest_t Request = {Offset, pData, Blocks};
    esp_err_t Result = ESP_FAIL;
    for (unsigned Attempt = 0; Attempt < 4; ++Attempt)
    {
        if (Attempt) vTaskDelay(pdMS_TO_TICKS(20 * Attempt));
        Result = WithCart(ReadFn, &Request);
        if (Result == ESP_OK || Result == ESP_ERR_INVALID_ARG) break;
        printf("CARTSTORE read_retry=%u error=%s offset=0x%lx\n", Attempt + 1, esp_err_to_name(Result), (unsigned long)Offset);
    }
    return Result;
}

static esp_err_t ScanFn(void *pContext)
{
    (void)pContext;
    uint8_t *pBlock = malloc(kBlock);
    if (!pBlock) return ESP_ERR_NO_MEM;
    EntryCount = 0;
    OwnName[0] = 0;
    OwnSize = 0;
    OwnArtOffset = OwnArtSize = OwnArtCrc = 0;
    esp_err_t Result = CartBulk_ReadCart(0, pBlock, 1);
    uint32_t Candidates[2];
    const size_t CandidateCount = Result == ESP_OK ? DirectoryCandidates(pBlock, Candidates) : 0;
    char Own[kCartStoreNameSize] = "";
    if (Result == ESP_OK) OwnNameFromHeader(pBlock, Own, sizeof(Own));
    const uint32_t Spans = Result == ESP_OK && pBlock[0x148] <= 8 ? 0x8000u << pBlock[0x148] : 0;
    /* Reading a cart into memory and writing its save back assume MBC5-style
     * banking (ModRetro and flash carts). A plain cart (no Magic Trick games)
     * is listed only when its header says so, or that it doesn't bank. */
    const uint8_t Mapper = Result == ESP_OK ? pBlock[0x147] : 0xff;
    const bool PlainOk = Own[0] && strncmp(Own, "MAGIC TRICK", 11) &&
                         (Mapper == 0x00 || (Mapper >= 0x19 && Mapper <= 0x1e));
    bool DirFound = false;
    for (size_t i = 0; Result == ESP_OK && i < CandidateCount; ++i)
    {
        Result = CartBulk_ReadCart(Candidates[i], pBlock, 1);
        int Count = -1;
        if (Result == ESP_OK && ReserveEntries(kCartStoreSlots)) Count = ParseDirectory(pBlock, Entries, NULL, NULL);
        if (Result == ESP_OK && Count < 0) Count = ParseDirectory2(Candidates[i], pBlock);
        if (Count >= 0)
        {
            EntryCount = (size_t)Count;
            /* Directory after the GB game (not a dedicated cart at 0x4000):
             * that game is still there to play. */
            DirFound = true;
            if (i > 0) { memcpy(OwnName, Own, sizeof(OwnName)); OwnSize = Spans; }
            printf("CARTSTORE dir=0x%lx entries=%d own=\"%s\"\n", (unsigned long)Candidates[i], Count, OwnName);
            break;
        }
        OwnArtOffset = OwnArtSize = OwnArtCrc = 0; /* only from the directory that counts */
    }
    /* A plain cart (no Magic Trick directory): its own game still goes in the
     * Cartridge list, so it can be picked (save states) with or without a card. */
    if (Result == ESP_OK && !DirFound && PlainOk)
    {
        memcpy(OwnName, Own, sizeof(OwnName));
        OwnSize = Spans;
        printf("CARTSTORE plain own=\"%s\"\n", OwnName);
    }
    if (!OwnName[0]) OwnArtOffset = OwnArtSize = OwnArtCrc = 0;
    free(pBlock);
    return Result;
}

/* The directory as last read by the GB core, for cores that cannot read
 * the cartridge (their Cartridge folder lists it; choosing a game goes back
 * through the GB core to stage it). Packed: per entry offset, size, CRC,
 * art offset, art size (5 x u32), system, name length, name. */
static uint8_t *PackDirectory(size_t *pLength)
{
    size_t Length = 0;
    for (size_t i = 0; i < EntryCount; ++i) Length += 22 + strlen(Entries[i].Name);
    uint8_t *pOut = malloc(Length ? Length : 1), *p = pOut;
    if (!pOut) return NULL;
    for (size_t i = 0; i < EntryCount; ++i)
    {
        const CartStoreEntry_t *e = &Entries[i];
        const size_t NameLength = strlen(e->Name);
        Write32(p, e->Offset); Write32(p + 4, e->Size); Write32(p + 8, e->Crc);
        Write32(p + 12, e->ArtOffset); Write32(p + 16, e->ArtSize);
        p[20] = e->System; p[21] = (uint8_t)NameLength;
        memcpy(p + 22, e->Name, NameLength);
        p += 22 + NameLength;
    }
    *pLength = Length;
    return pOut;
}

static void UnpackDirectory(const uint8_t *p, size_t Length)
{
    EntryCount = 0;
    size_t Count = 0;
    for (size_t At = 0; At + 22 <= Length && p[At + 21] < kCartStoreNameSize; At += 22 + p[At + 21]) ++Count;
    if (!Count || !ReserveEntries(Count)) return;
    for (size_t At = 0; At + 22 <= Length && EntryCount < Count; At += 22 + p[At + 21])
    {
        const size_t NameLength = p[At + 21];
        if (At + 22 + NameLength > Length) break;
        CartStoreEntry_t *e = &Entries[EntryCount++];
        e->Offset = Read32(p + At); e->Size = Read32(p + At + 4); e->Crc = Read32(p + At + 8);
        e->ArtOffset = Read32(p + At + 12); e->ArtSize = Read32(p + At + 16); e->ArtCrc = 0;
        e->System = p[At + 20];
        memcpy(e->Name, p + At + 22, NameLength);
        e->Name[NameLength] = 0;
    }
}

static void SaveDirectory(void)
{
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &Handle) != ESP_OK) return;
    bool Changed = nvs_erase_key(Handle, kNvsOldDirKey) == ESP_OK;
    size_t Length = 0, OldLength = 0;
    uint8_t *pPacked = PackDirectory(&Length);
    if (pPacked)
    {
        uint8_t *pOld = NULL;
        const bool Same = nvs_get_blob(Handle, kNvsDirKey, NULL, &OldLength) == ESP_OK && OldLength == Length &&
            (pOld = malloc(Length ? Length : 1)) && nvs_get_blob(Handle, kNvsDirKey, pOld, &OldLength) == ESP_OK &&
            !memcmp(pOld, pPacked, Length);
        free(pOld);
        if (!Same)
        {
            const esp_err_t Result = Length ? nvs_set_blob(Handle, kNvsDirKey, pPacked, Length)
                                            : nvs_erase_key(Handle, kNvsDirKey);
            if (Result != ESP_OK && Result != ESP_ERR_NVS_NOT_FOUND)
                printf("CARTSTORE dir_cache=%s bytes=%u\n", esp_err_to_name(Result), (unsigned)Length);
            Changed = true;
        }
        free(pPacked);
    }
    char OldOwn[kCartStoreNameSize] = "";
    Length = sizeof(OldOwn);
    if (nvs_get_str(Handle, kNvsOwnKey, OldOwn, &Length) != ESP_OK) OldOwn[0] = 0;
    if (strcmp(OldOwn, OwnName))
    {
        if (OwnName[0]) (void)nvs_set_str(Handle, kNvsOwnKey, OwnName);
        else (void)nvs_erase_key(Handle, kNvsOwnKey);
        Changed = true;
    }
    if (Changed) (void)nvs_commit(Handle);
    nvs_close(Handle);
}

static void LoadDirectory(void)
{
    EntryCount = 0;
    OwnName[0] = 0;
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &Handle) != ESP_OK) return;
    size_t Length = 0;
    if (nvs_get_blob(Handle, kNvsDirKey, NULL, &Length) == ESP_OK && Length)
    {
        uint8_t *p = malloc(Length);
        if (p && nvs_get_blob(Handle, kNvsDirKey, p, &Length) == ESP_OK) UnpackDirectory(p, Length);
        free(p);
    }
    Length = sizeof(OwnName);
    if (nvs_get_str(Handle, kNvsOwnKey, OwnName, &Length) != ESP_OK) OwnName[0] = 0;
    nvs_close(Handle);
}

size_t CartStore_Refresh(bool Force)
{
    xSemaphoreTake(Lock, portMAX_DELAY);
    if (Force || !Scanned)
    {
        CacheValid = false;
        EntryCount = 0;
        if (!CartStore_CanReadCart())
        {
            LoadDirectory();
            Scanned = true;
        }
        else
        {
            const esp_err_t Result = WithCart(ScanFn, NULL);
            Scanned = Result == ESP_OK || Result == ESP_ERR_NOT_FOUND;
            if (Scanned) SaveDirectory();
            else printf("CARTSTORE scan_error=%s\n", esp_err_to_name(Result));
        }
    }
    const size_t Count = EntryCount;
    xSemaphoreGive(Lock);
    return Count;
}

size_t CartStore_Count(void) { return EntryCount; }

bool CartStore_OwnGame(char *pPath, size_t Size)
{
    if (!pPath || !Size) return false;
    xSemaphoreTake(Lock, portMAX_DELAY);
    const int Length = OwnName[0] ? snprintf(pPath, Size, "%s/%s", CART_STORE_OWN_ROOT, OwnName) : 0;
    xSemaphoreGive(Lock);
    if (Length <= 0 || (size_t)Length >= Size) { pPath[0] = 0; return false; }
    return true;
}

esp_err_t CartStore_RestartCart(void)
{
    if (!CartStore_CanReadCart()) return ESP_ERR_INVALID_STATE;
    /* Enter holds the GB core in reset; Exit lets the cartridge boot. */
    const esp_err_t Result = CartStore_HoldCart();
    if (Result == ESP_OK) CartStore_ReleaseCart(true);
    printf("CARTSTORE restart=%s\n", esp_err_to_name(Result));
    return Result;
}

/* No lock: storage mounts call this, including from inside a locked
 * cartridge session (CartStore_FlashEnd); it only reads two flags and NVS. */
bool CartStore_HasGames(void)
{
    /* Anything for the Cartridge list: Magic Trick games or the cart's own game. */
    bool Has = Scanned && (EntryCount > 0 || OwnName[0]);
    if (!Scanned)
    {
        nvs_handle_t Handle;
        size_t Length = 0;
        if (nvs_open(kNvsNamespace, NVS_READONLY, &Handle) == ESP_OK)
        {
            Has = nvs_get_blob(Handle, kNvsDirKey, NULL, &Length) == ESP_OK && Length >= 22; /* one packed entry */
            Length = 0;
            if (!Has) Has = nvs_get_str(Handle, kNvsOwnKey, NULL, &Length) == ESP_OK && Length > 1;
            nvs_close(Handle);
        }
    }
    return Has;
}

esp_err_t CartStore_AutoBootPath(char *pPath, size_t Size)
{
    if (!pPath || !Size) return ESP_ERR_INVALID_ARG;
    pPath[0] = 0;
    if (!CartStore_CanReadCart()) return ESP_ERR_INVALID_STATE;
    size_t Count = CartStore_Refresh(true);
    for (unsigned Attempt = 0; Count == 0 && !Scanned && Attempt < 3; ++Attempt)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        Count = CartStore_Refresh(true); /* a failed scan, not an empty directory */
    }
    if (Count != 1) return Count ? ESP_ERR_INVALID_SIZE : ESP_ERR_NOT_FOUND;
    CartStoreEntry_t Entry;
    if (!CartStore_Entry(0, &Entry)) return ESP_ERR_NOT_FOUND;
    const int Length = snprintf(pPath, Size, "%s/%s", CART_STORE_ROOT, Entry.Name);
    if (Length <= 0 || (size_t)Length >= Size) { pPath[0] = 0; return ESP_ERR_INVALID_SIZE; }
    return ESP_OK;
}

bool CartStore_Entry(size_t Index, CartStoreEntry_t *pEntry)
{
    xSemaphoreTake(Lock, portMAX_DELAY);
    const bool Ok = Index < EntryCount;
    if (Ok && pEntry) *pEntry = Entries[Index];
    xSemaphoreGive(Lock);
    return Ok;
}

bool CartStore_IsCartPath(const char *pPath)
{
    return pPath && strncmp(pPath, CART_STORE_ROOT "/", sizeof(CART_STORE_ROOT)) == 0;
}

esp_err_t CartStore_SidecarBase(const char *pPath, char *pOut, size_t Size)
{
    if (!CartStore_IsCartPath(pPath)) return ESP_ERR_INVALID_ARG;
    const char *pName = CartStore_IsOwnGamePath(pPath) ? pPath + sizeof(CART_STORE_OWN_ROOT) : pPath + sizeof(CART_STORE_ROOT);
    const int Length = snprintf(pOut, Size, "%s/%s", CART_STORE_SIDECAR_ROOT, pName);
    return Length > 0 && (size_t)Length < Size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* --------------------------------------------------------- staging record */

bool CartStore_PsramHazard(uint32_t Address)
{
    /* PSRAMBIST_Burst (SHORTTEST): 8 bursts of 1 KiB at word k*262145, i.e.
     * byte k*0x80002; with burst wrap they may also fold into the 1 KiB
     * below. Treat both 1 KiB blocks touched as hazards. */
    for (uint32_t k = 0; k < 8; ++k)
    {
        const uint32_t Start = (k * 0x80002u) & ~(kBlock - 1u);
        const uint32_t End = ((k * 0x80002u) + kBlock + kBlock - 1u) & ~(kBlock - 1u);
        if (Address + kBlock > Start && Address < End) return true;
    }
    return false;
}

static void FreeRecord(void)
{
    free(Record);
    Record = NULL;
    RecordLoaded = false;
}

static bool FindEntry(const char *pName, CartStoreEntry_t *pEntry)
{
    /* "/cart/.cartridge/<own name>": the cart's own game, from offset 0 (read
     * in the GB core, so it can be played from memory with save states). */
    static const char kOwnDir[] = ".cartridge/";
    if (!strncmp(pName, kOwnDir, sizeof(kOwnDir) - 1))
    {
        if (!OwnName[0] || !OwnSize || strcmp(pName + sizeof(kOwnDir) - 1, OwnName)) return false;
        memset(pEntry, 0, sizeof(*pEntry));
        memcpy(pEntry->Name, OwnName, sizeof(pEntry->Name));
        pEntry->Size = OwnSize;
        return true;
    }
    for (size_t i = 0; i < EntryCount; ++i)
        if (strcmp(Entries[i].Name, pName) == 0) { *pEntry = Entries[i]; return true; }
    return false;
}

bool CartStore_CanReadCart(void)
{
    /* Only the GB/GBC core reads ModRetro carts reliably (the Game Gear core
     * reads 0xFF; the Lynx core has no cartridge engine). */
    return CoreSwitch_IsCore(kCoreSystem_GB) || CoreSwitch_Current() == kCoreSystem_None;
}

esp_err_t CartStore_StageBegin(const char *pPath)
{
    if (!CartStore_IsCartPath(pPath) || !CartStore_CanReadCart()) return ESP_ERR_INVALID_ARG;
    CartStoreEntry_t Entry;
    xSemaphoreTake(Lock, portMAX_DELAY);
    const bool Found = FindEntry(pPath + sizeof(CART_STORE_ROOT), &Entry);
    xSemaphoreGive(Lock);
    if (!Found) return ESP_ERR_NOT_FOUND;
    FreeRecord();
    Record = calloc(1, sizeof(*Record));
    if (!Record) return ESP_ERR_NO_MEM;
    Record->Magic = kRecordMagic;
    memcpy(Record->Name, Entry.Name, sizeof(Record->Name));
    Record->Size = Entry.Size;
    Record->EntryCrc = Entry.Crc;
    memset(Record->Head, 0xff, sizeof(Record->Head));
    StagedEntry = Entry;
    xSemaphoreTake(Lock, portMAX_DELAY);
    esp_err_t Result = ReadCart(Entry.Offset, Record->Head, 1);
    /* The SD copy (the caller has the SD mounted). With no card (the internal
     * store) only the CRC is checked here; StageMirror keeps what's needed. */
    const bool KeepCopy = !SDCard_IsVirtual();
    if (KeepCopy) (void)unlink(kStagedMirror);
    else (void)unlink(kStagedCopy);
    /* With no card nothing needs the whole game here: the upload that follows
     * reads it once (cart transfers are CRC-checked) and the caller compares
     * its CRC with the directory's. */
    if (!KeepCopy)
    {
        xSemaphoreGive(Lock);
        if (Result != ESP_OK) { printf("CARTSTORE head_error=%s\n", esp_err_to_name(Result)); FreeRecord(); }
        return Result;
    }
    uint8_t *pChunk = Result == ESP_OK ? malloc(kCacheBlocks * kBlock) : NULL;
    FILE *pCopy = pChunk && KeepCopy ? fopen(kStagedCopy, "wb") : NULL;
    if (Result == ESP_OK && (!pChunk || (KeepCopy && !pCopy))) Result = ESP_FAIL;
    uint32_t Crc = 0;
    for (uint32_t Done = 0; Result == ESP_OK && Done < Entry.Size; Done += kCacheBlocks * kBlock)
    {
        const uint32_t Want = Entry.Size - Done < kCacheBlocks * kBlock ? Entry.Size - Done : kCacheBlocks * kBlock;
        Result = ReadCart(Entry.Offset + Done, pChunk, (Want + kBlock - 1) / kBlock);
        if (Result == ESP_OK && pCopy && fwrite(pChunk, 1, Want, pCopy) != Want) Result = ESP_FAIL;
        Crc = esp_rom_crc32_le(Crc, pChunk, Want);
    }
    if (pCopy && fclose(pCopy) != 0 && Result == ESP_OK) Result = ESP_FAIL;
    free(pChunk);
    if (Result == ESP_OK && Crc != Entry.Crc) Result = ESP_ERR_INVALID_CRC;
    xSemaphoreGive(Lock);
    if (Result != ESP_OK)
    {
        printf("CARTSTORE copy_error=%s\n", esp_err_to_name(Result));
        (void)unlink(kStagedCopy);
        FreeRecord();
    }
    return Result;
}

/* The mirror bytes, then the trailer that identifies them (data first, so
 * each 4 KiB write fills one flash erase block of the internal store). */
static bool MirrorMatches(FILE *pFile, uint32_t EntryCrc, uint32_t Size, uint32_t Start, uint32_t *pLength)
{
    MirrorHeader_t Trailer;
    if (!pFile || fseek(pFile, -(long)sizeof(Trailer), SEEK_END) != 0 ||
        fread(&Trailer, sizeof(Trailer), 1, pFile) != 1 || Trailer.Magic != kMirrorMagic ||
        Trailer.EntryCrc != EntryCrc || Trailer.Size != Size || Trailer.Start != Start ||
        ftell(pFile) != (long)(Trailer.Length + sizeof(Trailer)))
        return false;
    if (pLength) *pLength = Trailer.Length;
    return fseek(pFile, 0, SEEK_SET) == 0;
}

esp_err_t CartStore_StageMirror(uint32_t Start, uint32_t Length)
{
    if (!Record || !StagedEntry.Size || Length % kBlock) return ESP_ERR_INVALID_STATE;
    /* Kept from the last time this game was staged: nothing to read or write. */
    FILE *pOld = fopen(kStagedMirror, "rb");
    uint32_t OldLength = 0;
    const bool Same = MirrorMatches(pOld, StagedEntry.Crc, StagedEntry.Size, Start, &OldLength) && OldLength == Length;
    if (pOld) fclose(pOld);
    if (Same)
    {
        printf("CARTSTORE mirror=kept start=0x%lx bytes=%lu\n", (unsigned long)Start, (unsigned long)Length);
        return ESP_OK;
    }
    enum { kChunk = kCacheBlocks * kBlock };
    uint8_t *pChunk = malloc(kChunk);
    (void)unlink(kStagedMirror);
    FILE *pFile = pChunk ? fopen(kStagedMirror, "wb") : NULL;
    SDCard_TuneWrite(pFile);
    esp_err_t Result = pFile ? ESP_OK : ESP_FAIL;
    xSemaphoreTake(Lock, portMAX_DELAY);
    for (uint32_t Done = 0; Result == ESP_OK && Done < Length;)
    {
        /* File offsets (Start + k) % Size: contiguous up to the end of the file. */
        const uint32_t Offset = (Start + Done) % StagedEntry.Size;
        uint32_t Count = kChunk;
        if (Count > Length - Done) Count = Length - Done;
        if (Count > StagedEntry.Size - Offset) Count = StagedEntry.Size - Offset;
        Result = ReadCart(StagedEntry.Offset + Offset, pChunk, Count / kBlock);
        if (Result == ESP_OK && fwrite(pChunk, 1, Count, pFile) != Count) Result = ESP_FAIL;
        Done += Count;
    }
    xSemaphoreGive(Lock);
    const MirrorHeader_t Trailer = {kMirrorMagic, StagedEntry.Crc, StagedEntry.Size, Start, Length};
    if (Result == ESP_OK && fwrite(&Trailer, sizeof(Trailer), 1, pFile) != 1) Result = ESP_FAIL;
    if (pFile && fclose(pFile) != 0 && Result == ESP_OK) Result = ESP_FAIL;
    free(pChunk);
    if (Result != ESP_OK) (void)unlink(kStagedMirror);
    printf("CARTSTORE mirror=%s start=0x%lx bytes=%lu\n", esp_err_to_name(Result), (unsigned long)Start, (unsigned long)Length);
    return Result;
}

void CartStore_StageSetRotation(uint8_t Rotation)
{
    if (Record) Record->Rotation = Rotation;
}

uint8_t CartStore_ReplayRotation(void)
{
    return Record ? Record->Rotation : 0;
}

FILE *CartStore_OpenMirror(uint32_t Start, uint32_t *pLength)
{
    if (!LoadRecord()) return NULL;
    FILE *pFile = fopen(kStagedMirror, "rb");
    if (MirrorMatches(pFile, Record->EntryCrc, Record->Size, Start, pLength)) return pFile;
    if (pFile) fclose(pFile);
    return NULL;
}

void CartStore_StageBlock(uint32_t Address, const uint8_t *pData)
{
    if (!Record || Record->Overflow || !CartStore_PsramHazard(Address)) return;
    unsigned Index;
    for (Index = 0; Index < Record->UniqueCount; ++Index)
        if (!memcmp(Record->Unique[Index], pData, kBlock)) break;
    if (Index == Record->UniqueCount)
    {
        if (Record->UniqueCount >= kMaxUnique) { Record->Overflow = 1; return; }
        memcpy(Record->Unique[Record->UniqueCount++], pData, kBlock);
    }
    for (unsigned i = 0; i < Record->AddressCount; ++i)
        if (Record->Addresses[i].Address == Address) { Record->Addresses[i].Unique = (uint8_t)Index; return; }
    if (Record->AddressCount >= kMaxAddresses) { Record->Overflow = 1; return; }
    Record->Addresses[Record->AddressCount].Address = Address;
    Record->Addresses[Record->AddressCount++].Unique = (uint8_t)Index;
}

static size_t RecordLength(const StageRecord_t *p)
{
    return offsetof(StageRecord_t, Unique) + p->UniqueCount * (size_t)kBlock;
}

esp_err_t CartStore_StageCommit(uint32_t RomCrc)
{
    if (!Record) return ESP_ERR_INVALID_STATE;
    esp_err_t Result = Record->Overflow ? ESP_ERR_INVALID_SIZE : ESP_OK;
    /* The upload's CRC covers the whole file for a Game Gear game: it must be
     * the directory's (with no card it is the only whole-game check). */
    const char *pExtension = strrchr(Record->Name, '.');
    if (Result == ESP_OK && RomCrc != Record->EntryCrc)
    {
        printf("CARTSTORE crc_mismatch upload=%08lx entry=%08lx\n", (unsigned long)RomCrc, (unsigned long)Record->EntryCrc);
        if (pExtension && !strcasecmp(pExtension, ".gg")) Result = ESP_ERR_INVALID_CRC;
    }
    Record->RomCrc = RomCrc;
    Record->Fresh = 1;
    nvs_handle_t Handle;
    if (Result == ESP_OK) Result = nvs_open(kNvsNamespace, NVS_READWRITE, &Handle);
    if (Result == ESP_OK)
    {
        Result = nvs_set_blob(Handle, kNvsKey, Record, RecordLength(Record));
        if (Result == ESP_OK) Result = nvs_set_u8(Handle, kNvsFreshKey, 1);
        if (Result == ESP_OK) Result = nvs_commit(Handle);
        nvs_close(Handle);
    }
    printf("CARTSTORE stage=%s name=\"%s\" blocks=%u unique=%u bytes=%u crc=%08lx\n", esp_err_to_name(Result),
           Record->Name, (unsigned)Record->AddressCount, (unsigned)Record->UniqueCount,
           (unsigned)RecordLength(Record), (unsigned long)RomCrc);
    FreeRecord();
    return Result;
}

static bool LoadRecord(void)
{
    if (RecordLoaded) return Record != NULL;
    RecordLoaded = true;
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &Handle) != ESP_OK) return false;
    Record = calloc(1, sizeof(*Record));
    size_t Length = sizeof(*Record);
    const bool Ok = Record && nvs_get_blob(Handle, kNvsKey, Record, &Length) == ESP_OK &&
        Length >= offsetof(StageRecord_t, Unique) && Record->Magic == kRecordMagic &&
        Record->UniqueCount <= kMaxUnique && Record->AddressCount <= kMaxAddresses &&
        Length == RecordLength(Record) &&
        strnlen(Record->Name, kCartStoreNameSize) < kCartStoreNameSize;
    for (unsigned i = 0; Ok && i < Record->AddressCount; ++i)
        if (Record->Addresses[i].Unique >= Record->UniqueCount) { nvs_close(Handle); FreeRecord(); RecordLoaded = true; return false; }
    nvs_close(Handle);
    if (!Ok) { free(Record); Record = NULL; }
    return Ok;
}

bool CartStore_ReplayReady(const char *pPath, uint32_t *pRomCrc, uint32_t *pSize)
{
    if (!CartStore_IsCartPath(pPath) || CartStore_CanReadCart()) return false;
    if (!LoadRecord() || strcmp(Record->Name, pPath + sizeof(CART_STORE_ROOT)) != 0) return false;
    nvs_handle_t Handle;
    uint8_t Fresh = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &Handle) == ESP_OK)
    {
        if (nvs_get_u8(Handle, kNvsFreshKey, &Fresh) != ESP_OK) Fresh = 0;
        nvs_close(Handle);
    }
    if (Fresh != 1) return false;
    if (pRomCrc) *pRomCrc = Record->RomCrc;
    if (pSize) *pSize = Record->Size;
    return true;
}

size_t CartStore_ReplayCount(void) { return Record ? Record->AddressCount : 0; }

bool CartStore_ReplayBlock(size_t Index, uint32_t *pAddress, const uint8_t **ppData)
{
    if (!Record || Index >= Record->AddressCount) return false;
    *pAddress = Record->Addresses[Index].Address;
    *ppData = Record->Unique[Record->Addresses[Index].Unique];
    return true;
}

void CartStore_ReplayDone(void)
{
    /* One use: PSRAM holds the game only until something else is loaded. */
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &Handle) == ESP_OK)
    {
        (void)nvs_erase_key(Handle, kNvsFreshKey);
        (void)nvs_commit(Handle);
        nvs_close(Handle);
    }
}

/* ------------------------------------------- the own game's save chip */

enum { kOwnSaveMax = 32 * 1024 }; /* ModRetro carts: 32 KiB (MBC5 banks of 8 KiB) */

typedef struct {
    const char *pPath;   /* Import: the temporary file; Export: the save */
    uint32_t Size;
    bool Export;
    uint32_t HeadCrc;   /* Import: out; Export: the cartridge must match */
    uint32_t Written;   /* Export: blocks rewritten */
} OwnSaveJob_t;

/* Leave the save chip as a cartridge starts: RAM bank 0 selected, RAM off.
 * (A game that boots next often reads its save without selecting a bank, so
 * a bank left selected here hides the save until the next cold power-on.) */
static void SaveWindowClose(void)
{
    (void)CartBulk_CartWrite(0x4000, 0x00);
    (void)CartBulk_CartWrite(0x0000, 0x00);
}

/* One 1 KiB block of the save chip, through the MBC5 RAM window. */
static esp_err_t OwnSaveBlock(uint32_t Offset, uint8_t *pData, bool Write)
{
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x0000, 0x0a), TAG, "save enable");
    esp_err_t Result = CartBulk_CartWrite(0x4000, (uint8_t)(Offset / 8192u));
    const uint16_t Address = (uint16_t)(0xa000u + Offset % 8192u);
    if (Result == ESP_OK) Result = Write ? CartBulk_WriteCartBlock(Address, pData, kBlock) : CartBulk_ReadBlock(Address, pData);
    SaveWindowClose();
    return Result;
}

static uint32_t HeaderCrc(const uint8_t Block0[kBlock]) { return esp_rom_crc32_le(0, &Block0[0x134], 0x150 - 0x134); }

static esp_err_t OwnSaveFn(void *pContext)
{
    OwnSaveJob_t *pJob = pContext;
    uint8_t *pCart = malloc(kBlock), *pFile = malloc(kBlock);
    FILE *pSave = NULL;
    esp_err_t Result = pCart && pFile ? CartBulk_ReadCart(0, pCart, 1) : ESP_ERR_NO_MEM;
    if (Result == ESP_OK && !pJob->Export && !pJob->Size)
    {
        /* Size from the cartridge header: battery-backed RAM only. */
        static const uint32_t kSizes[] = {0, 2048, 8192, 32768, 131072, 65536};
        const uint8_t Type = pCart[0x147], Code = pCart[0x149];
        const bool Battery = Type == 0x03 || Type == 0x06 || Type == 0x09 || Type == 0x0d || Type == 0x0f ||
                             Type == 0x10 || Type == 0x13 || Type == 0x1b || Type == 0x1e || Type == 0xff;
        uint32_t Size = Battery && Code < 6 ? kSizes[Code] : 0;
        if (Size > kOwnSaveMax) Size = kOwnSaveMax;
        pJob->Size = Size & ~(kBlock - 1u);
        if (!pJob->Size) Result = ESP_ERR_NOT_FOUND; /* no battery save to sync */
    }
    if (Result == ESP_OK)
    {
        const uint32_t Crc = HeaderCrc(pCart);
        if (pJob->Export && Crc != pJob->HeadCrc) Result = ESP_ERR_INVALID_STATE; /* another cart is in */
        pJob->HeadCrc = Crc;
    }
    if (Result == ESP_OK)
    {
        pSave = fopen(pJob->pPath, pJob->Export ? "rb" : "wb");
        if (!pSave) Result = pJob->Export && errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        else if (!pJob->Export) SDCard_TuneWrite(pSave);
    }
    for (uint32_t At = 0; Result == ESP_OK && At < pJob->Size; At += kBlock)
    {
        Result = OwnSaveBlock(At, pCart, false);
        if (Result != ESP_OK) break;
        if (!pJob->Export)
        {
            if (fwrite(pCart, 1, kBlock, pSave) != kBlock) Result = ESP_FAIL;
            continue;
        }
        if (fread(pFile, 1, kBlock, pSave) != kBlock) break; /* a shorter file: the rest stays */
        if (!memcmp(pFile, pCart, kBlock)) continue;
        Result = OwnSaveBlock(At, pFile, true);
        if (Result == ESP_OK) Result = OwnSaveBlock(At, pCart, false);
        if (Result == ESP_OK && memcmp(pFile, pCart, kBlock)) Result = ESP_ERR_INVALID_CRC;
        if (Result == ESP_OK) ++pJob->Written;
    }
    if (pSave && fclose(pSave) != 0 && Result == ESP_OK) Result = ESP_FAIL;
    free(pCart);
    free(pFile);
    return Result;
}

/* The pending write-back, if any: path (kPathSize-ish buffer), size, header CRC. */
static bool OwnSavePending(char *pPath, size_t PathSize, uint32_t *pSize, uint32_t *pHead, uint8_t *pKind)
{
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &Handle) != ESP_OK) return false;
    size_t Length = PathSize;
    *pKind = 0;
    const bool Found = nvs_get_str(Handle, kNvsOwnSaveKey, pPath, &Length) == ESP_OK &&
                       nvs_get_u32(Handle, kNvsOwnSaveSizeKey, pSize) == ESP_OK &&
                       nvs_get_u32(Handle, kNvsOwnSaveHeadKey, pHead) == ESP_OK;
    (void)nvs_get_u8(Handle, kNvsOwnSaveKindKey, pKind);
    nvs_close(Handle);
    return Found;
}

static void OwnSaveRecord(const char *pPath, uint32_t Size, uint32_t Head)
{
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &Handle) != ESP_OK) return;
    if (pPath)
    {
        (void)nvs_set_str(Handle, kNvsOwnSaveKey, pPath);
        (void)nvs_set_u32(Handle, kNvsOwnSaveSizeKey, Size);
        (void)nvs_set_u32(Handle, kNvsOwnSaveHeadKey, Head);
        (void)nvs_set_u8(Handle, kNvsOwnSaveKindKey, SDCard_IsVirtual() ? 1 : 0);
    }
    else
    {
        (void)nvs_erase_key(Handle, kNvsOwnSaveKey);
        (void)nvs_erase_key(Handle, kNvsOwnSaveSizeKey);
        (void)nvs_erase_key(Handle, kNvsOwnSaveHeadKey);
        (void)nvs_erase_key(Handle, kNvsOwnSaveKindKey);
    }
    (void)nvs_commit(Handle);
    nvs_close(Handle);
}

esp_err_t CartStore_OwnSaveImport(const char *pSavePath, uint32_t Size)
{
    if (!pSavePath || !CartStore_CanReadCart()) return ESP_ERR_INVALID_STATE;
    if (Size > kOwnSaveMax) Size = kOwnSaveMax;
    Size &= ~(kBlock - 1u); /* 0: from the cartridge header */
    char Pending[160];
    uint32_t PendingSize = 0, PendingHead = 0;
    uint8_t PendingKind = 0;
    if (OwnSavePending(Pending, sizeof(Pending), &PendingSize, &PendingHead, &PendingKind) &&
        !strcmp(Pending, pSavePath) && PendingKind == (SDCard_IsVirtual() ? 1 : 0))
    {
        /* Progress made here and not written back yet: the file is newer. */
        printf("CARTSTORE own_save=keep path=%s\n", pSavePath);
        return ESP_OK;
    }
    /* Read the whole chip into a temporary file first: a failed read never
     * costs the save file already there. */
    char Temp[168];
    if (snprintf(Temp, sizeof(Temp), "%s.imp", pSavePath) >= (int)sizeof(Temp)) return ESP_ERR_INVALID_SIZE;
    OwnSaveJob_t Job = {.pPath = Temp, .Size = Size};
    xSemaphoreTake(Lock, portMAX_DELAY);
    CacheValid = false;
    esp_err_t Result = WithCart(OwnSaveFn, &Job);
    xSemaphoreGive(Lock);
    if (Result == ESP_ERR_NOT_FOUND && !Job.Size) { (void)unlink(Temp); return ESP_OK; } /* no save */
    if (Result != ESP_OK) (void)unlink(Temp);
    else
    {
        (void)unlink(pSavePath);
        if (rename(Temp, pSavePath) != 0) Result = ESP_FAIL; /* (the chip still holds the save) */
    }
    if (Result == ESP_OK) OwnSaveRecord(pSavePath, Job.Size, Job.HeadCrc);
    printf("CARTSTORE own_save=import path=%s bytes=%lu result=%s\n", pSavePath, (unsigned long)Job.Size, esp_err_to_name(Result));
    DiagLog_Append("own save import %s", esp_err_to_name(Result));
    return Result;
}

esp_err_t CartStore_OwnSaveExport(const char *pSavePath)
{
    char Pending[160];
    uint32_t Size = 0, Head = 0;
    uint8_t Kind = 0;
    if (!OwnSavePending(Pending, sizeof(Pending), &Size, &Head, &Kind) || (pSavePath && strcmp(Pending, pSavePath)))
        return ESP_OK; /* nothing to write back */
    if (Kind != (SDCard_IsVirtual() ? 1 : 0))
    {
        /* Saved on the other storage (the card was put in or taken out):
         * wait until that storage is back. */
        DiagLog_Append("own save export wait storage=%s", Kind ? "internal" : "sd");
        return ESP_ERR_INVALID_STATE;
    }
    if (!CartStore_CanReadCart()) return ESP_ERR_INVALID_STATE;
    OwnSaveJob_t Job = {.pPath = Pending, .Size = Size, .Export = true, .HeadCrc = Head};
    xSemaphoreTake(Lock, portMAX_DELAY);
    CacheValid = false;
    const esp_err_t Result = WithCart(OwnSaveFn, &Job);
    xSemaphoreGive(Lock);
    /* Written back, or the file is gone: nothing left to do. Another cart in
     * the slot keeps the record for when this one is back. */
    if (Result == ESP_OK || Result == ESP_ERR_NOT_FOUND) OwnSaveRecord(NULL, 0, 0);
    printf("CARTSTORE own_save=export path=%s blocks=%lu result=%s\n", Pending, (unsigned long)Job.Written,
           esp_err_to_name(Result));
    DiagLog_Append("own save export %s blocks=%lu", esp_err_to_name(Result), (unsigned long)Job.Written);
    return Result;
}

static esp_err_t SyncArtFn(void *pContext);

/* Missing box art on the storage now mounted (Lock held). */
static bool ArtMissingLocked(void)
{
    char Path[sizeof(CART_STORE_ART_ROOT) + kCartStoreNameSize + 8];
    struct stat Info;
    for (size_t i = 0; i <= EntryCount; ++i)
    {
        const bool Own = i == EntryCount;
        if (Own ? !OwnArtSize || !OwnName[0] : !Entries[i].ArtSize) continue;
        snprintf(Path, sizeof(Path), "%s/%s.cma", CART_STORE_ART_ROOT, Own ? OwnName : Entries[i].Name);
        if (stat(Path, &Info) != 0) return true;
    }
    return false;
}

void CartStore_BootSync(void)
{
    char Pending[160];
    uint32_t Size = 0, Head = 0;
    uint8_t Kind = 0;
    const bool SaveDue = OwnSavePending(Pending, sizeof(Pending), &Size, &Head, &Kind);
    if (!SaveDue && !CartStore_HasGames()) return;
    if (!CartStore_CanReadCart() || !BootResume_CartridgeHeld())
    {
        if (SaveDue) DiagLog_Append("own save boot skip core=%d held=%u", (int)CoreSwitch_Current(),
                                    BootResume_CartridgeHeld() ? 1u : 0u);
        return;
    }
    sdmmc_card_t *pCard = NULL;
    const esp_err_t Mounted = SDCard_Mount(&pCard);
    if (Mounted != ESP_OK)
    {
        if (SaveDue) DiagLog_Append("own save boot storage=%s", esp_err_to_name(Mounted));
        return;
    }
    CartHeld = true; /* borrow the start-up hold: the cartridge is already in maintenance */
    if (SaveDue)
    {
        DiagLog_Append("own save boot sync");
        (void)CartStore_OwnSaveExport(NULL);
    }
    /* Box art for the menu on this storage (e.g. a card just put in): the
     * cartridge is held anyway, so no game restarts for it. */
    xSemaphoreTake(Lock, portMAX_DELAY);
    if (!Scanned) { Scanned = WithCart(ScanFn, NULL) == ESP_OK; if (Scanned) SaveDirectory(); }
    const bool Missing = Scanned && ArtMissingLocked();
    if (Missing) (void)WithCart(SyncArtFn, NULL);
    xSemaphoreGive(Lock);
    if (Missing) DiagLog_Append("cart art boot sync");
    CartHeld = false;
    (void)SDCard_Unmount(pCard);
}

/* -------------------------------------------------------------------- VFS */

static int VfsOpen(const char *pPath, int Flags, int Mode)
{
    (void)Mode;
    if ((Flags & O_ACCMODE) != O_RDONLY) { errno = EROFS; return -1; }
    const char *pName = pPath[0] == '/' ? pPath + 1 : pPath;
    int Fd = -1;
    for (int i = 0; i < kMaxFiles; ++i) if (!Files[i].Used) { Fd = i; break; }
    if (Fd < 0) { errno = ENFILE; return -1; }
    OpenFile_t File = {.Used = true};
    if (!CartStore_CanReadCart())
    {
        /* No cartridge access here: only the staged game's first 1 KiB. */
        if (!LoadRecord() || strcmp(Record->Name, pName) != 0) { errno = ENOENT; return -1; }
        File.HeadOnly = true;
        memcpy(File.Entry.Name, Record->Name, sizeof(File.Entry.Name));
        File.Entry.Size = Record->Size;
        File.Entry.Crc = Record->EntryCrc;
    }
    else
    {
        (void)CartStore_Refresh(false);
        xSemaphoreTake(Lock, portMAX_DELAY);
        const bool Found = FindEntry(pName, &File.Entry);
        xSemaphoreGive(Lock);
        if (!Found) { errno = ENOENT; return -1; }
    }
    Files[Fd] = File;
    return Fd;
}

static ssize_t VfsRead(int Fd, void *pDst, size_t Size)
{
    if (Fd < 0 || Fd >= kMaxFiles || !Files[Fd].Used) { errno = EBADF; return -1; }
    OpenFile_t *pFile = &Files[Fd];
    if (pFile->Position >= pFile->Entry.Size) return 0;
    if (Size > pFile->Entry.Size - pFile->Position) Size = pFile->Entry.Size - pFile->Position;
    uint8_t *pOut = pDst;
    size_t Done = 0;
    if (pFile->HeadOnly)
    {
        if (pFile->Position + Size <= kCartStoreHeadSize && Record)
        {
            memcpy(pOut, &Record->Head[pFile->Position], Size);
            pFile->Position += Size;
            return (ssize_t)Size;
        }
        /* Past the head: the GB core's SD copy (same size; CRC-checked when written). */
        if (!pFile->pCopy)
        {
            struct stat Stat;
            pFile->pCopy = stat(kStagedCopy, &Stat) == 0 && (uint32_t)Stat.st_size == pFile->Entry.Size
                ? fopen(kStagedCopy, "rb") : NULL;
        }
        if (!pFile->pCopy || fseek(pFile->pCopy, (long)pFile->Position, SEEK_SET) != 0 ||
            fread(pOut, 1, Size, pFile->pCopy) != Size)
        {
            errno = EIO;
            return -1;
        }
        pFile->Position += Size;
        return (ssize_t)Size;
    }
    xSemaphoreTake(Lock, portMAX_DELAY);
    while (Done < Size)
    {
        const uint32_t Absolute = pFile->Entry.Offset + pFile->Position;
        if (!CacheValid || Absolute < CacheOffset || Absolute >= CacheOffset + sizeof(Cache))
        {
            CacheOffset = Absolute & ~(kBlock - 1u);
            const esp_err_t Result = ReadCart(CacheOffset, Cache, kCacheBlocks);
            CacheValid = Result == ESP_OK;
            if (!CacheValid)
            {
                xSemaphoreGive(Lock);
                printf("CARTSTORE read_error=%s offset=0x%lx\n", esp_err_to_name(Result), (unsigned long)CacheOffset);
                errno = EIO;
                return -1;
            }
        }
        const uint32_t Within = Absolute - CacheOffset;
        size_t Chunk = sizeof(Cache) - Within;
        if (Chunk > Size - Done) Chunk = Size - Done;
        memcpy(pOut + Done, &Cache[Within], Chunk);
        Done += Chunk;
        pFile->Position += Chunk;
    }
    xSemaphoreGive(Lock);
    return (ssize_t)Done;
}

static off_t VfsLseek(int Fd, off_t Offset, int Whence)
{
    if (Fd < 0 || Fd >= kMaxFiles || !Files[Fd].Used) { errno = EBADF; return -1; }
    OpenFile_t *pFile = &Files[Fd];
    off_t Base = Whence == SEEK_SET ? 0 : Whence == SEEK_CUR ? (off_t)pFile->Position :
                 Whence == SEEK_END ? (off_t)pFile->Entry.Size : -1;
    if (Base < 0 || Base + Offset < 0) { errno = EINVAL; return -1; }
    pFile->Position = (uint32_t)(Base + Offset);
    return (off_t)pFile->Position;
}

static int VfsFstat(int Fd, struct stat *pStat)
{
    if (Fd < 0 || Fd >= kMaxFiles || !Files[Fd].Used) { errno = EBADF; return -1; }
    memset(pStat, 0, sizeof(*pStat));
    pStat->st_mode = S_IFREG | 0444;
    pStat->st_size = Files[Fd].Entry.Size;
    return 0;
}

static int VfsStat(const char *pPath, struct stat *pStat)
{
    const char *pName = pPath[0] == '/' ? pPath + 1 : pPath;
    memset(pStat, 0, sizeof(*pStat));
    if (!pName[0]) { pStat->st_mode = S_IFDIR | 0555; return 0; }
    CartStoreEntry_t Entry;
    xSemaphoreTake(Lock, portMAX_DELAY);
    const bool Found = Scanned && FindEntry(pName, &Entry);
    xSemaphoreGive(Lock);
    if (!Found) { errno = ENOENT; return -1; }
    pStat->st_mode = S_IFREG | 0444;
    pStat->st_size = Entry.Size;
    return 0;
}

static int VfsClose(int Fd)
{
    if (Fd < 0 || Fd >= kMaxFiles || !Files[Fd].Used) { errno = EBADF; return -1; }
    if (Files[Fd].pCopy) { fclose(Files[Fd].pCopy); Files[Fd].pCopy = NULL; }
    Files[Fd].Used = false;
    return 0;
}

esp_err_t CartStore_Init(void)
{
    Lock = xSemaphoreCreateMutexStatic(&LockStorage);
    const esp_vfs_t Vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = VfsOpen,
        .read = VfsRead,
        .lseek = VfsLseek,
        .fstat = VfsFstat,
        .stat = VfsStat,
        .close = VfsClose,
    };
    return esp_vfs_register(CART_STORE_ROOT, &Vfs, NULL);
}

/* ------------------------------------------------- writing (cartadd) */

typedef struct {
    int FileCount;
    char **ppFiles;
} AddRequest_t;

static esp_err_t Writes(const uint16_t (*pList)[2], size_t Count)
{
    for (size_t i = 0; i < Count; ++i)
        ESP_RETURN_ON_ERROR(CartBulk_CartWrite(pList[i][0], (uint8_t)pList[i][1]), TAG, "flash command");
    return ESP_OK;
}

static const uint16_t kReset[][2] = {{0x0000, 0xf0}};
static const uint16_t kIdentify[][2] = {{0x0aaa, 0xaa}, {0x0555, 0x55}, {0x0aaa, 0x90}};
static const uint16_t kCfi[][2] = {{0x00aa, 0x98}};

/* AMD-command flash chips: the ones on ModRetro carts (the ChroMagician/
 * FlashGBX "modretro" profiles) by ID, others by CFI. Capacity and the cart
 * engine's program mode (0x82 byte, 0x83 32-byte buffer, 0x85 256-byte). */
static esp_err_t IdentifyChip(uint32_t *pCapacity, uint8_t *pMode, uint8_t *pBlock)
{
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x2000, 0), TAG, "bank 0");
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x3000, 0), TAG, "bank 0 high");
    ESP_RETURN_ON_ERROR(Writes(kReset, 1), TAG, "reset");
    uint8_t Original[4];
    ESP_RETURN_ON_ERROR(CartBulk_ReadBlock(0, pBlock), TAG, "read");
    memcpy(Original, pBlock, 4);
    ESP_RETURN_ON_ERROR(Writes(kIdentify, 3), TAG, "identify");
    esp_err_t Result = CartBulk_ReadBlock(0, pBlock);
    uint8_t Id[4];
    memcpy(Id, pBlock, 4);
    (void)Writes(kReset, 1);
    if (Result != ESP_OK) return Result;
    printf("CARTADD chip_id=%02x%02x%02x%02x\n", Id[0], Id[1], Id[2], Id[3]);
    if (memcmp(Id, Original, 4) == 0) return ESP_ERR_NOT_SUPPORTED; /* not in ID mode: not flash */
    if (Id[0] == 0xbf && (Id[1] == 0xc8 || Id[1] == 0xc9))
    {
        *pCapacity = 2u << 20; *pMode = 0x82; /* SST 39VF1681/1682 */
        return ESP_OK;
    }
    static const uint8_t Is29[2][4] = {{0x01, 0x01, 0x7e, 0x7e}, {0x9d, 0x9d, 0x7e, 0x7e}};
    if (!memcmp(Id, Is29[0], 4) || !memcmp(Id, Is29[1], 4))
    {
        ESP_RETURN_ON_ERROR(Writes(kCfi, 1), TAG, "CFI");
        Result = CartBulk_ReadBlock(0, pBlock);
        (void)Writes(kReset, 1);
        if (Result != ESP_OK) return Result;
        if (pBlock[0x20] != 'Q' || pBlock[0x22] != 'R' || pBlock[0x24] != 'Y' || pBlock[0x4e] != 22)
            return ESP_ERR_NOT_SUPPORTED;
        *pCapacity = 4u << 20; *pMode = pBlock[0x54] >= 8 ? 0x85 : 0x83; /* IS29GL032 / S29GL032 */
        return ESP_OK;
    }
    /* Any other flash cart whose chip takes the same AMD commands at the same
     * addresses (0xAAA/0x555, as the cart engine sends them): ask it for its
     * size and write buffer (CFI). Anything else is refused untouched. */
    ESP_RETURN_ON_ERROR(Writes(kCfi, 1), TAG, "CFI");
    Result = CartBulk_ReadBlock(0, pBlock);
    (void)Writes(kReset, 1);
    if (Result != ESP_OK) return Result;
    const bool Cfi = pBlock[0x20] == 'Q' && pBlock[0x22] == 'R' && pBlock[0x24] == 'Y';
    const unsigned Algorithm = pBlock[0x26] | (pBlock[0x28] << 8), SizeLog = pBlock[0x4e], BufferLog = pBlock[0x54];
    printf("CARTADD cfi=%u algorithm=%u size_log=%u buffer_log=%u\n", Cfi ? 1u : 0u, Algorithm, SizeLog, BufferLog);
    if (!Cfi || (Algorithm != 2 && Algorithm != 4) || SizeLog < 17 || SizeLog > 26) return ESP_ERR_NOT_SUPPORTED;
    *pCapacity = SizeLog > 23 ? 8u << 20 : 1u << SizeLog; /* MBC5 reaches 8 MiB */
    *pMode = BufferLog >= 8 ? 0x85 : BufferLog >= 5 ? 0x83 : 0x82;
    return ESP_OK;
}

static esp_err_t CheckErased(uint32_t Offset, uint32_t Blocks, uint8_t *pBlock)
{
    for (uint32_t i = 0; i < Blocks; ++i)
    {
        ESP_RETURN_ON_ERROR(CartBulk_ReadCart(Offset + i * kBlock, pBlock, 1), TAG, "read");
        for (size_t j = 0; j < kBlock; ++j)
            if (pBlock[j] != 0xff)
            {
                printf("CARTADD not_erased offset=0x%lx\n", (unsigned long)(Offset + i * kBlock));
                return ESP_ERR_INVALID_STATE;
            }
    }
    return ESP_OK;
}

/* Program one 1 KiB block at At (pBlock: wanted bytes, pCart: scratch):
 * blank, partly written by an interrupted run, or already right. Programs
 * only where that just clears bits (NOR programming can't set a 0 back to
 * 1). *pProgrammed: whether it had to program. */
static esp_err_t ProgramBlock(uint32_t At, const uint8_t *pBlock, uint8_t Mode, uint8_t *pCart, const char *pTag,
                              bool *pProgrammed)
{
    esp_err_t Result = ESP_OK;
    if (pProgrammed) *pProgrammed = false;
    for (int Attempt = 0; Result == ESP_OK; ++Attempt)
    {
        Result = CartBulk_ReadCart(At, pCart, 1);
        if (Result != ESP_OK) break;
        if (!memcmp(pCart, pBlock, kBlock)) break;
        bool Programmable = true;
        for (size_t j = 0; j < kBlock && Programmable; ++j) Programmable = (pBlock[j] & ~pCart[j]) == 0;
        if (!Programmable)
        {
            printf("%s not_erased offset=0x%lx\n", pTag, (unsigned long)At);
            Result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (Attempt == 3) { Result = ESP_ERR_TIMEOUT; break; }
        if (Attempt > 0) printf("%s retry offset=0x%lx attempt=%d\n", pTag, (unsigned long)At, Attempt);
        const esp_err_t ProgramResult = CartBulk_ProgramCart(At, pBlock, Mode);
        if (ProgramResult == ESP_OK && Attempt == 0 && pProgrammed) *pProgrammed = true;
        if (ProgramResult != ESP_OK)
        {
            printf("%s program_error offset=0x%lx error=%s\n", pTag, (unsigned long)At, esp_err_to_name(ProgramResult));
            (void)CartBulk_ReadRangeAbort();
            (void)Writes(kReset, 1); /* back to read mode */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        /* Any outcome: read back and decide again. */
    }
    return Result;
}

/* Program Size bytes of pFile at cartridge Offset, then verify by CRC.
 * Each block is blank, partly written by an interrupted run, or already
 * right: program it only where that just clears bits (NOR programming can't
 * set a 0 back to 1), so a retry resumes safely. */
static esp_err_t ProgramFile(FILE *pFile, uint32_t Offset, uint32_t Size, uint8_t Mode, uint8_t *pBlock,
                             const char *pTag, uint32_t *pCrc)
{
    const uint32_t Blocks = (Size + kBlock - 1u) / kBlock;
    uint8_t *pCart = malloc(kBlock);
    esp_err_t Result = pCart ? ESP_OK : ESP_ERR_NO_MEM;
    uint32_t Crc = 0, Programmed = 0, Kept = 0;
    for (uint32_t b = 0; Result == ESP_OK && b < Blocks; ++b)
    {
        const uint32_t At = Offset + b * kBlock;
        memset(pBlock, 0xff, kBlock);
        const size_t Count = Size - b * kBlock < kBlock ? Size - b * kBlock : kBlock;
        if (fread(pBlock, 1, Count, pFile) != Count) { Result = ESP_FAIL; break; }
        Crc = esp_rom_crc32_le(Crc, pBlock, Count);
        bool Did = false;
        Result = ProgramBlock(At, pBlock, Mode, pCart, pTag, &Did);
        if (Result == ESP_OK) { if (Did) ++Programmed; else ++Kept; }
        if (Result != ESP_OK) printf("%s FAIL stage=program offset=0x%lx error=%s\n",
                                     pTag, (unsigned long)At, esp_err_to_name(Result));
        if ((b & 63u) == 63u) printf("%s progress=%lu/%lu\n", pTag, (unsigned long)(b + 1), (unsigned long)Blocks);
    }
    free(pCart);
    if (Result == ESP_OK) printf("%s blocks programmed=%lu kept=%lu\n", pTag, (unsigned long)Programmed, (unsigned long)Kept);
    uint32_t Check = 0;
    for (uint32_t b = 0; Result == ESP_OK && b < Blocks; ++b)
    {
        Result = CartBulk_ReadCart(Offset + b * kBlock, pBlock, 1);
        const size_t Count = Size - b * kBlock < kBlock ? Size - b * kBlock : kBlock;
        Check = esp_rom_crc32_le(Check, pBlock, Count);
    }
    if (Result == ESP_OK && Check != Crc)
    {
        printf("%s FAIL verify crc=%08lx cart=%08lx\n", pTag, (unsigned long)Crc, (unsigned long)Check);
        Result = ESP_ERR_INVALID_CRC;
    }
    *pCrc = Crc;
    return Result;
}

static bool GameName(const char *pName)
{
    const char *pDot = strrchr(pName, '.');
    static const char *const Extensions[] = {".gg", ".lnx", ".lyx", ".o", ".gb", ".gbc"};
    for (size_t i = 0; pDot && i < sizeof(Extensions) / sizeof(Extensions[0]); ++i)
        if (!strcasecmp(pDot, Extensions[i])) return true;
    return false;
}

static uint8_t SystemFor(const char *pName)
{
    const CoreSystem_t System = CoreSwitch_SystemForPath(pName);
    return System == kCoreSystem_GG ? 1 : System == kCoreSystem_Lynx ? 2 : 0;
}

static esp_err_t AddFn(void *pContext)
{
    AddRequest_t *pRequest = pContext;
    uint8_t *pBlock = malloc(kBlock), *pDir = malloc(kBlock);
    esp_err_t Result = pBlock && pDir ? ESP_OK : ESP_ERR_NO_MEM;
    uint32_t Capacity = 0;
    uint8_t Mode = 0;
    if (Result == ESP_OK) Result = IdentifyChip(&Capacity, &Mode, pBlock);
    if (Result != ESP_OK) printf("CARTADD FAIL stage=identify error=%s\n", esp_err_to_name(Result));

    /* Find the directory, or where a new one goes. */
    uint32_t DirOffset = 0, End = 0;
    int FreeSlot = -1;
    bool NewDir = false;
    if (Result == ESP_OK) Result = CartBulk_ReadCart(0, pBlock, 1);
    if (Result == ESP_OK)
    {
        uint32_t Candidates[2];
        const size_t CandidateCount = DirectoryCandidates(pBlock, Candidates);
        for (size_t i = 0; Result == ESP_OK && i < CandidateCount; ++i)
        {
            Result = CartBulk_ReadCart(Candidates[i], pDir, 1);
            if (Result == ESP_OK && !memcmp(pDir, kDir2Magic, sizeof(kDir2Magic)))
            {
                printf("CARTADD FAIL version 2 directory: change this cart's games with the installer\n");
                Result = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            if (Result == ESP_OK && ParseDirectory(pDir, NULL, &FreeSlot, &End) >= 0)
            {
                DirOffset = Candidates[i];
                break;
            }
        }
        if (Result == ESP_OK && DirOffset == 0)
        {
            /* Ordinary cart: the directory goes right after its game. */
            if (CandidateCount < 2) Result = ESP_ERR_NOT_SUPPORTED;
            else
            {
                DirOffset = Candidates[1];
                NewDir = true;
                FreeSlot = 0;
                memset(pDir, 0xff, kBlock);
                memcpy(pDir, kDirMagic, sizeof(kDirMagic));
                if (DirOffset + kBlock > Capacity) Result = ESP_ERR_NO_MEM;
                if (Result == ESP_OK) Result = CheckErased(DirOffset, 1, pBlock);
            }
        }
        if (End < DirOffset + kBlock) End = DirOffset + kBlock;
    }
    if (Result == ESP_OK)
        printf("CARTADD capacity=%lu mode=0x%02x dir=0x%lx new=%u next=0x%lx\n", (unsigned long)Capacity,
               Mode, (unsigned long)DirOffset, NewDir ? 1u : 0u, (unsigned long)End);
    else
        printf("CARTADD FAIL stage=directory error=%s\n", esp_err_to_name(Result));

    for (int f = 0; Result == ESP_OK && f < pRequest->FileCount; ++f)
    {
        const char *pPath = pRequest->ppFiles[f];
        const char *pName = strrchr(pPath, '/') ? strrchr(pPath, '/') + 1 : pPath;
        if (FreeSlot < 0 || FreeSlot >= kCartStoreSlots) { Result = ESP_ERR_NO_MEM; printf("CARTADD FAIL directory_full\n"); break; }
        if (strlen(pName) >= kCartStoreNameSize || !GameName(pName))
        { Result = ESP_ERR_INVALID_ARG; printf("CARTADD FAIL bad_name=\"%s\"\n", pName); break; }
        FILE *pFile = fopen(pPath, "rb");
        if (!pFile) { Result = ESP_ERR_NOT_FOUND; printf("CARTADD FAIL open=\"%s\"\n", pPath); break; }
        struct stat Stat;
        const uint32_t Size = fstat(fileno(pFile), &Stat) == 0 ? (uint32_t)Stat.st_size : 0;
        const uint32_t Blocks = (Size + kBlock - 1u) / kBlock;
        if (Size == 0 || End + Blocks * kBlock > Capacity)
        {
            fclose(pFile);
            Result = ESP_ERR_NO_MEM;
            printf("CARTADD FAIL no_space name=\"%s\" size=%lu free=%lu\n", pName, (unsigned long)Size,
                   (unsigned long)(Capacity > End ? Capacity - End : 0));
            break;
        }
        printf("CARTADD file=\"%s\" size=%lu offset=0x%lx\n", pName, (unsigned long)Size, (unsigned long)End);
        uint32_t Crc = 0;
        Result = ProgramFile(pFile, End, Size, Mode, pBlock, "CARTADD", &Crc);
        fclose(pFile);
        if (Result != ESP_OK) break;

        /* Program the directory slot (an empty slot only clears bits). */
        uint8_t *pSlot = &pDir[kSlotBase + FreeSlot * kSlotSize];
        memset(pSlot, 0, kSlotSize);
        memcpy(pSlot, kEntryMagic, 4);
        pSlot[4] = SystemFor(pName);
        pSlot[5] = 0xff;
        pSlot[6] = pSlot[7] = 0xff;
        Write32(&pSlot[8], End);
        Write32(&pSlot[12], Size);
        Write32(&pSlot[16], Crc);
        memcpy(&pSlot[24], pName, strlen(pName) + 1);
        Write32(&pSlot[20], EntryCrc(pSlot));
        Result = CartBulk_ProgramCart(DirOffset, pDir, Mode);
        if (Result == ESP_OK) Result = CartBulk_ReadCart(DirOffset, pBlock, 1);
        if (Result == ESP_OK && memcmp(pBlock, pDir, kBlock) != 0) Result = ESP_ERR_INVALID_CRC;
        if (Result != ESP_OK) { printf("CARTADD FAIL stage=directory_write error=%s\n", esp_err_to_name(Result)); break; }
        printf("CARTADD added=\"%s\" slot=%d crc=%08lx\n", pName, FreeSlot, (unsigned long)Crc);
        End += Blocks * kBlock;
        int Next = -1;
        (void)ParseDirectory(pDir, NULL, &Next, NULL);
        FreeSlot = Next;
    }
    free(pBlock);
    free(pDir);
    return Result;
}

static int AddCommand(int argc, char **argv)
{
    if (argc < 2) { printf("CARTADD FAIL usage: cartadd </sdcard/...> [...]\n"); return 1; }
    if (!CartStore_CanReadCart()) { printf("CARTADD FAIL needs_gb_core\n"); return 1; }
    sdmmc_card_t *pCard = NULL;
    esp_err_t Result = SDCard_MountPhysical(&pCard);
    AddRequest_t Request = {argc - 1, &argv[1]};
    if (Result == ESP_OK)
    {
        xSemaphoreTake(Lock, portMAX_DELAY);
        CacheValid = false;
        Result = WithCart(AddFn, &Request);
        xSemaphoreGive(Lock);
        (void)SDCard_Unmount(pCard);
    }
    printf("CARTADD %s error=%s\n", Result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(Result));
    (void)CartStore_Refresh(true);
    return Result == ESP_OK ? 0 : 1;
}

static int DirCommand(int argc, char **argv)
{
    (void)argc; (void)argv;
    const size_t Count = CartStore_Refresh(true);
    for (size_t i = 0; i < Count; ++i)
    {
        CartStoreEntry_t Entry;
        if (CartStore_Entry(i, &Entry))
            printf("CARTDIR %u system=%u offset=0x%lx size=%lu crc=%08lx name=\"%s\"\n", (unsigned)i, Entry.System,
                   (unsigned long)Entry.Offset, (unsigned long)Entry.Size, (unsigned long)Entry.Crc, Entry.Name);
    }
    printf("CARTDIR DONE count=%u\n", (unsigned)Count);
    return 0;
}

/* cartpeek <address> [bank]: one engine ReadBlock (diagnostics). */
static esp_err_t PeekFn(void *pContext)
{
    const uint32_t *p = pContext;
    if (p[1] != UINT32_MAX)
    {
        ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x3000, (uint8_t)(p[1] >> 8)), TAG, "bank");
        ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x2000, (uint8_t)p[1]), TAG, "bank");
    }
    uint8_t *pBlock = malloc(kBlock);
    if (!pBlock) return ESP_ERR_NO_MEM;
    esp_err_t Result = CartBulk_ReadBlock((uint16_t)p[0], pBlock);
    if (Result == ESP_OK)
    {
        uint32_t Ff = 0;
        for (size_t i = 0; i < kBlock; ++i) Ff += pBlock[i] == 0xff;
        printf("CARTPEEK address=%04lx ff=%lu data=", (unsigned long)p[0], (unsigned long)Ff);
        for (size_t i = 0; i < 16; ++i) printf("%02x", pBlock[i]);
        printf("\n");
    }
    free(pBlock);
    return Result;
}

static int PeekCommand(int argc, char **argv)
{
    uint32_t Args[2] = {argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0,
                        argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : UINT32_MAX};
    xSemaphoreTake(Lock, portMAX_DELAY);
    const esp_err_t Result = WithCart(PeekFn, Args);
    xSemaphoreGive(Lock);
    printf("CARTPEEK %s\n", esp_err_to_name(Result));
    return Result != ESP_OK;
}

/* ---------------------------------------------- whole-cart service commands
 * Back up, erase and rewrite a cartridge's flash (e.g. to turn a cart into a
 * dedicated Magic Trick cart and back). The cart's save memory is separate
 * and untouched. */

typedef struct { uint32_t Start, Blocks; } DumpRequest_t;

/* cartdump [start] [blocks]: every 1 KiB block as one console line,
 * "CARTDUMP <offset> <crc32> <hex|FF>" (FF: the whole block reads 0xFF). */
static esp_err_t DumpFn(void *pContext)
{
    DumpRequest_t *p = pContext;
    uint8_t *pBlock = malloc(kBlock);
    if (!pBlock) return ESP_ERR_NO_MEM;
    uint32_t Capacity = 0;
    uint8_t Mode = 0;
    esp_err_t Result = IdentifyChip(&Capacity, &Mode, pBlock);
    if (Result != ESP_OK) { free(pBlock); return Result; }
    const uint32_t Total = Capacity / kBlock;
    if (p->Start >= Total) { free(pBlock); return ESP_ERR_INVALID_ARG; }
    const uint32_t Blocks = p->Blocks && p->Blocks < Total - p->Start ? p->Blocks : Total - p->Start;
    printf("CARTDUMP BEGIN capacity=%lu start=%lu blocks=%lu\n", (unsigned long)Capacity, (unsigned long)p->Start,
           (unsigned long)Blocks);
    static const char kHex[] = "0123456789abcdef";
    char *pLine = malloc(kBlock * 2 + 1);
    if (!pLine) { free(pBlock); return ESP_ERR_NO_MEM; }
    uint32_t Crc = 0;
    for (uint32_t b = 0; Result == ESP_OK && b < Blocks; ++b)
    {
        const uint32_t Offset = (p->Start + b) * kBlock;
        Result = CartBulk_ReadCart(Offset, pBlock, 1);
        if (Result != ESP_OK) break;
        Crc = esp_rom_crc32_le(Crc, pBlock, kBlock);
        bool Blank = true;
        for (size_t j = 0; j < kBlock && Blank; ++j) Blank = pBlock[j] == 0xff;
        if (Blank) strcpy(pLine, "FF");
        else
        {
            for (size_t j = 0; j < kBlock; ++j)
            { pLine[2 * j] = kHex[pBlock[j] >> 4]; pLine[2 * j + 1] = kHex[pBlock[j] & 15]; }
            pLine[kBlock * 2] = 0;
        }
        printf("CARTDUMP %06lx %08lx %s\n", (unsigned long)Offset,
               (unsigned long)esp_rom_crc32_le(0, pBlock, kBlock), pLine);
    }
    free(pLine);
    free(pBlock);
    if (Result == ESP_OK) printf("CARTDUMP DONE crc=%08lx\n", (unsigned long)Crc);
    return Result;
}

static int DumpCommand(int argc, char **argv)
{
    if (!CartStore_CanReadCart()) { printf("CARTDUMP FAIL needs_gb_core\n"); return 1; }
    DumpRequest_t Request = {argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) / kBlock : 0,
                             argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0};
    xSemaphoreTake(Lock, portMAX_DELAY);
    const esp_err_t Result = WithCart(DumpFn, &Request);
    xSemaphoreGive(Lock);
    printf("CARTDUMP %s error=%s\n", Result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(Result));
    return Result != ESP_OK;
}

static const uint16_t kChipErase[][2] = {{0x0aaa, 0xaa}, {0x0555, 0x55}, {0x0aaa, 0x80},
                                         {0x0aaa, 0xaa}, {0x0555, 0x55}, {0x0aaa, 0x10}};

/* carterase yes: chip erase, then check every block reads 0xFF. */
static esp_err_t EraseFn(void *pContext)
{
    (void)pContext;
    uint8_t *pBlock = malloc(kBlock);
    if (!pBlock) return ESP_ERR_NO_MEM;
    uint32_t Capacity = 0;
    uint8_t Mode = 0;
    esp_err_t Result = IdentifyChip(&Capacity, &Mode, pBlock);
    if (Result == ESP_OK)
    {
        printf("CARTERASE capacity=%lu\n", (unsigned long)Capacity);
        Result = Writes(kChipErase, sizeof(kChipErase) / sizeof(kChipErase[0]));
    }
    /* Busy until DQ7 polling reads 1 (data 0xFF) twice in a row; IS29 chips
     * take up to about a minute, SST ones well under a second. */
    const int64_t Start = esp_timer_get_time();
    unsigned Settled = 0;
    while (Result == ESP_OK && Settled < 2)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        Result = CartBulk_ReadBlock(0, pBlock);
        if (Result != ESP_OK) break;
        Settled = pBlock[0] == 0xff ? Settled + 1 : 0;
        if (esp_timer_get_time() - Start > 120000000) Result = ESP_ERR_TIMEOUT;
    }
    (void)Writes(kReset, 1);
    if (Result == ESP_OK)
        printf("CARTERASE erased ms=%lld; checking\n", (long long)((esp_timer_get_time() - Start) / 1000));
    for (uint32_t b = 0; Result == ESP_OK && b < Capacity / kBlock; ++b)
    {
        Result = CheckErased(b * kBlock, 1, pBlock);
        if ((b & 255u) == 255u) printf("CARTERASE checked=%lu/%lu\n", (unsigned long)(b + 1), (unsigned long)(Capacity / kBlock));
    }
    free(pBlock);
    return Result;
}

static int EraseCommand(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes")) { printf("CARTERASE FAIL usage: carterase yes (erases the whole cartridge flash)\n"); return 1; }
    if (!CartStore_CanReadCart()) { printf("CARTERASE FAIL needs_gb_core\n"); return 1; }
    xSemaphoreTake(Lock, portMAX_DELAY);
    CacheValid = false;
    const esp_err_t Result = WithCart(EraseFn, NULL);
    xSemaphoreGive(Lock);
    printf("CARTERASE %s error=%s\n", Result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(Result));
    (void)CartStore_Refresh(true);
    return Result != ESP_OK;
}

/* cartwrite </sdcard/image>: program a whole-cart image from offset 0 onto
 * erased flash (or resume an interrupted write), then verify. */
static esp_err_t WriteFn(void *pContext)
{
    const char *pPath = pContext;
    uint8_t *pBlock = malloc(kBlock);
    if (!pBlock) return ESP_ERR_NO_MEM;
    uint32_t Capacity = 0;
    uint8_t Mode = 0;
    esp_err_t Result = IdentifyChip(&Capacity, &Mode, pBlock);
    FILE *pFile = Result == ESP_OK ? fopen(pPath, "rb") : NULL;
    if (Result == ESP_OK && !pFile) Result = ESP_ERR_NOT_FOUND;
    struct stat Stat;
    const uint32_t Size = pFile && fstat(fileno(pFile), &Stat) == 0 ? (uint32_t)Stat.st_size : 0;
    if (Result == ESP_OK && (Size == 0 || Size > Capacity)) Result = ESP_ERR_INVALID_SIZE;
    uint32_t Crc = 0;
    if (Result == ESP_OK)
    {
        printf("CARTWRITE capacity=%lu mode=0x%02x size=%lu\n", (unsigned long)Capacity, Mode, (unsigned long)Size);
        Result = ProgramFile(pFile, 0, Size, Mode, pBlock, "CARTWRITE", &Crc);
    }
    if (pFile) fclose(pFile);
    if (Result == ESP_OK) printf("CARTWRITE crc=%08lx\n", (unsigned long)Crc);
    free(pBlock);
    return Result;
}

static int WriteCommand(int argc, char **argv)
{
    if (argc != 2) { printf("CARTWRITE FAIL usage: cartwrite </sdcard/image>\n"); return 1; }
    if (!CartStore_CanReadCart()) { printf("CARTWRITE FAIL needs_gb_core\n"); return 1; }
    sdmmc_card_t *pCard = NULL;
    esp_err_t Result = SDCard_MountPhysical(&pCard);
    if (Result == ESP_OK)
    {
        xSemaphoreTake(Lock, portMAX_DELAY);
        CacheValid = false;
        Result = WithCart(WriteFn, argv[1]);
        xSemaphoreGive(Lock);
        (void)SDCard_Unmount(pCard);
    }
    printf("CARTWRITE %s error=%s\n", Result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(Result));
    (void)CartStore_Refresh(true);
    return Result != ESP_OK;
}

/* ------------------------------------- installer access (pcsd, USB) */

/* Cartridge reads go up to 16 KiB at a time (less when RAM is short, e.g.
 * with Wi-Fi running). */
static uint32_t FlashScratchBlocks;
static bool FlashOpen;
/* From the start of Begin to the end of End: USB TRANSFERS and Wi-Fi Files
 * keep re-asserting PC mode, which must leave the cartridge to the session. */
static volatile bool FlashWanted;
static bool FlashRestorePCMode;
bool CartStore_FlashIsOpen(void) { return FlashWanted; }
static uint8_t FlashMode;
static uint32_t FlashCapacity;
static uint32_t FlashSaveSize; /* cartridge save memory (MBC5 banks of 8 KiB) */
static uint8_t *pFlashScratch;
static const uint16_t kEraseUnlock[][2] = {{0x0aaa, 0xaa}, {0x0555, 0x55}, {0x0aaa, 0x80},
                                           {0x0aaa, 0xaa}, {0x0555, 0x55}};

esp_err_t CartStore_FlashBegin(uint32_t *pCapacity)
{
    if (!CartStore_CanReadCart()) return ESP_ERR_INVALID_STATE;
    if (FlashOpen) { if (pCapacity) *pCapacity = FlashCapacity; return ESP_OK; }
    /* PC mode (USB TRANSFERS, Wi-Fi Files) holds the cartridge in maintenance:
     * leave it for this session's own hold, and come back to it at End. */
    FlashWanted = true;
    FlashRestorePCMode = false;
    if (CartBackup_IsPCModeActive())
    {
        const esp_err_t Left = CartBackup_SetPCMode(false);
        if (Left != ESP_OK) { FlashWanted = false; return Left; }
        FlashRestorePCMode = true;
    }
    xSemaphoreTake(Lock, portMAX_DELAY);
    CacheValid = false;
    /* Leave ~12 KiB for whatever else runs (Wi-Fi Files idles at ~15 KiB
     * free): a 1 KiB scratch only costs speed. */
    const size_t Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t Largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    for (FlashScratchBlocks = 16; FlashScratchBlocks > 1; FlashScratchBlocks /= 4)
        if (FlashScratchBlocks * kBlock <= Largest && Free >= FlashScratchBlocks * kBlock + 12 * 1024) break;
    pFlashScratch = malloc(FlashScratchBlocks * kBlock);
    DiagLog_Append("cartflash open free=%u largest=%u scratch=%u", (unsigned)Free, (unsigned)Largest,
                   pFlashScratch ? (unsigned)FlashScratchBlocks : 0u);
    esp_err_t Result = pFlashScratch ? CartBulk_Begin() : ESP_ERR_NO_MEM;
    const bool Bulk = Result == ESP_OK;
    if (Result == ESP_OK) Result = CartBulk_CartEnter(NULL);
    const bool Entered = Result == ESP_OK;
    if (Result == ESP_OK) Result = IdentifyChip(&FlashCapacity, &FlashMode, pFlashScratch);
    /* Save memory size from the cart's header; ModRetro carts have 32 KiB,
     * and a header without RAM (e.g. a Magic Trick cart) still has the chip. */
    if (Result == ESP_OK && CartBulk_ReadBlock(0, pFlashScratch) == ESP_OK)
    {
        static const uint32_t kSizes[] = {0, 2048, 8192, 32768, 131072, 65536};
        const uint8_t Code = pFlashScratch[0x149];
        FlashSaveSize = Code < 6 && kSizes[Code] >= 8192 ? kSizes[Code] : 32768;
    }
    if (Result != ESP_OK)
    {
        if (Entered) (void)CartBulk_CartExit();
        if (Bulk) CartBulk_End();
        free(pFlashScratch); pFlashScratch = NULL;
        xSemaphoreGive(Lock);
        if (FlashRestorePCMode) (void)CartBackup_SetPCMode(true);
        FlashRestorePCMode = FlashWanted = false;
        return Result;
    }
    FlashOpen = true;
    if (pCapacity) *pCapacity = FlashCapacity;
    printf("CARTFLASH open capacity=%lu mode=0x%02x scratch=%lu\n", (unsigned long)FlashCapacity, FlashMode,
           (unsigned long)FlashScratchBlocks);
    return ESP_OK;
}

static bool FlashRange(uint32_t Offset, uint32_t Length)
{
    return FlashOpen && !(Offset & (kBlock - 1u)) && Length && Offset < FlashCapacity && Length <= FlashCapacity - Offset;
}

esp_err_t CartStore_FlashReadShared(uint32_t Offset, uint32_t *pBlocks, const uint8_t **ppData)
{
    if (!pBlocks || !ppData || !*pBlocks) return ESP_ERR_INVALID_ARG;
    if (*pBlocks > FlashScratchBlocks) *pBlocks = FlashScratchBlocks;
    if (!FlashRange(Offset, *pBlocks * kBlock)) return ESP_ERR_INVALID_ARG;
    *ppData = pFlashScratch;
    return CartBulk_ReadCart(Offset, pFlashScratch, *pBlocks);
}

esp_err_t CartStore_FlashRead(uint32_t Offset, uint8_t *pData, uint32_t Blocks)
{
    if (!pData || !FlashRange(Offset, Blocks * kBlock)) return ESP_ERR_INVALID_ARG;
    return CartBulk_ReadCart(Offset, pData, Blocks);
}

/* Sector erase at Offset. The bank is selected first and status is polled
 * through the 0x4000 window without bank writes (a bank value of 0xB0 would
 * be an erase-suspend command to the chip). */
static esp_err_t EraseSector(uint32_t Offset)
{
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x3000, (uint8_t)((Offset >> 22) & 1u)), TAG, "bank high");
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x2000, (uint8_t)(Offset >> 14)), TAG, "bank");
    ESP_RETURN_ON_ERROR(Writes(kEraseUnlock, sizeof(kEraseUnlock) / sizeof(kEraseUnlock[0])), TAG, "unlock");
    const uint16_t Window = (uint16_t)(0x4000u | (Offset & 0x3c00u));
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(Window, 0x30), TAG, "erase");
    const int64_t Start = esp_timer_get_time();
    unsigned Settled = 0;
    esp_err_t Result = ESP_OK;
    while (Result == ESP_OK && Settled < 2)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        Result = CartBulk_ReadBlock(Window, pFlashScratch);
        if (Result != ESP_OK) break;
        Settled = pFlashScratch[0] == 0xff ? Settled + 1 : 0;
        if (esp_timer_get_time() - Start > 10000000) Result = ESP_ERR_TIMEOUT;
    }
    (void)Writes(kReset, 1);
    return Result;
}

/* Erase the 64 KiB block at Offset (64 KiB aligned): every chip's sectors
 * are 64 KiB or smaller and aligned, so nothing outside it is touched.
 * Erases wherever it doesn't read blank yet (one command for a 64 KiB
 * sector, several for 4 or 8 KiB ones), then checks it all reads 0xFF. */
esp_err_t CartStore_FlashErase(uint32_t Offset)
{
    enum { kEraseBlock = 64 * 1024, kChunk = 4 * 1024 };
    if ((Offset & (kEraseBlock - 1u)) || !FlashRange(Offset, kEraseBlock)) return ESP_ERR_INVALID_ARG;
    esp_err_t Result = ESP_OK;
    unsigned Erases = 0;
    for (uint32_t At = Offset; Result == ESP_OK && At < Offset + kEraseBlock; )
    {
        bool Blank = true;
        const uint32_t Step = FlashScratchBlocks < kChunk / kBlock ? FlashScratchBlocks : kChunk / kBlock;
        for (uint32_t b = 0; Result == ESP_OK && Blank && b < kChunk / kBlock; b += Step)
        {
            Result = CartBulk_ReadCart(At + b * kBlock, pFlashScratch, Step);
            for (size_t j = 0; Result == ESP_OK && Blank && j < Step * kBlock; ++j) Blank = pFlashScratch[j] == 0xff;
        }
        if (Result != ESP_OK) break;
        if (Blank) { At += kChunk; continue; }
        if (++Erases > 24) { Result = ESP_ERR_INVALID_STATE; break; }
        Result = EraseSector(At);
    }
    printf("CARTFLASH erase offset=0x%lx sectors=%u result=%s\n", (unsigned long)Offset, Erases, esp_err_to_name(Result));
    return Result;
}

esp_err_t CartStore_FlashProgram(uint32_t Offset, const uint8_t *pData)
{
    if (!pData || !FlashRange(Offset, kBlock)) return ESP_ERR_INVALID_ARG;
    return ProgramBlock(Offset, pData, FlashMode, pFlashScratch, "CARTFLASH", NULL);
}

uint32_t CartStore_FlashSaveSize(void) { return FlashOpen ? FlashSaveSize : 0; }

/* Save memory, MBC5 style: enable RAM (0x0000 = 0x0A), bank at 0x4000,
 * 8 KiB window at 0xA000; disabled again after each block. */
static esp_err_t SaveWindow(uint32_t Offset, uint16_t *pAddress)
{
    if (!FlashOpen || (Offset & (kBlock - 1u)) || Offset >= FlashSaveSize) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x0000, 0x0a), TAG, "save enable");
    ESP_RETURN_ON_ERROR(CartBulk_CartWrite(0x4000, (uint8_t)(Offset / 8192u)), TAG, "save bank");
    *pAddress = (uint16_t)(0xa000u + Offset % 8192u);
    return ESP_OK;
}

esp_err_t CartStore_FlashSaveRead(uint32_t Offset, uint8_t *pData)
{
    uint16_t Address;
    ESP_RETURN_ON_ERROR(SaveWindow(Offset, &Address), TAG, "save window");
    const esp_err_t Result = CartBulk_ReadBlock(Address, pData);
    SaveWindowClose();
    return Result;
}

esp_err_t CartStore_FlashSaveWrite(uint32_t Offset, const uint8_t *pData)
{
    uint16_t Address;
    ESP_RETURN_ON_ERROR(SaveWindow(Offset, &Address), TAG, "save window");
    esp_err_t Result = CartBulk_WriteCartBlock(Address, pData, kBlock);
    if (Result == ESP_OK) Result = CartBulk_ReadBlock(Address, pFlashScratch);
    if (Result == ESP_OK && memcmp(pFlashScratch, pData, kBlock)) Result = ESP_ERR_INVALID_CRC;
    SaveWindowClose();
    return Result;
}

esp_err_t CartStore_FlashCrc(uint32_t Offset, uint32_t Length, uint32_t *pCrc)
{
    if (!pCrc || !FlashRange(Offset, Length)) return ESP_ERR_INVALID_ARG;
    uint32_t Crc = 0;
    esp_err_t Result = ESP_OK;
    for (uint32_t At = Offset; Result == ESP_OK && At < Offset + Length; )
    {
        const uint32_t Left = Offset + Length - At;
        const uint32_t Count = Left < FlashScratchBlocks * kBlock ? Left : FlashScratchBlocks * kBlock;
        Result = CartBulk_ReadCart(At, pFlashScratch, (Count + kBlock - 1u) / kBlock);
        if (Result == ESP_OK) Crc = esp_rom_crc32_le(Crc, pFlashScratch, Count);
        At += Count;
    }
    *pCrc = Crc;
    return Result;
}

/* Copies box art off the cartridge (session open, Lock held, storage
 * mounted) to CART_STORE_ART_ROOT. Replace: also rewrite art that changed
 * and remove art of games no longer on the cartridge; otherwise only add
 * what's missing (a quick check before each cartridge game loads). */
/* One box art file: Name.cma from the cartridge at Offset (Size bytes,
 * CRC-32 Crc). Returns 1 copied, 0 already there, -1 failed. */
static int CopyArtLocked(const char *pName, uint32_t Offset, uint32_t Size, uint32_t Crc, bool Replace,
                         uint8_t *pChunk, uint32_t ChunkSize)
{
    char Path[sizeof(CART_STORE_ART_ROOT) + kCartStoreNameSize + 8];
    snprintf(Path, sizeof(Path), "%s/%s.cma", CART_STORE_ART_ROOT, pName);
    struct stat Info;
    const bool Have = stat(Path, &Info) == 0 && (uint32_t)Info.st_size == Size;
    if (Have && !Replace) return 0;
    if (Have)
    {
        FILE *pOld = fopen(Path, "rb");
        uint32_t OldCrc = 0;
        size_t Got;
        while (pOld && (Got = fread(pChunk, 1, ChunkSize, pOld)) > 0) OldCrc = esp_rom_crc32_le(OldCrc, pChunk, Got);
        if (pOld) fclose(pOld);
        if (pOld && OldCrc == Crc) return 0;
    }
    (void)unlink(Path);
    FILE *pFile = fopen(Path, "wb");
    SDCard_TuneWrite(pFile);
    bool Ok = pFile != NULL;
    uint32_t Got = 0;
    for (uint32_t At = 0; Ok && At < Size; At += ChunkSize)
    {
        const uint32_t Count = Size - At < ChunkSize ? Size - At : ChunkSize;
        Ok = CartBulk_ReadCart(Offset + At, pChunk, (Count + kBlock - 1u) / kBlock) == ESP_OK &&
             fwrite(pChunk, 1, Count, pFile) == Count;
        Got = esp_rom_crc32_le(Got, pChunk, Count);
    }
    if (pFile && fclose(pFile) != 0) Ok = false;
    if (Ok && Got != Crc) Ok = false;
    if (Ok) return 1;
    (void)unlink(Path);
    printf("CARTSTORE art_failed=\"%s\" open=%u errno=%d\n", pName, pFile ? 1u : 0u, errno);
    return -1;
}

/* Copies box art off the cartridge (session open, Lock held, storage
 * mounted) to CART_STORE_ART_ROOT: each game's, and the cart's own game's
 * (as "<TITLE> (CARTRIDGE).gb[c].cma"). Replace: also rewrite art that
 * changed and remove art of games no longer on the cartridge; otherwise only
 * add what's missing (a quick check before each cartridge game loads). */
static void SyncArtLocked(bool Replace)
{
    enum { kMaxArt = 64 * 1024 }; /* streamed: no big buffer */
    if (mkdir(CART_STORE_SIDECAR_ROOT, 0775) != 0 && errno != EEXIST) return;
    if (mkdir(CART_STORE_ART_ROOT, 0775) != 0 && errno != EEXIST) return;
    uint32_t kChunk = 4 * 1024;
    uint8_t *pChunk = malloc(kChunk);
    if (!pChunk) pChunk = malloc(kChunk = kBlock);
    if (!pChunk) { printf("CARTSTORE art_nomem\n"); return; }
    unsigned Copied = 0, Failed = 0;
    for (size_t i = 0; i <= EntryCount; ++i)
    {
        const bool Own = i == EntryCount;
        const CartStoreEntry_t *e = Own ? NULL : &Entries[i];
        const char *pName = Own ? OwnName : e->Name;
        const uint32_t Size = Own ? OwnArtSize : e->ArtSize;
        if (!Size || Size > kMaxArt || !pName[0]) continue;
        const int Done = CopyArtLocked(pName, Own ? OwnArtOffset : e->ArtOffset, Size, Own ? OwnArtCrc : e->ArtCrc,
                                       Replace, pChunk, kChunk);
        if (Done > 0) ++Copied;
        if (Done < 0) ++Failed;
    }
    free(pChunk);
    unsigned Removed = 0;
    char Path[sizeof(CART_STORE_ART_ROOT) + kCartStoreNameSize + 8];
    DIR *pDir = Replace ? opendir(CART_STORE_ART_ROOT) : NULL;
    for (struct dirent *d; pDir && (d = readdir(pDir)) != NULL; )
    {
        const size_t Length = strlen(d->d_name);
        if (Length < 5 || strcasecmp(d->d_name + Length - 4, ".cma")) continue;
        bool Keep = OwnArtSize && OwnName[0] && strlen(OwnName) == Length - 4 && !strncmp(OwnName, d->d_name, Length - 4);
        for (size_t i = 0; i < EntryCount && !Keep; ++i)
            Keep = Entries[i].ArtSize && strlen(Entries[i].Name) == Length - 4 &&
                   !strncmp(Entries[i].Name, d->d_name, Length - 4);
        if (Keep) continue;
        snprintf(Path, sizeof(Path), "%s/%.*s", CART_STORE_ART_ROOT, (int)(sizeof(Path) - sizeof(CART_STORE_ART_ROOT) - 1), d->d_name);
        if (unlink(Path) == 0) ++Removed;
    }
    if (pDir) closedir(pDir);
    printf("CARTSTORE art copied=%u failed=%u removed=%u\n", Copied, Failed, Removed);
}

static esp_err_t SyncArtFn(void *pContext) { (void)pContext; SyncArtLocked(false); return ESP_OK; }

void CartStore_SyncArt(void)
{
    if (!CartStore_CanReadCart()) return;
    xSemaphoreTake(Lock, portMAX_DELAY);
    bool Needed = false;
    if (!Scanned) { Scanned = WithCart(ScanFn, NULL) == ESP_OK; if (Scanned) SaveDirectory(); }
    Needed = ArtMissingLocked();
    if (Needed) (void)WithCart(SyncArtFn, NULL); /* only then touch the cartridge */
    xSemaphoreGive(Lock);
}

/* Re-read the directory (for the menu and the other cores), bring the
 * menu's box art up to date, then let the cartridge go: it restarts.
 * Returns the game count. */
size_t CartStore_FlashEnd(bool StorageMounted)
{
    if (!FlashOpen) return 0;
    (void)Writes(kReset, 1);
    Scanned = ScanFn(NULL) == ESP_OK;
    if (Scanned) SaveDirectory();
    sdmmc_card_t *pCard = NULL;
    if (Scanned && (StorageMounted || SDCard_Mount(&pCard) == ESP_OK))
    {
        SyncArtLocked(true);
        if (pCard) (void)SDCard_Unmount(pCard);
    }
    const size_t Count = EntryCount;
    (void)CartBulk_CartExit();
    CartBulk_End();
    free(pFlashScratch); pFlashScratch = NULL;
    FlashOpen = false;
    xSemaphoreGive(Lock);
    if (FlashRestorePCMode)
    {
        const esp_err_t Back = CartBackup_SetPCMode(true);
        if (Back != ESP_OK) printf("CARTFLASH pcmode_restore=%s\n", esp_err_to_name(Back));
    }
    FlashRestorePCMode = FlashWanted = false;
    printf("CARTFLASH close games=%u own=\"%s\"\n", (unsigned)Count, OwnName);
    return Count;
}

esp_err_t CartStore_RegisterConsoleCommands(void)
{
    const esp_console_cmd_t Commands[] = {
        {.command = "cartpeek", .help = "Read one cartridge block (diagnostics)", .hint = "<address> [bank]", .func = PeekCommand},
        {.command = "cartdir", .help = "List the Magic Trick games on the inserted cartridge", .func = DirCommand},
        {.command = "cartadd", .help = "Add SD games to the cartridge's free (erased) flash",
         .hint = "</sdcard/...> [...]", .func = AddCommand},
        {.command = "cartdump", .help = "Print the cartridge flash as hex lines (backup)", .hint = "[start] [blocks]", .func = DumpCommand},
        {.command = "carterase", .help = "Erase the whole cartridge flash (not its save memory)", .hint = "yes", .func = EraseCommand},
        {.command = "cartwrite", .help = "Write a whole-cart image from the SD card onto erased flash", .hint = "</sdcard/image>", .func = WriteCommand},
    };
    for (size_t i = 0; i < sizeof(Commands) / sizeof(Commands[0]); ++i)
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&Commands[i]), TAG, "register");
    return ESP_OK;
}
