#include "cart_store.h"

#include "cart_bulk.h"
#include "core_switch.h"
#include "sd_card.h"

#include "esp_check.h"
#include "esp_console.h"
#include "esp_rom_crc.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

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
static const char kEntryMagic[4] = {'M', 'T', 'E', '1'};
static const char kNvsNamespace[] = "cartstore";
static const char kNvsKey[] = "stage";
static const char kNvsDirKey[] = "dir";
/* 1 from staging until the target core has replayed it (the record itself
 * stays, so that core can still open the game for later reads). */
static const char kNvsFreshKey[] = "fresh";
/* Full copy of the staged game, for reads the target core needs past the
 * first 1 KiB (e.g. restoring the Game Gear ROM mirror after a save or
 * state import). Written by the GB core while staging. */
static const char kStagedCopy[] = CART_STORE_SIDECAR_ROOT "/.staged.bin";

static SemaphoreHandle_t Lock;
static StaticSemaphore_t LockStorage;

static CartStoreEntry_t Entries[kCartStoreSlots];
static size_t EntryCount;
static bool Scanned;

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
    struct { uint32_t Address; uint8_t Unique; uint8_t Pad[3]; } Addresses[kMaxAddresses];
    uint8_t Head[kCartStoreHeadSize];
    uint8_t Unique[kMaxUnique][kBlock];
} StageRecord_t;

static StageRecord_t *Record;      /* staging or replay in progress */
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
    pEntry->System = pSlot[4];
    pEntry->Offset = Offset;
    pEntry->Size = Size;
    pEntry->Crc = Read32(&pSlot[16]);
    return true;
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
static esp_err_t WithCart(esp_err_t (*Fn)(void *), void *pContext)
{
    const bool Own = !CartBulk_IsSessionActive();
    esp_err_t Result = Own ? CartBulk_Begin() : ESP_OK;
    if (Result != ESP_OK) return Result;
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

static esp_err_t ReadCart(uint32_t Offset, uint8_t *pData, uint32_t Blocks)
{
    ReadRequest_t Request = {Offset, pData, Blocks};
    return WithCart(ReadFn, &Request);
}

static esp_err_t ScanFn(void *pContext)
{
    (void)pContext;
    uint8_t *pBlock = malloc(kBlock);
    if (!pBlock) return ESP_ERR_NO_MEM;
    EntryCount = 0;
    esp_err_t Result = CartBulk_ReadCart(0, pBlock, 1);
    uint32_t Candidates[2];
    const size_t CandidateCount = Result == ESP_OK ? DirectoryCandidates(pBlock, Candidates) : 0;
    for (size_t i = 0; Result == ESP_OK && i < CandidateCount; ++i)
    {
        Result = CartBulk_ReadCart(Candidates[i], pBlock, 1);
        const int Count = Result == ESP_OK ? ParseDirectory(pBlock, Entries, NULL, NULL) : -1;
        if (Count >= 0)
        {
            EntryCount = (size_t)Count;
            printf("CARTSTORE dir=0x%lx entries=%d\n", (unsigned long)Candidates[i], Count);
            break;
        }
    }
    free(pBlock);
    return Result;
}

/* The directory as last read by the GB core, for cores that cannot read
 * the cartridge (their Cartridge folder lists it; choosing a game goes back
 * through the GB core to stage it). */
static void SaveDirectory(void)
{
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &Handle) != ESP_OK) return;
    CartStoreEntry_t Old[kCartStoreSlots];
    size_t Length = sizeof(Old);
    const bool Same = nvs_get_blob(Handle, kNvsDirKey, Old, &Length) == ESP_OK &&
        Length == EntryCount * sizeof(CartStoreEntry_t) && !memcmp(Old, Entries, Length);
    if (!Same)
    {
        const esp_err_t Result = EntryCount ? nvs_set_blob(Handle, kNvsDirKey, Entries, EntryCount * sizeof(CartStoreEntry_t))
                                            : nvs_erase_key(Handle, kNvsDirKey);
        if (Result == ESP_OK) (void)nvs_commit(Handle);
    }
    nvs_close(Handle);
}

static void LoadDirectory(void)
{
    EntryCount = 0;
    nvs_handle_t Handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &Handle) != ESP_OK) return;
    size_t Length = sizeof(Entries);
    if (nvs_get_blob(Handle, kNvsDirKey, Entries, &Length) == ESP_OK && Length % sizeof(CartStoreEntry_t) == 0)
        EntryCount = Length / sizeof(CartStoreEntry_t);
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
    const int Length = snprintf(pOut, Size, "%s/%s", CART_STORE_SIDECAR_ROOT, pPath + sizeof(CART_STORE_ROOT));
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
    xSemaphoreTake(Lock, portMAX_DELAY);
    esp_err_t Result = ReadCart(Entry.Offset, Record->Head, 1);
    /* The SD copy (the caller has the SD mounted). */
    uint8_t *pChunk = Result == ESP_OK ? malloc(kCacheBlocks * kBlock) : NULL;
    FILE *pCopy = pChunk ? fopen(kStagedCopy, "wb") : NULL;
    if (Result == ESP_OK && (!pChunk || !pCopy)) Result = ESP_FAIL;
    uint32_t Crc = 0;
    for (uint32_t Done = 0; Result == ESP_OK && Done < Entry.Size; Done += kCacheBlocks * kBlock)
    {
        const uint32_t Want = Entry.Size - Done < kCacheBlocks * kBlock ? Entry.Size - Done : kCacheBlocks * kBlock;
        Result = ReadCart(Entry.Offset + Done, pChunk, (Want + kBlock - 1) / kBlock);
        if (Result == ESP_OK && fwrite(pChunk, 1, Want, pCopy) != Want) Result = ESP_FAIL;
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

/* AMD-command flash chips on ModRetro carts (the ChroMagician/FlashGBX
 * "modretro" profiles): capacity and the cart engine's program mode. */
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
    return ESP_ERR_NOT_SUPPORTED;
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
        /* Each block is blank, partly written by an interrupted cartadd, or
         * already right: program it only where that just clears bits (NOR
         * programming can't set a 0 back to 1), so a retry resumes safely. */
        uint8_t *pCart = malloc(kBlock);
        if (!pCart) Result = ESP_ERR_NO_MEM;
        uint32_t Crc = 0, Programmed = 0, Kept = 0;
        for (uint32_t b = 0; Result == ESP_OK && b < Blocks; ++b)
        {
            const uint32_t Offset = End + b * kBlock;
            memset(pBlock, 0xff, kBlock);
            const size_t Count = Size - b * kBlock < kBlock ? Size - b * kBlock : kBlock;
            if (fread(pBlock, 1, Count, pFile) != Count) { Result = ESP_FAIL; break; }
            Crc = esp_rom_crc32_le(Crc, pBlock, Count);
            for (int Attempt = 0; Result == ESP_OK; ++Attempt)
            {
                Result = CartBulk_ReadCart(Offset, pCart, 1);
                if (Result != ESP_OK) break;
                if (!memcmp(pCart, pBlock, kBlock)) { if (Attempt == 0) ++Kept; break; }
                bool Programmable = true;
                for (size_t j = 0; j < kBlock && Programmable; ++j) Programmable = (pBlock[j] & ~pCart[j]) == 0;
                if (!Programmable)
                {
                    printf("CARTADD not_erased offset=0x%lx\n", (unsigned long)Offset);
                    Result = ESP_ERR_INVALID_STATE;
                    break;
                }
                if (Attempt == 3) { Result = ESP_ERR_TIMEOUT; break; }
                if (Attempt > 0) printf("CARTADD retry offset=0x%lx attempt=%d\n", (unsigned long)Offset, Attempt);
                const esp_err_t ProgramResult = CartBulk_ProgramCart(Offset, pBlock, Mode);
                if (ProgramResult == ESP_OK && Attempt == 0) ++Programmed;
                if (ProgramResult != ESP_OK)
                {
                    printf("CARTADD program_error offset=0x%lx error=%s\n", (unsigned long)Offset, esp_err_to_name(ProgramResult));
                    (void)CartBulk_ReadRangeAbort();
                    (void)Writes(kReset, 1); /* back to read mode */
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                /* Any outcome: read back and decide again. */
            }
            if (Result != ESP_OK) printf("CARTADD FAIL stage=program offset=0x%lx error=%s\n",
                                         (unsigned long)Offset, esp_err_to_name(Result));
            if ((b & 63u) == 63u) printf("CARTADD progress=%lu/%lu\n", (unsigned long)(b + 1), (unsigned long)Blocks);
        }
        free(pCart);
        if (Result == ESP_OK) printf("CARTADD blocks programmed=%lu kept=%lu\n", (unsigned long)Programmed, (unsigned long)Kept);
        fclose(pFile);
        /* Verify against the file's CRC. */
        uint32_t Check = 0;
        for (uint32_t b = 0; Result == ESP_OK && b < Blocks; ++b)
        {
            Result = CartBulk_ReadCart(End + b * kBlock, pBlock, 1);
            const size_t Count = Size - b * kBlock < kBlock ? Size - b * kBlock : kBlock;
            Check = esp_rom_crc32_le(Check, pBlock, Count);
        }
        if (Result == ESP_OK && Check != Crc)
        {
            printf("CARTADD FAIL verify crc=%08lx cart=%08lx\n", (unsigned long)Crc, (unsigned long)Check);
            Result = ESP_ERR_INVALID_CRC;
        }
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
    esp_err_t Result = SDCard_Mount(&pCard);
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

esp_err_t CartStore_RegisterConsoleCommands(void)
{
    const esp_console_cmd_t Commands[] = {
        {.command = "cartpeek", .help = "Read one cartridge block (diagnostics)", .hint = "<address> [bank]", .func = PeekCommand},
        {.command = "cartdir", .help = "List the Magic Trick games on the inserted cartridge", .func = DirCommand},
        {.command = "cartadd", .help = "Add SD games to the cartridge's free (erased) flash",
         .hint = "</sdcard/...> [...]", .func = AddCommand},
    };
    for (size_t i = 0; i < sizeof(Commands) / sizeof(Commands[0]); ++i)
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&Commands[i]), TAG, "register");
    return ESP_OK;
}
