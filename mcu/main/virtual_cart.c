#include "gg_cheats.h"
#include "core_switch.h"
#include "virtual_cart.h"
#include "gg_rom.h"
#include "state_save_branch.h"
#include "state_file.h"
#include "last_game.h"
#include "recent_games.h"
#include "cheats.h"
#include "cart_backup.h"

#include "cart_bulk.h"
#include "cart_link.h"
#include "cart_mapper.h"
#include "cart_rtc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwrmgr.h"
#include "pc_backup.h"
#include "rom_browser.h"
#include "rom_browser_ui.h"
#include "sd_card.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    kRomBase = 0x020000,
    kSaveBase = 0x420000,
    kMaximumRomSize = 4 * 1024 * 1024,
    kMaximumSaveSize = 128 * 1024,
    kHeaderSize = 0x150,
    kRomBankSize = 16 * 1024,
    kPathSize = 320,
    kLoadTaskStack = 8 * 1024,
    kLoadTaskPriority = 4,
    kVirtualCommandStop = 0,
    kVirtualCommandStart = 1,
    kVirtualCommandStatus = 2,
    kVirtualCommandPrepare = 3,
    kVirtualCommandSaveBlock = 4,
    kVirtualCommandRtcRestoreLow = 5,
    kVirtualCommandRtcRestoreHigh = 6,
    kVirtualCommandRtcSnapshot = 7,
    kVirtualCommandQuiesce = 8,
    kVirtualCommandResume = 9,
    /* Native states: 10 save, 11 load, 12 status, 13 debug (below). */
    kVirtualCommandStateBlock = 14,
    kVirtualCommandStateImport = 15,
    kStateSlotBase = 0x440000,
    /* Game Gear save states (gg_state_engine.sv): 10 freezes, 14 reads a block,
     * 11 restores a payload staged at ROM address 0x3F0000 (inside the 4 MiB ROM
     * window, above any Game Gear ROM; the MCU rewrites the ROM mirror there
     * afterwards), 16 releases. Status bit 1 = frozen, bit 4 = load complete. */
    kVirtualCommandGGFreeze = 10,
    kVirtualCommandGGLoad = 11,
    kVirtualCommandGGRelease = 16,
    kGGStateRomAddress = 0x3F0000,
    kGGStatusFrozen = 0x02,
    kGGStatusFault = 0x04,
    kGGStatusLoaded = 0x10,
    kVirtualCapabilityRtcState = 1 << 7,
    kVirtualStartMbc1m = 0x80,
    kVirtualStatusEnabled = 1 << 0,
    kVirtualStatusInitialized = 1 << 1,
    kVirtualStatusSavePending = 1 << 2,
    kVirtualStatusDisableSeen = 1 << 3,
    kVirtualStatusResetAsserted = 1 << 4,
    kVirtualStatusResetReleased = 1 << 5,
    kVirtualStatusBootRomHigh = 1 << 6,
    kVirtualStatusBootRomExited = 1 << 7,
    kVirtualStatusPostExitFrame = 1 << 8,
    kVirtualStatusQuiesced = 1 << 9,
    kVirtualBootAttempts = 3,
    kVirtualBootPolls = 200,
    kVirtualPreparePolls = 100,
    kSaveTaskStack = 6 * 1024,
    kSaveTaskPriority = 3,
    kSavePollPeriod_ms = 250,
    kNativeAutosaveWait_ms = 5000,
    kRtcPersistPeriod_ms = 60 * 1000,
    kForcedSavePasses = 3,
    kRtcStateSize = 4,
};

typedef struct {
    CartMapperInfo_t Mapper;
    uint32_t RomSize;
    uint32_t MappedRomSize;
    uint32_t RamSize;
    uint16_t GlobalChecksum;
    uint16_t Configuration;
    uint8_t ConfigurationHigh;
    uint8_t MapperSelect;
    bool Mbc1m;
    bool GameGear;
    char Title[17];
} VirtualCartInfo_t;

static const char *TAG = "VirtualCart";
static const char *RomDirectory = "/sdcard/CHROMAGIC/BACKUPS/";
static portMUX_TYPE StateLock = portMUX_INITIALIZER_UNLOCKED;
static bool Running;
static bool AutosaveRunning;
static bool NativeRequested;
static void (*StateWaitCallback)(void);
void VirtualCart_RegisterStateWait(void (*Callback)(void)) { StateWaitCallback=Callback; }
static bool StateUncertain;
static char StateCanonicalPath[kPathSize];
static bool LoadRequested;
static bool Active;
static bool ActiveSavePending;
static VirtualCartInfo_t ActiveInfo;
static char ActiveSavePath[kPathSize];
static char ActiveRtcPath[kPathSize];
static char ActiveRomPath[kPathSize];
static uint32_t GGRomCrc;
/* SD copy of the native state ("<rom>.state"); empty when unavailable. */
/* Three save-state slots per game: "<rom>.state", ".state2", ".state3". The
 * FPGA has one working slot; FpgaStateOwner names the slot it holds (0: none). */
enum { kStateSlots = 3 };
static char ActiveStatePath[kStateSlots][kPathSize];
static uint8_t StateOnSdMask, FpgaStateOwner;
static bool LastStateSdFailed;
static uint32_t ActiveRtcPersistedState;
static bool ActiveRtcPersistedValid;
static bool ActiveRtcSupported;
static TickType_t ActiveRtcLastCheck;
static char PendingPath[kPathSize];
static DRAM_ATTR uint8_t TransferBuffer[kCartBulkBlockSize];
static uint16_t SaveSequence;

static const uint8_t NintendoLogo[48] = {
    0xce, 0xed, 0x66, 0x66, 0xcc, 0x0d, 0x00, 0x0b,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0c, 0x00, 0x0d,
    0x00, 0x08, 0x11, 0x1f, 0x88, 0x89, 0x00, 0x0e,
    0xdc, 0xcc, 0x6e, 0xe6, 0xdd, 0xdd, 0xd9, 0x99,
    0xbb, 0xbb, 0x67, 0x63, 0x6e, 0x0e, 0xec, 0xcc,
    0xdd, 0xdc, 0x99, 0x9f, 0xbb, 0xb9, 0x33, 0x3e,
};

_Static_assert(kCartMapper_RomOnly == 0 && kCartMapper_Mbc1 == 1 &&
               kCartMapper_Mbc2 == 2 && kCartMapper_Mbc3 == 3 &&
               kCartMapper_Mbc5 == 4 && kCartMapper_Huc1 == 5,
               "FPGA and MCU mapper selectors must agree");

static bool SetRunning(bool Value)
{
    bool Changed = false;
    portENTER_CRITICAL(&StateLock);
    if (Running != Value && (!Value || (!LoadRequested && !StateUncertain && !NativeRequested)))
    {
        Running = Value;
        if (!Value) AutosaveRunning = false;
        Changed = true;
    }
    portEXIT_CRITICAL(&StateLock);
    return Changed;
}

// Distinguish a short background save from Stop, a ROM load, or another snapshot.
static bool BeginAutosave(void)
{
    portENTER_CRITICAL(&StateLock);
    const bool Available = !Running && !LoadRequested && !StateUncertain && !NativeRequested;
    if (Available) { Running = true; AutosaveRunning = true; }
    portEXIT_CRITICAL(&StateLock);
    return Available;
}

static esp_err_t BeginNativeOperation(void)
{
    const TickType_t Started = xTaskGetTickCount();
    const char *Reason = NULL;
    bool Wait = false;
    portENTER_CRITICAL(&StateLock);
    if (StateUncertain) Reason = "uncertain_restore";
    else if (LoadRequested) Reason = "rom_change";
    else if (NativeRequested) Reason = "native_request_pending";
    else if (Running && !AutosaveRunning) Reason = "operation_busy";
    else {
        // Reserve the next operation so neither autosave nor a ROM change can
        // overtake this request between cleanup and acquiring Running.
        NativeRequested = true;
        Wait = Running;
    }
    portEXIT_CRITICAL(&StateLock);
    if (Reason) { printf("STATE_REJECT=%s\n", Reason); return ESP_ERR_INVALID_STATE; }
    if (Wait) { printf("STATE_WAIT=autosave\n");if(StateWaitCallback)StateWaitCallback(); }
    for (;;) {
        esp_err_t Result = ESP_ERR_NOT_FINISHED;
        const TickType_t Now = xTaskGetTickCount();
        portENTER_CRITICAL(&StateLock);
        if (StateUncertain || LoadRequested) {
            NativeRequested = false;
            Result = ESP_ERR_INVALID_STATE;
        } else if (!Running) {
            Running = true;
            NativeRequested = false;
            Result = ESP_OK;
        } else if ((TickType_t)(Now - Started) >= pdMS_TO_TICKS(kNativeAutosaveWait_ms)) {
            NativeRequested = false;
            Result = ESP_ERR_TIMEOUT;
        }
        portEXIT_CRITICAL(&StateLock);
        if (Result != ESP_ERR_NOT_FINISHED) {
            if (Result == ESP_ERR_TIMEOUT) printf("STATE_REJECT=autosave_timeout\n");
            if (Result == ESP_ERR_INVALID_STATE) printf("STATE_REJECT=state_changed_while_waiting\n");
            return Result;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool SetLoadRequested(bool Value)
{
    portENTER_CRITICAL(&StateLock);
    const bool Changed = LoadRequested != Value && (!Value || (!StateUncertain && !NativeRequested));
    if (Changed) LoadRequested = Value;
    portEXIT_CRITICAL(&StateLock);
    return Changed;
}

static bool BeginRequestedLoad(void)
{
    portENTER_CRITICAL(&StateLock);
    const bool Available = LoadRequested && !Running && !StateUncertain && !NativeRequested;
    if (Available) Running = true;
    portEXIT_CRITICAL(&StateLock);
    return Available;
}

bool VirtualCart_IsBusy(void)
{
    portENTER_CRITICAL(&StateLock);
    const bool Busy = Running || LoadRequested || StateUncertain || NativeRequested;
    portEXIT_CRITICAL(&StateLock);
    return Busy;
}

static uint32_t NativeSession=1;
static void SetActive(bool Value)
{
    bool Changed;
    portENTER_CRITICAL(&StateLock);
    Changed = Active != Value;
    Active = Value;
    if(++NativeSession==0)++NativeSession;
    portEXIT_CRITICAL(&StateLock);

    if (Changed)
    {
        if (Value)
        {
            PwrMgr_InhibitSleep();
        }
        else
        {
            PwrMgr_AllowSleep();
        }
    }
}

bool VirtualCart_IsActive(void)
{
    bool Value;
    portENTER_CRITICAL(&StateLock);
    Value = Active;
    portEXIT_CRITICAL(&StateLock);
    return Value;
}

static esp_err_t ResolvePath(const char *pInput, char Output[kPathSize])
{
    if (pInput == NULL || pInput[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    const int Length = pInput[0] == '/'
        ? snprintf(Output, kPathSize, "%s", pInput)
        : snprintf(Output, kPathSize, "%s%s", RomDirectory, pInput);
    if (Length < 0 || Length >= kPathSize)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *pSlash = strrchr(Output, '/');
    const char *pDot = strrchr(Output, '.');
    if (pDot == NULL || (pSlash != NULL && pDot < pSlash) ||
        (strcasecmp(pDot, ".gb") != 0 && strcasecmp(pDot, ".gbc") != 0 && strcasecmp(pDot, ".gg") != 0))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t DecodeRomSize(uint8_t Code, uint32_t *pSize)
{
    if (Code <= 8)
    {
        *pSize = 32u * 1024u << Code;
        return ESP_OK;
    }
    if (Code >= 0x52 && Code <= 0x54)
    {
        static const uint8_t Banks[] = {72, 80, 96};
        *pSize = (uint32_t)Banks[Code - 0x52] * kRomBankSize;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ResolveGeometry(const CartMapperInfo_t *pMapper,
                                 uint8_t RomCode, uint8_t RamCode,
                                 uint32_t RomSize, uint32_t *pRamSize)
{
    static const uint32_t RamSizes[] = {
        0, 2u * 1024u, 8u * 1024u, 32u * 1024u,
        128u * 1024u, 64u * 1024u,
    };
    if (RamCode >= sizeof(RamSizes) / sizeof(RamSizes[0]) ||
        RomSize > kMaximumRomSize)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t MaximumRom;
    uint32_t MaximumRam;
    switch (pMapper->Kind)
    {
        case kCartMapper_RomOnly:
            MaximumRom = 32u * 1024u;
            MaximumRam = 8u * 1024u;
            break;
        case kCartMapper_Mbc1:
            MaximumRom = 2u * 1024u * 1024u;
            MaximumRam = 32u * 1024u;
            break;
        case kCartMapper_Mbc2:
            MaximumRom = 256u * 1024u;
            MaximumRam = 512u;
            break;
        case kCartMapper_Mbc3:
            MaximumRom = (RomCode == 7 || RamCode == 5)
                ? 4u * 1024u * 1024u : 2u * 1024u * 1024u;
            MaximumRam = (RomCode == 7 || RamCode == 5)
                ? 64u * 1024u : 32u * 1024u;
            break;
        case kCartMapper_Mbc5:
            MaximumRom = kMaximumRomSize;
            MaximumRam = pMapper->HasRumble
                ? 64u * 1024u : 128u * 1024u;
            break;
        case kCartMapper_Huc1:
            MaximumRom = 1024u * 1024u;
            MaximumRam = 32u * 1024u;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    if (RomSize > MaximumRom)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t RamSize = 0;
    if (pMapper->Kind == kCartMapper_Mbc2)
    {
        RamSize = 512;
    }
    else if (pMapper->HasRam)
    {
        RamSize = RamSizes[RamCode];
        if (RamSize == 0 || RamSize > MaximumRam)
        {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    *pRamSize = RamSize;
    return ESP_OK;
}

static uint8_t RamMask(uint32_t RamSize)
{
    switch (RamSize)
    {
        case 2u * 1024u: return 1;
        case 32u * 1024u: return 3;
        case 64u * 1024u: return 7;
        case 128u * 1024u: return 15;
        default: return 0;
    }
}

static void ReadTitle(const uint8_t Header[kHeaderSize], char Title[17])
{
    const size_t Maximum = (Header[0x143] & 0x80) != 0 ? 15 : 16;
    size_t Length = 0;
    while (Length < Maximum)
    {
        const uint8_t Character = Header[0x134 + Length];
        if (Character < 0x20 || Character > 0x7e)
        {
            break;
        }
        Title[Length++] = (char)Character;
    }
    Title[Length] = '\0';
    if (Length == 0)
    {
        memcpy(Title, "UNTITLED", 9);
    }
}

static esp_err_t DecodeHeader(FILE *pRom, VirtualCartInfo_t *pInfo)
{
    uint8_t Header[kHeaderSize];
    if (fseek(pRom, 0, SEEK_SET) != 0 ||
        fread(Header, sizeof(Header), 1, pRom) != 1 ||
        memcmp(&Header[0x104], NintendoLogo, sizeof(NintendoLogo)) != 0)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t HeaderChecksum = 0;
    for (size_t i = 0x134; i <= 0x14c; ++i)
    {
        HeaderChecksum = (uint8_t)(HeaderChecksum - Header[i] - 1);
    }
    if (HeaderChecksum != Header[0x14d])
    {
        return ESP_ERR_INVALID_CRC;
    }
    if (!CartMapper_Decode(Header[0x147], &pInfo->Mapper))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(DecodeRomSize(Header[0x148], &pInfo->RomSize),
                        TAG, "unsupported ROM size code");
    ESP_RETURN_ON_ERROR(ResolveGeometry(&pInfo->Mapper, Header[0x148],
                                        Header[0x149], pInfo->RomSize,
                                        &pInfo->RamSize),
                        TAG, "unsupported mapper geometry");
    if (fseek(pRom, 0, SEEK_END) != 0 || ftell(pRom) != (long)pInfo->RomSize)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    pInfo->GlobalChecksum = ((uint16_t)Header[0x14e] << 8) | Header[0x14f];
    pInfo->MapperSelect = (uint8_t)pInfo->Mapper.Kind;
    pInfo->MappedRomSize = 32u * 1024u;
    while (pInfo->MappedRomSize < pInfo->RomSize) pInfo->MappedRomSize <<= 1;
    const uint16_t RomMask = (uint16_t)(pInfo->MappedRomSize / kRomBankSize - 1);
    pInfo->Configuration = pInfo->MapperSelect |
        ((uint16_t)pInfo->Mapper.Type << 3) | ((RomMask & 0x1f) << 11);
    pInfo->ConfigurationHigh = (uint8_t)((RomMask >> 5) & 0x0f) |
        (uint8_t)(RamMask(pInfo->RamSize) << 4);
    ReadTitle(Header, pInfo->Title);

    pInfo->Mbc1m = false;
    if (pInfo->Mapper.Kind == kCartMapper_Mbc1 &&
        pInfo->RomSize >= 0x40114u && fseek(pRom, 0x40104, SEEK_SET) == 0)
    {
        uint8_t Candidate[16];
        pInfo->Mbc1m = fread(Candidate, sizeof(Candidate), 1, pRom) == 1 &&
            memcmp(Candidate, NintendoLogo, sizeof(Candidate)) == 0;
    }
    return fseek(pRom, 0, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t VirtualCommandRaw(uint8_t Command, uint16_t Configuration,
                                   uint8_t ConfigurationHigh,
                                   CartLinkResponse_t *pResponse)
{
    const esp_err_t Result = CartLink_VirtualCommand(
        Command, Configuration, ConfigurationHigh, pResponse);
    if (Result != ESP_OK)
    {
        return Result;
    }
    return pResponse->Status == 0 && pResponse->Count == 4
        ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t VirtualCommand(uint8_t Command,
                                const VirtualCartInfo_t *pInfo,
                                CartLinkResponse_t *pResponse)
{
    return VirtualCommandRaw(
        Command, pInfo != NULL ? pInfo->Configuration : 0,
        pInfo != NULL ? pInfo->ConfigurationHigh : 0, pResponse);
}

static esp_err_t UploadRomPatched(FILE *pRom, const VirtualCartInfo_t *pInfo, const CheatPlan_t *Cheats)
{
    if (pInfo->GameGear) {
        /* Fill the entire bounded region, matching the simulator's mirroring.
         * Rewind only at ROM boundaries; never upload GB cheats or save bytes. */
        GGRomCrc=0;
        for (uint32_t offset=0;offset<kMaximumRomSize;offset+=sizeof(TransferBuffer)) {
            if (offset % pInfo->RomSize == 0 && fseek(pRom,0,SEEK_SET)) return ESP_FAIL;
            if (fread(TransferBuffer,1,sizeof(TransferBuffer),pRom)!=sizeof(TransferBuffer)) return ESP_FAIL;
            /* The ROM's CRC identifies the game in its save-state files. */
            if (offset<pInfo->RomSize) GGRomCrc=StateFile_Crc32(GGRomCrc,TransferBuffer,sizeof(TransferBuffer));
            ESP_RETURN_ON_ERROR(CartBulk_WritePSRAMBlock(kRomBase+offset,TransferBuffer), TAG,"GG ROM upload");
            RomBrowserUI_SetProgress(offset+sizeof(TransferBuffer),kMaximumRomSize);
        }
        return ESP_OK;
    }
    uint32_t Sum = 0;
    const uint32_t Total = pInfo->MappedRomSize + pInfo->RamSize;
    for (uint32_t Offset = 0; Offset < pInfo->MappedRomSize;
         Offset += sizeof(TransferBuffer))
    {
        if (Offset < pInfo->RomSize)
        {
            if (fread(TransferBuffer, sizeof(TransferBuffer), 1, pRom) != 1)
                return ESP_FAIL;
        }
        else
        {
            memset(TransferBuffer, 0xff, sizeof(TransferBuffer));
        }
        for (size_t i = 0; i < sizeof(TransferBuffer); ++i)
        {
            const uint32_t Address = Offset + i;
            if (Address < pInfo->RomSize && Address != 0x14e && Address != 0x14f)
            {
                Sum += TransferBuffer[i];
            }
        }
        if (Cheats && Offset < pInfo->RomSize)
            ESP_RETURN_ON_ERROR(Cheats_Apply(Cheats, Offset, TransferBuffer, sizeof(TransferBuffer)), TAG, "cheat original bytes");
        ESP_RETURN_ON_ERROR(CartBulk_WritePSRAMBlock(kRomBase + Offset,
                                                     TransferBuffer),
                            TAG, "ROM QSPI upload");
        RomBrowserUI_SetProgress(Offset + sizeof(TransferBuffer), Total);
        if ((Offset & ((512u * 1024u) - 1u)) == 0)
        {
            printf("VIRTUAL_CART_PROGRESS loaded=%lu total=%lu\n",
                   (unsigned long)(Offset + sizeof(TransferBuffer)),
                   (unsigned long)pInfo->MappedRomSize);
        }
    }
    /* The boot ROM checks the header checksum, not this optional whole-ROM
     * checksum. Prototypes, homebrew and rereleases often leave it stale.
     * CartBulk_WritePSRAMBlock still checks transfer completion, and all
     * read/transport errors remain fatal. This sum covers source bytes only. */
    if ((uint16_t)Sum != pInfo->GlobalChecksum)
    {
        /* This build disables ESP_LOG macros. Keep this diagnostic visible
         * on the console, like the other VIRTUAL_CART status messages. */
        printf("VIRTUAL_CART_CHECKSUM_WARNING stored=%04x actual=%04x\n",
               (unsigned)pInfo->GlobalChecksum, (unsigned)(uint16_t)Sum);
    }
    return ESP_OK;
}

#ifndef ESP_PLATFORM
static esp_err_t UploadRom(FILE *Rom, const VirtualCartInfo_t *Info)
{ return UploadRomPatched(Rom, Info, NULL); }
#endif

static esp_err_t MakeCompanionPath(const char *pRomPath,
                                   const char Extension[5],
                                   char Output[kPathSize])
{
    const int Length = snprintf(Output, kPathSize, "%s", pRomPath);
    if (Length < 0 || Length >= kPathSize)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char *pDot = strrchr(Output, '.');
    if (pDot == NULL || (size_t)(pDot - Output) + 5 > kPathSize)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(pDot, Extension, 5);
    return ESP_OK;
}

static esp_err_t MakeSavePath(const char *pRomPath, bool Lowercase,
                              char SavePath[kPathSize])
{
    return MakeCompanionPath(pRomPath, Lowercase ? ".sav" : ".SAV",
                             SavePath);
}

static esp_err_t MakeStatePath(const char *pRomPath, unsigned Slot,
                               char StatePath[kPathSize])
{
    static const char *const Extensions[kStateSlots] = {".state", ".state2", ".state3"};
    const int Length = snprintf(StatePath, kPathSize, "%s", pRomPath);
    char *pDot = Length > 0 && Length < kPathSize ? strrchr(StatePath, '.') : NULL;
    /* Leave room for the ".tmp"/".old" publication sidecars. */
    if (Slot < 1 || Slot > kStateSlots || pDot == NULL ||
        (size_t)(pDot - StatePath) + strlen(Extensions[Slot - 1]) + 1 + 4 > kPathSize)
    {
        StatePath[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(pDot, Extensions[Slot - 1]);
    return ESP_OK;
}

static esp_err_t MakeRtcPath(const char *pRomPath, bool Lowercase,
                             char RtcPath[kPathSize])
{
    return MakeCompanionPath(pRomPath, Lowercase ? ".rtc" : ".RTC",
                             RtcPath);
}

static esp_err_t MakeSidecarPath(const char *pPath, const char *pSuffix,
                                 char Output[kPathSize]);

static esp_err_t ReadRtcFile(const char *pPath, uint32_t *pState)
{
    FILE *pRtc = fopen(pPath, "rb");
    if (pRtc == NULL)
    {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    uint8_t Encoded[kRtcStateSize];
    esp_err_t Result = ESP_OK;
    if (fseek(pRtc, 0, SEEK_END) != 0 ||
        ftell(pRtc) != kRtcStateSize ||
        fseek(pRtc, 0, SEEK_SET) != 0 ||
        fread(Encoded, sizeof(Encoded), 1, pRtc) != 1)
    {
        Result = ESP_ERR_INVALID_SIZE;
    }
    if (fclose(pRtc) != 0 && Result == ESP_OK)
    {
        Result = ESP_FAIL;
    }
    if (Result != ESP_OK)
    {
        return Result;
    }

    const uint32_t State = (uint32_t)Encoded[0] |
        ((uint32_t)Encoded[1] << 8) |
        ((uint32_t)Encoded[2] << 16) |
        ((uint32_t)Encoded[3] << 24);
    if (!CartRtc_IsStateValid(State))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *pState = State;
    return ESP_OK;
}

static esp_err_t TryRtcFile(const char *pFinal, uint32_t *pState,
                            bool *pPresent, bool *pInvalid)
{
    *pPresent = false;
    *pInvalid = false;

    char Old[kPathSize];
    ESP_RETURN_ON_ERROR(MakeSidecarPath(pFinal, ".old", Old), TAG,
                        "RTC recovery path");

    const esp_err_t FinalResult = ReadRtcFile(pFinal, pState);
    if (FinalResult == ESP_OK)
    {
        *pPresent = true;
        if (unlink(Old) != 0 && errno != ENOENT)
        {
            printf("VIRTUAL_CART_RTC cleanup=deferred path=%s\n", Old);
        }
        return ESP_OK;
    }
    if (FinalResult != ESP_ERR_NOT_FOUND &&
        FinalResult != ESP_ERR_INVALID_SIZE &&
        FinalResult != ESP_ERR_INVALID_RESPONSE)
    {
        return FinalResult;
    }

    uint32_t OldState = 0;
    const esp_err_t OldResult = ReadRtcFile(Old, &OldState);
    if (OldResult == ESP_OK)
    {
        if (FinalResult != ESP_ERR_NOT_FOUND && unlink(pFinal) != 0)
        {
            return ESP_FAIL;
        }
        if (rename(Old, pFinal) != 0)
        {
            return ESP_FAIL;
        }
        *pState = OldState;
        *pPresent = true;
        printf("VIRTUAL_CART_RTC recovered=%s\n", pFinal);
        return ESP_OK;
    }
    if (OldResult != ESP_ERR_NOT_FOUND &&
        OldResult != ESP_ERR_INVALID_SIZE &&
        OldResult != ESP_ERR_INVALID_RESPONSE)
    {
        return OldResult;
    }

    *pInvalid = FinalResult != ESP_ERR_NOT_FOUND ||
                OldResult != ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

static esp_err_t MakeSidecarPath(const char *pPath, const char *pSuffix,
                                 char Output[kPathSize])
{
    const int Length = snprintf(Output, kPathSize, "%s%s", pPath, pSuffix);
    return Length >= 0 && Length < kPathSize
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t RecoverSavePublish(const char *pFinal)
{
    char Old[kPathSize];
    ESP_RETURN_ON_ERROR(MakeSidecarPath(pFinal, ".old", Old), TAG,
                        "save recovery path");

    struct stat Stat;
    if (stat(Old, &Stat) != 0)
    {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (stat(pFinal, &Stat) == 0)
    {
        return unlink(Old) == 0 ? ESP_OK : ESP_FAIL;
    }
    if (errno != ENOENT)
    {
        return ESP_FAIL;
    }
    return rename(Old, pFinal) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t FinishSaveFile(FILE **ppFile)
{
    if (ppFile == NULL || *ppFile == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *pFile = *ppFile;
    esp_err_t Result = ESP_OK;
    if (fflush(pFile) != 0 || fsync(fileno(pFile)) != 0)
    {
        Result = ESP_FAIL;
    }
    if (fclose(pFile) != 0)
    {
        Result = ESP_FAIL;
    }
    *ppFile = NULL;
    return Result;
}

static esp_err_t PublishSaveTemp(const char *pFinal, const char *pTemp,
                                 uint32_t ExpectedSize)
{
    char Old[kPathSize];
    ESP_RETURN_ON_ERROR(MakeSidecarPath(pFinal, ".old", Old), TAG,
                        "save old path");
    ESP_RETURN_ON_ERROR(RecoverSavePublish(pFinal), TAG,
                        "recover prior save publication");

    bool Staged = false;
    struct stat Stat;
    if (stat(pFinal, &Stat) == 0)
    {
        if (rename(pFinal, Old) != 0)
        {
            return ESP_FAIL;
        }
        Staged = true;
    }
    else if (errno != ENOENT)
    {
        return ESP_FAIL;
    }

    if (rename(pTemp, pFinal) != 0)
    {
        if (Staged) (void)rename(Old, pFinal);
        return ESP_FAIL;
    }
    if (stat(pFinal, &Stat) != 0 || Stat.st_size != (off_t)ExpectedSize)
    {
        (void)unlink(pFinal);
        if (Staged) (void)rename(Old, pFinal);
        return ESP_ERR_INVALID_SIZE;
    }
    if (Staged && unlink(Old) != 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t LoadRtcState(const char *pRomPath,
                              const VirtualCartInfo_t *pInfo,
                              char SelectedPath[kPathSize],
                              uint32_t *pState, bool *pPresent)
{
    if (SelectedPath == NULL || pState == NULL || pPresent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    SelectedPath[0] = '\0';
    *pState = 0;
    *pPresent = false;
    if (!pInfo->Mapper.HasRtc)
    {
        return ESP_OK;
    }

    char UpperPath[kPathSize];
    char LowerPath[kPathSize];
    ESP_RETURN_ON_ERROR(MakeRtcPath(pRomPath, false, UpperPath), TAG,
                        "RTC path");
    ESP_RETURN_ON_ERROR(MakeRtcPath(pRomPath, true, LowerPath), TAG,
                        "RTC path");

    bool UpperPresent = false;
    bool UpperInvalid = false;
    ESP_RETURN_ON_ERROR(TryRtcFile(UpperPath, pState, &UpperPresent,
                                   &UpperInvalid),
                        TAG, "read uppercase RTC");
    if (UpperPresent)
    {
        (void)snprintf(SelectedPath, kPathSize, "%s", UpperPath);
        *pPresent = true;
        printf("VIRTUAL_CART_RTC source=%s state=%08lx\n", UpperPath,
               (unsigned long)*pState);
        return ESP_OK;
    }

    bool LowerPresent = false;
    bool LowerInvalid = false;
    ESP_RETURN_ON_ERROR(TryRtcFile(LowerPath, pState, &LowerPresent,
                                   &LowerInvalid),
                        TAG, "read lowercase RTC");
    if (LowerPresent)
    {
        (void)snprintf(SelectedPath, kPathSize, "%s", LowerPath);
        *pPresent = true;
        printf("VIRTUAL_CART_RTC source=%s state=%08lx\n", LowerPath,
               (unsigned long)*pState);
        return ESP_OK;
    }

    const char *pSelected = UpperInvalid ? UpperPath :
                            LowerInvalid ? LowerPath : UpperPath;
    (void)snprintf(SelectedPath, kPathSize, "%s", pSelected);
    *pState = 0;
    if (UpperInvalid || LowerInvalid)
    {
        printf("VIRTUAL_CART_RTC source=invalid-reset path=%s state=00000000\n",
               pSelected);
    }
    else
    {
        printf("VIRTUAL_CART_RTC source=new state=00000000\n");
    }
    return ESP_OK;
}

static esp_err_t RestoreRtcState(const VirtualCartInfo_t *pInfo,
                                 uint32_t State, bool RtcSupported)
{
    if (!pInfo->Mapper.HasRtc || !RtcSupported)
    {
        return ESP_OK;
    }
    if (!CartRtc_IsStateValid(State))
    {
        return ESP_ERR_INVALID_ARG;
    }

    CartLinkResponse_t Response = {0};
    ESP_RETURN_ON_ERROR(
        VirtualCommandRaw(kVirtualCommandRtcRestoreLow,
                          (uint16_t)State, 0, &Response),
        TAG, "restore RTC low word");
    return VirtualCommandRaw(kVirtualCommandRtcRestoreHigh,
                             (uint16_t)(State >> 16), 0, &Response);
}

static esp_err_t QueryRtcState(uint32_t *pState)
{
    if (pState == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    CartLinkResponse_t Response = {0};
    ESP_RETURN_ON_ERROR(
        VirtualCommandRaw(kVirtualCommandRtcSnapshot, 0, 0, &Response),
        TAG, "snapshot RTC");
    const uint32_t State = (uint32_t)Response.Data[0] |
        ((uint32_t)Response.Data[1] << 8) |
        ((uint32_t)Response.Data[2] << 16) |
        ((uint32_t)Response.Data[3] << 24);
    if (!CartRtc_IsStateValid(State))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *pState = State;
    return ESP_OK;
}

static esp_err_t PersistActiveRtc(void)
{
    if (!Active || !ActiveInfo.Mapper.HasRtc || !ActiveRtcSupported ||
        ActiveRtcPath[0] == '\0')
    {
        return ESP_OK;
    }

    uint32_t State;
    ESP_RETURN_ON_ERROR(QueryRtcState(&State), TAG, "read active RTC");
    ActiveRtcLastCheck = xTaskGetTickCount();
    if (ActiveRtcPersistedValid && State == ActiveRtcPersistedState)
    {
        return ESP_OK;
    }

    char Temp[kPathSize];
    ESP_RETURN_ON_ERROR(MakeSidecarPath(ActiveRtcPath, ".tmp", Temp), TAG,
                        "RTC temporary path");
    ESP_RETURN_ON_ERROR(RecoverSavePublish(ActiveRtcPath), TAG,
                        "recover RTC publication");
    if (unlink(Temp) != 0 && errno != ENOENT)
    {
        return ESP_FAIL;
    }

    FILE *pRtc = fopen(Temp, "wb");
    if (pRtc == NULL)
    {
        return ESP_FAIL;
    }
    if (setvbuf(pRtc, NULL, _IONBF, 0) != 0)
    {
        (void)fclose(pRtc);
        (void)unlink(Temp);
        return ESP_FAIL;
    }
    const uint8_t Encoded[kRtcStateSize] = {
        (uint8_t)State,
        (uint8_t)(State >> 8),
        (uint8_t)(State >> 16),
        (uint8_t)(State >> 24),
    };
    esp_err_t Result = fwrite(Encoded, sizeof(Encoded), 1, pRtc) == 1
        ? ESP_OK : ESP_FAIL;
    if (Result == ESP_OK)
    {
        Result = FinishSaveFile(&pRtc);
    }
    else
    {
        (void)fclose(pRtc);
        pRtc = NULL;
    }
    if (Result == ESP_OK)
    {
        Result = PublishSaveTemp(ActiveRtcPath, Temp, kRtcStateSize);
    }
    if (Result != ESP_OK)
    {
        (void)unlink(Temp);
        return Result;
    }

    ActiveRtcPersistedState = State;
    ActiveRtcPersistedValid = true;
    printf("VIRTUAL_CART_RTC=PASS path=%s state=%08lx\n", ActiveRtcPath,
           (unsigned long)State);
    return ESP_OK;
}

static esp_err_t UploadSave(const char *pRomPath,
                            const VirtualCartInfo_t *pInfo,
                            char SelectedPath[kPathSize])
{
    SelectedPath[0] = '\0';
    if (pInfo->RamSize == 0)
    {
        return ESP_OK;
    }

    char SavePath[kPathSize] = {0};
    FILE *pSave = NULL;
    if (pInfo->Mapper.HasBattery)
    {
        ESP_RETURN_ON_ERROR(MakeSavePath(pRomPath, false, SavePath), TAG,
                            "save path");
        ESP_RETURN_ON_ERROR(RecoverSavePublish(SavePath), TAG,
                            "recover uppercase save");
        pSave = fopen(SavePath, "rb");
        if (pSave == NULL && errno == ENOENT)
        {
            ESP_RETURN_ON_ERROR(MakeSavePath(pRomPath, true, SavePath), TAG,
                                "save path");
            ESP_RETURN_ON_ERROR(RecoverSavePublish(SavePath), TAG,
                                "recover lowercase save");
            pSave = fopen(SavePath, "rb");
        }
    }

    if (pSave != NULL)
    {
        if (fseek(pSave, 0, SEEK_END) != 0 ||
            ftell(pSave) != (long)pInfo->RamSize ||
            fseek(pSave, 0, SEEK_SET) != 0)
        {
            fclose(pSave);
            return ESP_ERR_INVALID_SIZE;
        }
        printf("VIRTUAL_CART_SAVE source=%s bytes=%lu\n", SavePath,
               (unsigned long)pInfo->RamSize);
    }
    else if (pInfo->Mapper.HasBattery && errno != ENOENT)
    {
        return ESP_FAIL;
    }
    else if (pInfo->Mapper.HasBattery)
    {
        printf("VIRTUAL_CART_SAVE source=new bytes=%lu\n",
               (unsigned long)pInfo->RamSize);
    }
    else
    {
        printf("VIRTUAL_CART_SAVE source=volatile bytes=%lu\n",
               (unsigned long)pInfo->RamSize);
    }

    if (pInfo->Mapper.HasBattery)
    {
        (void)snprintf(SelectedPath, kPathSize, "%s", SavePath);
    }

    esp_err_t Result = ESP_OK;
    for (uint32_t Offset = 0; Offset < pInfo->RamSize;
         Offset += sizeof(TransferBuffer))
    {
        memset(TransferBuffer, 0xff, sizeof(TransferBuffer));
        if (pSave != NULL && Offset < pInfo->RamSize)
        {
            const size_t Remaining = pInfo->RamSize - Offset;
            const size_t Count = Remaining < sizeof(TransferBuffer)
                ? Remaining : sizeof(TransferBuffer);
            if (fread(TransferBuffer, Count, 1, pSave) != 1)
            {
                Result = ESP_FAIL;
                break;
            }
        }
        Result = CartBulk_WritePSRAMBlock(kSaveBase + Offset,
                                          TransferBuffer);
        if (Result != ESP_OK)
        {
            break;
        }
        const uint32_t SaveComplete =
            (Offset + sizeof(TransferBuffer) < pInfo->RamSize)
                ? Offset + sizeof(TransferBuffer) : pInfo->RamSize;
        RomBrowserUI_SetProgress(pInfo->MappedRomSize + SaveComplete,
                                 pInfo->MappedRomSize + pInfo->RamSize);
    }
    if (pSave != NULL && fclose(pSave) != 0 && Result == ESP_OK)
    {
        Result = ESP_FAIL;
    }
    return Result;
}

static esp_err_t QueryVirtualSaveDirty(bool *pDirty)
{
    if (pDirty == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    CartLinkResponse_t Response = {0};
    ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStatus, NULL,
                                       &Response),
                        TAG, "virtual save status");
    *pDirty = (Response.Data[0] & kVirtualStatusSavePending) != 0;
    return ESP_OK;
}

static esp_err_t PersistActiveSave(void)
{
    if (!Active || ActiveInfo.RamSize == 0 ||
        !ActiveInfo.Mapper.HasBattery || ActiveSavePath[0] == '\0')
    {
        ActiveSavePending = false;
        return ESP_OK;
    }

    bool Dirty = false;
    ESP_RETURN_ON_ERROR(QueryVirtualSaveDirty(&Dirty), TAG,
                        "check virtual save");
    ActiveSavePending |= Dirty;
    if (!ActiveSavePending)
    {
        return ESP_OK;
    }

    char Temp[kPathSize];
    ESP_RETURN_ON_ERROR(MakeSidecarPath(ActiveSavePath, ".tmp", Temp), TAG,
                        "save temporary path");
    ESP_RETURN_ON_ERROR(RecoverSavePublish(ActiveSavePath), TAG,
                        "recover save publication");
    if (unlink(Temp) != 0 && errno != ENOENT)
    {
        return ESP_FAIL;
    }

    FILE *pSave = fopen(Temp, "wb");
    if (pSave == NULL)
    {
        return ESP_FAIL;
    }
    if (setvbuf(pSave, NULL, _IONBF, 0) != 0)
    {
        (void)fclose(pSave);
        (void)unlink(Temp);
        return ESP_FAIL;
    }

    esp_err_t Result = ESP_OK;
    for (uint32_t Offset = 0; Offset < ActiveInfo.RamSize;
         Offset += sizeof(TransferBuffer))
    {
        ++SaveSequence;
        if (SaveSequence == 0) ++SaveSequence;

        CartLinkResponse_t Response = {0};
        const uint8_t Block = (uint8_t)(Offset / sizeof(TransferBuffer));
        Result = VirtualCommandRaw(kVirtualCommandSaveBlock, SaveSequence,
                                   Block, &Response);
        if (Result == ESP_OK)
        {
            Result = CartBulk_ReadVirtualBlock(SaveSequence,
                                               TransferBuffer);
        }
        if (Result != ESP_OK)
        {
            break;
        }

        if (ActiveInfo.Mapper.Kind == kCartMapper_Mbc2)
        {
            for (size_t i = 0; i < sizeof(TransferBuffer); ++i)
            {
                TransferBuffer[i] |= 0xf0;
            }
        }
        const size_t Remaining = ActiveInfo.RamSize - Offset;
        const size_t Count = Remaining < sizeof(TransferBuffer)
            ? Remaining : sizeof(TransferBuffer);
        if (fwrite(TransferBuffer, 1, Count, pSave) != Count)
        {
            Result = ESP_FAIL;
            break;
        }
    }

    if (Result == ESP_OK)
    {
        Result = QueryVirtualSaveDirty(&Dirty);
        if (Result == ESP_OK && Dirty)
        {
            Result = ESP_ERR_NOT_FINISHED;
        }
    }

    if (Result == ESP_OK)
    {
        Result = FinishSaveFile(&pSave);
    }
    else
    {
        (void)fclose(pSave);
        pSave = NULL;
    }
    if (Result == ESP_OK)
    {
        Result = PublishSaveTemp(ActiveSavePath, Temp,
                                 ActiveInfo.RamSize);
    }
    if (Result != ESP_OK)
    {
        (void)unlink(Temp);
        ActiveSavePending = true;
        return Result;
    }

    ActiveSavePending = false;
    printf("VIRTUAL_CART_SAVE=PASS path=%s bytes=%lu\n", ActiveSavePath,
           (unsigned long)ActiveInfo.RamSize);
    return ESP_OK;
}

static esp_err_t QuiesceActiveGame(void)
{
    CartLinkResponse_t Response = {0};
    ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandQuiesce, NULL,
                                       &Response), TAG, "virtual quiesce");
    for (unsigned Poll = 0; Poll < kVirtualPreparePolls; ++Poll)
    {
        ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStatus, NULL,
                                           &Response), TAG, "virtual quiesce status");
        const uint16_t Status = (uint16_t)Response.Data[0] |
                                ((uint16_t)Response.Data[1] << 8);
        if ((Status & (kVirtualStatusEnabled | kVirtualStatusQuiesced)) ==
                      (kVirtualStatusEnabled | kVirtualStatusQuiesced))
            return ESP_OK;
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

static void ResumeActiveGame(void)
{
    CartLinkResponse_t Response = {0};
    (void)VirtualCommand(kVirtualCommandResume, NULL, &Response);
}

static esp_err_t PersistActiveDataForTransition(void)
{
    if (!Active) return ESP_OK;
    esp_err_t Result = QuiesceActiveGame();
    if (Result != ESP_OK)
    {
        ResumeActiveGame();
        return Result;
    }
    for (unsigned Pass = 0; Pass < kForcedSavePasses; ++Pass)
    {
        Result = PersistActiveSave();
        if (Result != ESP_ERR_NOT_FINISHED)
        {
            break;
        }
        vTaskDelay(1);
    }
    if (Result == ESP_OK) Result = PersistActiveRtc();
    if (Result != ESP_OK) ResumeActiveGame();
    return Result;
}

static void ClearActiveSave(void)
{
    memset(&ActiveInfo, 0, sizeof(ActiveInfo));
    StateCanonicalPath[0] = 0;
    ActiveSavePath[0] = '\0';
    ActiveRtcPath[0] = '\0';
    memset(ActiveStatePath, 0, sizeof(ActiveStatePath));
    ActiveRomPath[0] = '\0';
    StateOnSdMask = 0;
    FpgaStateOwner = 0;
    ActiveSavePending = false;
    ActiveRtcPersistedState = 0;
    ActiveRtcPersistedValid = false;
    ActiveRtcSupported = false;
    ActiveRtcLastCheck = 0;
}

static esp_err_t PrepareAndWaitForReset(const VirtualCartInfo_t *pInfo,
                                        bool *pRtcSupported)
{
    CartLinkResponse_t Response = {0};
    ESP_RETURN_ON_ERROR(
        VirtualCommand(kVirtualCommandPrepare |
                       (pInfo->Mbc1m ? kVirtualStartMbc1m : 0),
                       pInfo, &Response),
        TAG, "virtual prepare");

    const uint16_t Required = kVirtualStatusDisableSeen |
                              kVirtualStatusResetAsserted;
    for (unsigned Poll = 0; Poll < kVirtualPreparePolls; ++Poll)
    {
        ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStatus, NULL,
                                           &Response),
                            TAG, "virtual prepare status");
        const uint16_t Status = (uint16_t)Response.Data[0] |
                                ((uint16_t)Response.Data[1] << 8);
        if ((Status & Required) == Required &&
            (Status & kVirtualStatusEnabled) == 0 &&
            (!pInfo->GameGear || (Response.Data[3]==0x47 && Response.Data[2]==1 && !(Status & 0x4000))))
        {
            if (pRtcSupported != NULL)
            {
                *pRtcSupported =
                    (Response.Data[1] & kVirtualCapabilityRtcState) != 0;
            }
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t StartAndWaitForFrame(const VirtualCartInfo_t *pInfo,
                                      uint32_t RtcState,
                                      bool *pRtcSupported)
{
    if (pInfo->GameGear) {
        CartLinkResponse_t response={0};
        ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStart,pInfo,&response),TAG,"GG start");
        for (unsigned poll=0;poll<kVirtualBootPolls;poll++) {
            ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStatus,NULL,&response),TAG,"GG status");
            uint16_t flags=(uint16_t)response.Data[0] | ((uint16_t)response.Data[1]<<8);
            if (response.Data[3]!=0x47 || response.Data[2]!=1) return ESP_ERR_NOT_SUPPORTED;
            /* Bit 13 means reset/fault while enabled; bit 14 is a fresh frame.
             * Reset is normal during start propagation. Never accept stale ready. */
            if ((flags & 0x603b)==0x403b) return ESP_OK;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return ESP_ERR_TIMEOUT;
    }
    const uint16_t Required = kVirtualStatusEnabled |
                              kVirtualStatusInitialized |
                              kVirtualStatusDisableSeen |
                              kVirtualStatusResetAsserted |
                              kVirtualStatusResetReleased |
                              kVirtualStatusBootRomHigh |
                              kVirtualStatusBootRomExited |
                              kVirtualStatusPostExitFrame;
    for (unsigned Attempt = 0; Attempt < kVirtualBootAttempts; ++Attempt)
    {
        CartLinkResponse_t Response = {0};
        uint16_t PreviousStatus = UINT16_MAX;
        ESP_RETURN_ON_ERROR(RestoreRtcState(pInfo, RtcState,
                                            *pRtcSupported), TAG,
                            "restore RTC before start");
        ESP_RETURN_ON_ERROR(
            VirtualCommand(kVirtualCommandStart |
                           (pInfo->Mbc1m ? kVirtualStartMbc1m : 0),
                           pInfo, &Response),
            TAG, "virtual start");
        for (unsigned Poll = 0; Poll < kVirtualBootPolls; ++Poll)
        {
            ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStatus, NULL,
                                               &Response),
                                TAG, "virtual status");
            const uint16_t Status = (uint16_t)Response.Data[0] |
                                    ((uint16_t)Response.Data[1] << 8);
            if (Status != PreviousStatus)
            {
                printf("VIRTUAL_CART_LIFECYCLE attempt=%u flags=0x%03x "
                       "enabled=%u initialized=%u disable=%u "
                       "reset_assert=%u reset_release=%u boot_high=%u "
                       "boot_exit=%u post_frame=%u\n",
                       Attempt + 1, Status,
                       !!(Status & kVirtualStatusEnabled),
                       !!(Status & kVirtualStatusInitialized),
                       !!(Status & kVirtualStatusDisableSeen),
                       !!(Status & kVirtualStatusResetAsserted),
                       !!(Status & kVirtualStatusResetReleased),
                       !!(Status & kVirtualStatusBootRomHigh),
                       !!(Status & kVirtualStatusBootRomExited),
                       !!(Status & kVirtualStatusPostExitFrame));
                PreviousStatus = Status;
            }
            if (Response.Count == 4 && (Status & Required) == Required)
            {
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        printf("VIRTUAL_CART_ATTEMPT_TIMEOUT attempt=%u flags=0x%03x\n",
               Attempt + 1, PreviousStatus);
        if (Attempt + 1 < kVirtualBootAttempts)
        {
            printf("VIRTUAL_CART_RETRY attempt=%u\n", Attempt + 2);
            ESP_RETURN_ON_ERROR(PrepareAndWaitForReset(pInfo,
                                                       pRtcSupported), TAG,
                                "virtual retry prepare");
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t RunLoad(const char *pPath)
{
    esp_err_t Result = ESP_FAIL;
    sdmmc_card_t *pCard = NULL;
    FILE *pRom = NULL;
    bool Mounted = false;
    bool BulkOpen = false;
    bool VirtualDisturbed = false;
    bool SwitchRequested = false;
    VirtualCartInfo_t Info = {0};
    char SelectedSavePath[kPathSize] = {0};
    char SelectedRtcPath[kPathSize] = {0};
    uint32_t RtcState = 0;
    bool RtcPresent = false;
    bool RtcSupported = false;
    CheatPlan_t Cheats = {0};

    Result = SDCard_Mount(&pCard);
    if (Result != ESP_OK) goto Cleanup;
    Mounted = true;

    pRom = fopen(pPath, "rb");
    if (pRom == NULL)
    {
        Result = ESP_ERR_NOT_FOUND;
        goto Cleanup;
    }
    const char *extension=strrchr(pPath,'.');
    Info.GameGear=extension && !strcasecmp(extension,".gg");
    if (Info.GameGear) {
        Result=GGRom_Inspect(pRom,&Info.RomSize);
        if(Result==ESP_OK) Result=Cheats_Prepare(pPath,pRom,&Cheats);
        Info.MappedRomSize=kMaximumRomSize;
        snprintf(Info.Title,sizeof(Info.Title),"GAME GEAR");
    } else {
        Result = DecodeHeader(pRom, &Info);
        if (Result == ESP_OK) Result = Cheats_Prepare(pPath, pRom, &Cheats);
    }
    if (Result != ESP_OK) goto Cleanup;

    printf("VIRTUAL_CART_START path=%s title=\"%s\" mapper=\"%s\" rom=%lu save=%lu mbc1m=%u\n",
           pPath, Info.Title, CartMapper_TypeName(Info.Mapper.Type),
           (unsigned long)Info.RomSize, (unsigned long)Info.RamSize,
           Info.Mbc1m ? 1u : 0u);

    Result = CartBulk_Begin();
    if (Result != ESP_OK) goto Cleanup;
    BulkOpen = true;
    /* Reject mismatched images before preparing/resetting or writing memory.
     * A legacy GB FPGA has zero in the high status byte. */
    CartLinkResponse_t identity={0};
    Result=VirtualCommand(kVirtualCommandStatus,NULL,&identity);
    if (Result!=ESP_OK) goto Cleanup;
    bool ggCore=identity.Data[3]==0x47 && identity.Data[2]==1;
    if ((Info.GameGear && !ggCore) || (!Info.GameGear && identity.Data[3]!=0)) {
        if (!CoreSwitch_IsSupported() || CoreSwitch_IsTarget(Info.GameGear)) {
            Result=ESP_ERR_NOT_SUPPORTED;
            goto Cleanup;
        }
        if (Active) {
            Result = PersistActiveDataForTransition();
            if (Result != ESP_OK) goto Cleanup;
        }
        CartLinkResponse_t Stopped = {0};
        Result = VirtualCommand(kVirtualCommandStop, NULL, &Stopped);
        if (Result != ESP_OK) goto Cleanup;
        VirtualDisturbed = true;
        SetActive(false);
        ClearActiveSave();
        SwitchRequested = true;
        goto Cleanup;
    }

    if (Active)
    {
        Result = PersistActiveDataForTransition();
        if (Result != ESP_OK) goto Cleanup;
    }

    VirtualDisturbed = true;
    Result = PrepareAndWaitForReset(&Info, &RtcSupported);
    if (Result != ESP_OK) goto Cleanup;
    SetActive(false);
    ClearActiveSave();

    Result = UploadRomPatched(pRom, &Info, &Cheats);
    if (Info.GameGear) Info.GlobalChecksum = (uint16_t)(GGRomCrc ^ (GGRomCrc >> 16));
    if (Result != ESP_OK) goto Cleanup;
    if (fclose(pRom) != 0)
    {
        pRom = NULL;
        Result = ESP_FAIL;
        goto Cleanup;
    }
    pRom = NULL;

    if (Info.GameGear) {
        if (CoreSwitch_IsTarget(true)) Result=GGCheats_Install(&Cheats);
        else if(Cheats.Count) Result=ESP_ERR_NOT_SUPPORTED;
        if(Result!=ESP_OK) goto Cleanup;
    }
    Result = UploadSave(pPath, &Info, SelectedSavePath);
    if (Result != ESP_OK) goto Cleanup;

    Result = LoadRtcState(pPath, &Info, SelectedRtcPath, &RtcState,
                          &RtcPresent);
    if (Result != ESP_OK) goto Cleanup;

    if (Info.Mapper.HasRtc && !RtcSupported)
    {
        printf("VIRTUAL_CART_RTC=UNSUPPORTED reason=fpga-capability\n");
    }
    Result = StartAndWaitForFrame(&Info, RtcState, &RtcSupported);
    if (Result != ESP_OK) goto Cleanup;

    ActiveInfo = Info;
    (void)snprintf(ActiveRomPath, sizeof(ActiveRomPath), "%s", pPath);
    (void)snprintf(ActiveSavePath, sizeof(ActiveSavePath), "%s",
                   SelectedSavePath);
    StateOnSdMask = 0;
    FpgaStateOwner = 0;
    for (unsigned Slot = 1; Slot <= kStateSlots; ++Slot)
    {
        char *pState = ActiveStatePath[Slot - 1];
        struct stat StateStat;
        if (MakeStatePath(pPath, Slot, pState) == ESP_OK &&
            stat(pState, &StateStat) == 0 && S_ISREG(StateStat.st_mode))
        {
            StateOnSdMask |= 1u << (Slot - 1);
        }
    }
    (void)snprintf(ActiveRtcPath, sizeof(ActiveRtcPath), "%s",
                   SelectedRtcPath);
    ActiveSavePending = false;
    ActiveRtcPersistedState = RtcState;
    ActiveRtcPersistedValid = RtcPresent;
    ActiveRtcSupported = RtcSupported;
    ActiveRtcLastCheck = xTaskGetTickCount();
    SetActive(true);

Cleanup:
    Cheats_Free(&Cheats);
    if (pRom != NULL) (void)fclose(pRom);
    if (BulkOpen) CartBulk_End();
    if (Mounted)
    {
        const esp_err_t UnmountResult = SDCard_Unmount(pCard);
        if (Result == ESP_OK && UnmountResult != ESP_OK)
        {
            Result = UnmountResult;
        }
    }
    if (SwitchRequested && Result == ESP_OK)
        Result = CoreSwitch_Request(pPath);
    /* Completion clears the browser's error tracking. Publish it only after
     * cleanup succeeds, so an unmount failure is still reported to the UI. */
    if (Result == ESP_OK)
    {
        printf("VIRTUAL_CART=PASS title=\"%s\"\n", Info.Title);
        const esp_err_t RememberResult = LastGame_Remember(pPath);
        if (RememberResult != ESP_OK)
            printf("LAST_GAME remember_error=%s\n", esp_err_to_name(RememberResult));
        const esp_err_t RecentResult = RecentGames_Remember(pPath);
        if (RecentResult != ESP_OK)
            printf("RECENT_GAMES remember_error=%s\n", esp_err_to_name(RecentResult));
        if (RomBrowserUI_SetComplete())
        {
            const esp_err_t CloseResult = RomBrowser_CloseMenu();
            printf("ROM_BROWSER_CLOSE=%s error=%s\n",
                   CloseResult == ESP_OK ? "PASS" : "FAIL",
                   esp_err_to_name(CloseResult));
        }
    }
    if (Result != ESP_OK)
    {
        if (VirtualDisturbed)
        {
            CartLinkResponse_t Response = {0};
            (void)VirtualCommand(kVirtualCommandStop, NULL, &Response);
            SetActive(false);
            ClearActiveSave();
        }
        printf("VIRTUAL_CART=FAIL error=%s\n", esp_err_to_name(Result));
        RomBrowserUI_SetError(Result);
    }
    return Result;
}

static void LoadTask(void *pArg)
{
    (void)pArg;
    PwrMgr_InhibitSleep();
    while (!BeginRequestedLoad()) vTaskDelay(1);
    (void)RunLoad(PendingPath);
    PwrMgr_AllowSleep();
    (void)SetRunning(false);
    (void)SetLoadRequested(false);
    vTaskDelete(NULL);
}

esp_err_t VirtualCart_Start(const char *pPath)
{
    if (PCBackup_IsEnabled() || CartBackup_IsPCModeActive())
    {
        return ESP_ERR_INVALID_STATE;
    }
    char Resolved[kPathSize];
    ESP_RETURN_ON_ERROR(ResolvePath(pPath, Resolved), TAG, "ROM path");
    if (!SetLoadRequested(true))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (PCBackup_IsEnabled() || CartBackup_IsPCModeActive())
    {
        (void)SetLoadRequested(false);
        return ESP_ERR_INVALID_STATE;
    }
    (void)snprintf(PendingPath, sizeof(PendingPath), "%s", Resolved);
    if (xTaskCreate(LoadTask, "virtual_cart", kLoadTaskStack, NULL,
                    kLoadTaskPriority, NULL) != pdPASS)
    {
        (void)SetLoadRequested(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t VirtualCart_Stop(void)
{
    if (!SetRunning(true)) return ESP_ERR_INVALID_STATE;

    esp_err_t Result = ESP_OK;
    sdmmc_card_t *pCard = NULL;
    bool Mounted = false;
    bool BulkOpen = false;
    bool Quiesced = false;
    CartLinkResponse_t Response = {0};
    const bool HasBatteryRam = Active && ActiveInfo.RamSize != 0 &&
        ActiveInfo.Mapper.HasBattery && ActiveSavePath[0] != '\0';
    const bool HasRtc = Active && ActiveInfo.Mapper.HasRtc &&
        ActiveRtcSupported &&
        ActiveRtcPath[0] != '\0';
    if (HasBatteryRam || HasRtc)
    {
        Result = SDCard_Mount(&pCard);
        if (Result != ESP_OK) goto Cleanup;
        Mounted = true;
        Result = CartBulk_Begin();
        if (Result != ESP_OK) goto Cleanup;
        BulkOpen = true;
    }
    Result = PersistActiveDataForTransition();
    if (Result != ESP_OK) goto Cleanup;
    Quiesced = Active;

    Result = VirtualCommand(kVirtualCommandStop, NULL, &Response);
    if (Result == ESP_OK)
    {
        SetActive(false);
        ClearActiveSave();
    }

Cleanup:
    if (Result != ESP_OK && Quiesced && Active) ResumeActiveGame();
    if (BulkOpen) CartBulk_End();
    if (Mounted)
    {
        const esp_err_t UnmountResult = SDCard_Unmount(pCard);
        if (Result == ESP_OK && UnmountResult != ESP_OK)
        {
            Result = UnmountResult;
        }
    }
    (void)SetRunning(false);
    return Result;
}

static void SaveTask(void *pArg)
{
    (void)pArg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(kSavePollPeriod_ms));
        if (!VirtualCart_IsActive() || !BeginAutosave())
        {
            continue;
        }

        esp_err_t Result = ESP_OK;
        sdmmc_card_t *pCard = NULL;
        bool Mounted = false;
        bool BulkOpen = false;
        const bool HasBatteryRam = ActiveInfo.RamSize != 0 &&
            ActiveInfo.Mapper.HasBattery && ActiveSavePath[0] != '\0';
        const bool HasRtc = ActiveInfo.Mapper.HasRtc && ActiveRtcSupported &&
            ActiveRtcPath[0] != '\0';
        const TickType_t Now = xTaskGetTickCount();
        const bool RtcDue = HasRtc &&
            (Now - ActiveRtcLastCheck >=
             pdMS_TO_TICKS(kRtcPersistPeriod_ms));
        if (RtcDue)
        {
            ActiveRtcLastCheck = Now;
        }
        if (HasBatteryRam)
        {
            bool Dirty = false;
            Result = QueryVirtualSaveDirty(&Dirty);
            if (Result == ESP_OK)
            {
                ActiveSavePending |= Dirty;
            }
        }
        if (Result == ESP_OK && (ActiveSavePending || RtcDue))
        {
            Result = SDCard_Mount(&pCard);
            Mounted = Result == ESP_OK;
        }
        if (Result == ESP_OK && Mounted && ActiveSavePending)
        {
            Result = CartBulk_Begin();
            BulkOpen = Result == ESP_OK;
        }
        if (Result == ESP_OK && ActiveSavePending)
        {
            Result = PersistActiveSave();
        }
        if (Result == ESP_OK && Mounted && HasRtc)
        {
            Result = PersistActiveRtc();
        }

        if (BulkOpen) CartBulk_End();
        if (Mounted)
        {
            const esp_err_t UnmountResult = SDCard_Unmount(pCard);
            if (Result == ESP_OK && UnmountResult != ESP_OK)
            {
                Result = UnmountResult;
            }
        }
        if (Result != ESP_OK && Result != ESP_ERR_NOT_FINISHED)
        {
            printf("VIRTUAL_CART_SAVE=DEFERRED error=%s\n",
                   esp_err_to_name(Result));
        }
        (void)SetRunning(false);
    }
}


static esp_err_t ReadNativeStatus(uint32_t *status)
{
    CartLinkResponse_t response={0};
    ESP_RETURN_ON_ERROR(VirtualCommand(12,NULL,&response),TAG,"native status");
    *status=(uint32_t)response.Data[0] | (uint32_t)response.Data[1]<<8 |
            (uint32_t)response.Data[2]<<16 | (uint32_t)response.Data[3]<<24;
    return (*status>>16)==0x5353 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void PrintNativeDebug(const char *phase)
{
    CartLinkResponse_t response={0};
    const esp_err_t err=VirtualCommand(13,NULL,&response);
    const uint32_t data=(uint32_t)response.Data[0] | (uint32_t)response.Data[1]<<8 |
        (uint32_t)response.Data[2]<<16 | (uint32_t)response.Data[3]<<24;
    if(err!=ESP_OK || (data>>16)!=0x5344) {
        printf("STATE_DEBUG phase=%s unavailable error=%s\n",phase,esp_err_to_name(err));
        return;
    }
    unsigned frame=(data>>8)&15;
    frame^=frame>>1;frame^=frame>>2;
    printf("STATE_DEBUG phase=%s frame=%u maintenance=%u reset=%u sleep=%u pause=%u quiesced=%u cart_wait=%u halt=%u lcd=%u\n",
        phase,frame,(unsigned)(data>>7)&1,(unsigned)(data>>6)&1,
        (unsigned)(data>>5)&1,(unsigned)(data>>4)&1,(unsigned)(data>>3)&1,
        (unsigned)(data>>2)&1,(unsigned)(data>>1)&1,(unsigned)data&1);
}

/* Games whose mapper the native state engine captures and restores. */
static bool NativeMapperSupported(const VirtualCartInfo_t *pInfo)
{
    /* Game Gear: the core's status reports whether its engine is present. */
    if (pInfo->GameGear) return pInfo->RomSize != 0;
    switch (pInfo->Mapper.Kind)
    {
    case kCartMapper_Mbc1:
    case kCartMapper_Mbc5:
        return !pInfo->Mapper.HasRtc;
    case kCartMapper_Mbc3:
        /* MBC30 (64 KiB RAM, Japanese Crystal) is untested; the FPGA refuses it. */
        return !pInfo->Mapper.IsMbc30 && pInfo->RamSize <= 32u * 1024u;
    default:
        return false;
    }
}

static void ActiveStateGame(StateFileGame_t *pGame)
{
    memset(pGame, 0, sizeof(*pGame));
    memcpy(pGame->Title, ActiveInfo.Title, sizeof(pGame->Title));
    pGame->RomSize = ActiveInfo.RomSize;
    pGame->RamSize = ActiveInfo.RamSize;
    pGame->GlobalChecksum = ActiveInfo.GlobalChecksum;
    pGame->CartType = ActiveInfo.Mapper.Type;
    if (ActiveInfo.GameGear)
    {
        pGame->RamSize = 0;
        pGame->CartType = 0;
        pGame->Engine = kStateEngine_GameGear;
    }
}

static esp_err_t WaitNativeIdle(uint32_t *pStatus)
{
    for (unsigned Poll = 0; Poll < 200; ++Poll)
    {
        ESP_RETURN_ON_ERROR(ReadNativeStatus(pStatus), TAG, "native status");
        if (!(*pStatus & 1)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_TIMEOUT;
}

/* Game paused, SD mounted, bulk session open, FPGA slot valid. Copies the slot
 * to "<rom>.state" through a temporary file and the .old publication scheme. */
static esp_err_t ExportStateToSd(unsigned Slot)
{
    const char *const pPath = ActiveStatePath[Slot - 1];
    if (!pPath[0]) return ESP_ERR_INVALID_STATE;
    StateFileGame_t Game;
    ActiveStateGame(&Game);
    const uint32_t Length = StateFile_GamePayloadLength(&Game);
    char Temp[kPathSize];
    ESP_RETURN_ON_ERROR(MakeSidecarPath(pPath, ".tmp", Temp), TAG,
                        "state temporary path");
    if (unlink(Temp) != 0 && errno != ENOENT) return ESP_FAIL;
    FILE *pFile = fopen(Temp, "wb");
    if (pFile == NULL) return ESP_FAIL;

    uint8_t Header[kStateFileHeaderSize] = {0};
    esp_err_t Result = fwrite(Header, 1, sizeof(Header), pFile) == sizeof(Header)
        ? ESP_OK : ESP_FAIL;
    uint32_t Crc = 0;
    for (uint32_t Offset = 0; Result == ESP_OK && Offset < Length;
         Offset += kCartBulkBlockSize)
    {
        if (++SaveSequence == 0) ++SaveSequence;
        CartLinkResponse_t Response = {0};
        Result = VirtualCommandRaw(kVirtualCommandStateBlock, SaveSequence,
                                   (uint8_t)(Offset / kCartBulkBlockSize), &Response);
        if (Result == ESP_OK)
            Result = CartBulk_ReadVirtualBlock(SaveSequence, TransferBuffer);
        if (Result != ESP_OK) break;
        if (Offset == 0 &&
            !StateFile_GamePayloadSignatureOk(&Game, TransferBuffer, sizeof(TransferBuffer)))
        {
            Result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        const size_t Count = Length - Offset < kCartBulkBlockSize
            ? Length - Offset : kCartBulkBlockSize;
        Crc = StateFile_Crc32(Crc, TransferBuffer, Count);
        if (fwrite(TransferBuffer, 1, Count, pFile) != Count) Result = ESP_FAIL;
    }
    if (Result == ESP_OK)
    {
        StateFile_EncodeHeader(&Game, Length, Crc, Header);
        if (fseek(pFile, 0, SEEK_SET) != 0 ||
            fwrite(Header, 1, sizeof(Header), pFile) != sizeof(Header))
            Result = ESP_FAIL;
    }
    if (Result == ESP_OK) Result = FinishSaveFile(&pFile);
    else (void)fclose(pFile);
    if (Result == ESP_OK)
        Result = PublishSaveTemp(pPath, Temp, kStateFileHeaderSize + Length);
    if (Result != ESP_OK)
    {
        (void)unlink(Temp);
        printf("STATE_FILE_SAVE=FAIL path=%s error=%s\n", pPath,
               esp_err_to_name(Result));
        return Result;
    }
    printf("STATE_FILE_SAVE=PASS path=%s bytes=%lu crc=%08lx\n", pPath,
           (unsigned long)Length, (unsigned long)Crc);
    return ESP_OK;
}

/* Game paused, SD mounted, bulk session open, FPGA slot not valid (after power-on
 * or a game change). Validates "<rom>.state", writes it into the FPGA slot and
 * marks the slot valid. The engine checks its own header again on load. */
static esp_err_t ImportStateFromSd(unsigned Slot)
{
    const char *const pPath = ActiveStatePath[Slot - 1];
    if (!pPath[0]) return ESP_ERR_NOT_FOUND;
    ESP_RETURN_ON_ERROR(RecoverSavePublish(pPath), TAG,
                        "recover state publication");
    FILE *pFile = fopen(pPath, "rb");
    if (pFile == NULL) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;

    StateFileGame_t Game;
    ActiveStateGame(&Game);
    uint8_t Header[kStateFileHeaderSize];
    uint32_t Length = 0, Expected = 0, Crc = 0;
    esp_err_t Result = ESP_OK;
    if (fread(Header, 1, sizeof(Header), pFile) != sizeof(Header))
    {
        Result = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        const StateFileResult_t Check =
            StateFile_DecodeHeader(Header, &Game, &Length, &Expected);
        if (Check != kStateFile_Ok)
        {
            printf("STATE_FILE_REJECT=%s path=%s\n", StateFile_ResultName(Check),
                   pPath);
            Result = ESP_ERR_INVALID_VERSION;
        }
    }
    for (uint32_t Offset = 0; Result == ESP_OK && Offset < Length;
         Offset += kCartBulkBlockSize)
    {
        const size_t Count = Length - Offset < kCartBulkBlockSize
            ? Length - Offset : kCartBulkBlockSize;
        if (fread(TransferBuffer, 1, Count, pFile) != Count)
        {
            Result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (Count < kCartBulkBlockSize)
            memset(TransferBuffer + Count, 0, kCartBulkBlockSize - Count);
        if (Offset == 0 &&
            !StateFile_GamePayloadSignatureOk(&Game, TransferBuffer, sizeof(TransferBuffer)))
        {
            Result = ESP_ERR_INVALID_VERSION;
            break;
        }
        Crc = StateFile_Crc32(Crc, TransferBuffer, Count);
        const uint32_t Base = Game.Engine == kStateEngine_GameGear
            ? kRomBase + kGGStateRomAddress : kStateSlotBase;
        Result = CartBulk_WritePSRAMBlock(Base + Offset, TransferBuffer);
    }
    if (Result == ESP_OK && fgetc(pFile) != EOF) Result = ESP_ERR_INVALID_SIZE;
    if (fclose(pFile) != 0 && Result == ESP_OK) Result = ESP_FAIL;
    if (Result == ESP_OK && Crc != Expected)
    {
        printf("STATE_FILE_REJECT=bad_crc path=%s\n", pPath);
        Result = ESP_ERR_INVALID_CRC;
    }
    if (Result != ESP_OK) return Result;
    /* Game Gear: staged and verified; the caller asks the FPGA to load it. */
    if (Game.Engine == kStateEngine_GameGear) return ESP_OK;

    CartLinkResponse_t Response = {0};
    ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStateImport, NULL, &Response),
                        TAG, "state import");
    uint32_t Status = 0;
    ESP_RETURN_ON_ERROR(WaitNativeIdle(&Status), TAG, "state import status");
    if ((Status & 6) != 2) return ESP_ERR_INVALID_RESPONSE;
    printf("STATE_FILE_LOAD=PASS path=%s bytes=%lu\n", pPath,
           (unsigned long)Length);
    return ESP_OK;
}

bool VirtualCart_LastStateSdFailed(void) { return LastStateSdFailed; }

esp_err_t VirtualCart_NativeInfo(bool *Supported,bool *Valid,bool *Uncertain,uint32_t *Session)
{
    uint8_t mask=0;
    const esp_err_t result=VirtualCart_NativeSlotInfo(Supported,&mask,Uncertain,Session);
    *Valid=(mask&1)!=0;
    return result;
}
esp_err_t VirtualCart_NativeSlotInfo(bool *Supported,uint8_t *ValidMask,bool *Uncertain,uint32_t *Session)
{
    *Supported=false;*ValidMask=0;*Session=0;
    portENTER_CRITICAL(&StateLock);*Uncertain=StateUncertain;portEXIT_CRITICAL(&StateLock);
    if(!SetRunning(true))return ESP_ERR_NOT_FINISHED;
    *Session=NativeSession;
    *Supported=Active && NativeMapperSupported(&ActiveInfo);
    uint32_t status=0;esp_err_t result=ESP_OK;
    if(*Supported) {
        result=ReadNativeStatus(&status);
        // A core without a state engine refuses the status command.
        if(result==ESP_ERR_INVALID_RESPONSE || result==ESP_ERR_NOT_SUPPORTED) { result=ESP_OK;status=0; }
        *Supported=result==ESP_OK && (status&8)!=0;
        /* Valid in the FPGA slot, or loadable from the SD card copy. */
        if(*Supported) {
            if((status&7)==2 && FpgaStateOwner) *ValidMask|=1u<<(FpgaStateOwner-1);
            if(!(status&5)) *ValidMask|=StateOnSdMask;
        }
    }
    (void)SetRunning(false);return result;
}

// Completion of snapshot transfer does not prove the external pause released.
static esp_err_t ResumeNativeGameChecked(void)
{
    CartLinkResponse_t response={0};
    ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandResume,NULL,&response),TAG,"native resume");
    for(unsigned poll=0;poll<kVirtualPreparePolls;++poll) {
        ESP_RETURN_ON_ERROR(VirtualCommand(kVirtualCommandStatus,NULL,&response),TAG,"native resume status");
        const uint16_t status=(uint16_t)response.Data[0] | (uint16_t)response.Data[1]<<8;
        if(!(status & kVirtualStatusEnabled)) return ESP_ERR_INVALID_RESPONSE;
        if(!(status & kVirtualStatusQuiesced)) return ESP_OK;
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

// Local prototype: one volatile PSRAM slot. All control and persistence serialized.
esp_err_t VirtualCart_StateCapture(bool load)
{
    return VirtualCart_StateCaptureSlot(load,1,0);
}
esp_err_t VirtualCart_StateCaptureForSession(bool load,uint32_t Session)
{
    return VirtualCart_StateCaptureSlot(load,1,Session);
}
/* Game Gear: poll the engine status until (status & Mask) == Want. While
 * waiting for a load, the fault bit ends the wait early. */
static esp_err_t WaitGGStatus(uint32_t Want, uint32_t Mask, bool FaultEnds,
                              unsigned Timeout_ms, uint32_t *pStatus)
{
    const TickType_t Started = xTaskGetTickCount();
    for (;;)
    {
        ESP_RETURN_ON_ERROR(ReadNativeStatus(pStatus), TAG, "GG state status");
        if ((*pStatus & Mask) == Want) return ESP_OK;
        if (FaultEnds && (*pStatus & kGGStatusFault)) return ESP_FAIL;
        if ((TickType_t)(xTaskGetTickCount() - Started) >= pdMS_TO_TICKS(Timeout_ms))
            return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* The payload was staged over the ROM mirror at kGGStateRomAddress; put the
 * mirror back so a game selecting those banks reads its own ROM again. */
static esp_err_t RestoreGGRomMirror(void)
{
    if (!ActiveRomPath[0] || ActiveInfo.RomSize == 0) return ESP_ERR_INVALID_STATE;
    FILE *pRom = fopen(ActiveRomPath, "rb");
    if (pRom == NULL) return ESP_FAIL;
    esp_err_t Result = ESP_OK;
    for (uint32_t Offset = kGGStateRomAddress;
         Result == ESP_OK && Offset < kGGStateRomAddress + kStateFileGameGearPayload;
         Offset += kCartBulkBlockSize)
    {
        if (fseek(pRom, (long)(Offset % ActiveInfo.RomSize), SEEK_SET) != 0 ||
            fread(TransferBuffer, 1, kCartBulkBlockSize, pRom) != kCartBulkBlockSize)
            Result = ESP_FAIL;
        else
            Result = CartBulk_WritePSRAMBlock(kRomBase + Offset, TransferBuffer);
    }
    (void)fclose(pRom);
    return Result;
}

/* Game Gear save/load. The FPGA keeps no copy: SAVE freezes the game, writes
 * the frozen state straight to "<rom>.state<n>" and releases it; LOAD freezes,
 * stages the verified file in PSRAM, lets the FPGA restore it and releases.
 * A file that fails its checks is refused before anything in the game changes. */
static esp_err_t GGStateCapture(bool load, unsigned slot)
{
    const uint8_t bit = 1u << (slot - 1);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool mounted = false, bulk = false, frozen = false, submitted = false;
    sdmmc_card_t *card = NULL;
    uint32_t status = 0;
    CartLinkResponse_t response = {0};
    result = ReadNativeStatus(&status);
    if (result != ESP_OK || !(status & 8))
    {
        printf("STATE_REJECT=gg_engine_missing\n");
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    if (status & (1 | kGGStatusFrozen) || (load && !(StateOnSdMask & bit)))
    {
        printf("STATE_REJECT=snapshot_status value=%08lx\n", (unsigned long)status);
        result = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    result = SDCard_Mount(&card); if (result != ESP_OK) goto cleanup; mounted = true;
    result = CartBulk_Begin(); if (result != ESP_OK) goto cleanup; bulk = true;
    result = VirtualCommand(kVirtualCommandGGFreeze, NULL, &response);
    if (result != ESP_OK) goto cleanup;
    frozen = true;
    result = WaitGGStatus(kGGStatusFrozen, kGGStatusFrozen | 1, false, 1000, &status);
    if (result != ESP_OK) { printf("STATE_REJECT=gg_freeze_timeout\n"); goto cleanup; }
    if (!load)
    {
        result = ExportStateToSd(slot);
        if (result == ESP_OK) StateOnSdMask |= bit;
        goto cleanup;
    }
    result = ImportStateFromSd(slot);
    if (result != ESP_OK) goto cleanup;
    // From here a failure may leave a partly restored game: keep it frozen.
    submitted = true; StateUncertain = true;
    result = VirtualCommand(kVirtualCommandGGLoad, NULL, &response);
    if (result == ESP_OK)
        result = WaitGGStatus(kGGStatusLoaded, kGGStatusLoaded | 1, true, 3000, &status);
    if (result != ESP_OK) { printf("STATE_REJECT=gg_load status=%08lx\n", (unsigned long)status); goto cleanup; }
    StateUncertain = false;
    if (RestoreGGRomMirror() != ESP_OK) printf("STATE_GG_MIRROR=FAIL\n");
    printf("STATE_FILE_LOAD=PASS path=%s bytes=%u\n", ActiveStatePath[slot - 1],
           (unsigned)kStateFileGameGearPayload);
cleanup:
    if (frozen && (!submitted || !StateUncertain))
    {
        esp_err_t release = VirtualCommand(kVirtualCommandGGRelease, NULL, &response);
        if (release == ESP_OK)
            release = WaitGGStatus(0, kGGStatusFrozen, false, 1000, &status);
        if (release != ESP_OK)
        {
            printf("STATE_REJECT=gg_release error=%s\n", esp_err_to_name(release));
            StateUncertain = true;
            if (result == ESP_OK) result = release;
        }
    }
    if (bulk) CartBulk_End();
    if (mounted)
    {
        const esp_err_t unmount = SDCard_Unmount(card);
        if (result == ESP_OK) result = unmount;
    }
    (void)SetRunning(false);
    return result;
}

esp_err_t VirtualCart_StateCaptureSlot(bool load,unsigned slot,uint32_t Session)
{
    if(slot<1 || slot>kStateSlots) return ESP_ERR_INVALID_ARG;
    const uint8_t bit=1u<<(slot-1);
    const esp_err_t acquire = BeginNativeOperation();
    if (acquire != ESP_OK) return acquire;
    if(Session && Session!=NativeSession) {
        (void)SetRunning(false);
        return ESP_ERR_INVALID_STATE;
    }
    if(Active && ActiveInfo.GameGear) {
        LastStateSdFailed=false; // no FPGA copy: a save is on the SD card or fails
        return GGStateCapture(load,slot);
    }
    esp_err_t result=ESP_ERR_INVALID_STATE;
    bool mounted=false,bulk=false,paused=false,submitted=false;
    sdmmc_card_t *card=NULL;
    uint32_t status=0;
    CartLinkResponse_t response={0};
    if(!Active || !NativeMapperSupported(&ActiveInfo)) {
        printf("STATE_REJECT=unsupported_or_inactive\n");goto cleanup;
    }
    result=ReadNativeStatus(&status);
    if(result!=ESP_OK) goto cleanup;
    // The FPGA slot is used directly only when it holds this very slot.
    const bool from_sd=load && !((status&2) && FpgaStateOwner==slot);
    if((status&13)!=8 || (from_sd && !(StateOnSdMask&bit))) {
        printf("STATE_REJECT=snapshot_status value=%08lx\n", (unsigned long)status);
        result=ESP_ERR_INVALID_STATE;goto cleanup;
    }
    const bool battery=ActiveInfo.Mapper.HasBattery && ActiveInfo.RamSize;
    // SD is needed for the state file even without battery RAM.
    result=SDCard_Mount(&card);if(result!=ESP_OK) goto cleanup;mounted=true;
    result=CartBulk_Begin();if(result!=ESP_OK) goto cleanup;bulk=true;
    result=PersistActiveDataForTransition();if(result!=ESP_OK) goto cleanup;
    paused=true;
    if(from_sd) {
        // Nothing is submitted yet: a bad file just resumes the game unchanged.
        // A failed import may leave the FPGA slot half overwritten.
        FpgaStateOwner=0;
        result=ImportStateFromSd(slot);if(result!=ESP_OK) goto cleanup;
        FpgaStateOwner=slot;
    }
    if(load && battery) {
        // Force a complete current battery save even when its dirty flag is clear.
        ActiveSavePending=true;
        result=PersistActiveSave();if(result!=ESP_OK) goto cleanup;
        char branch[kPathSize];
        const char *canonical=StateCanonicalPath[0] ? StateCanonicalPath : ActiveSavePath;
        if(StateSaveBranch_Create(canonical,ActiveSavePath,ActiveInfo.RamSize,branch,sizeof(branch))) {
            result=ESP_FAIL;goto cleanup;
        }
        if(!StateCanonicalPath[0]) strcpy(StateCanonicalPath,ActiveSavePath);
        strcpy(ActiveSavePath,branch); // Must precede the first RAM-changing command.
        ActiveSavePending=true;
        printf("STATE_BATTERY_BRANCH=%s\n",ActiveSavePath);
    }
    // Any lost acknowledgement may conceal a partial restore. Keep paused/isolated.
    StateUncertain=true;submitted=true;
    if(!load) FpgaStateOwner=0; // A capture replaces the FPGA slot.
    result=VirtualCommand(load?11:10,NULL,&response);
    if(result!=ESP_OK) goto cleanup;
    for(unsigned poll=0;poll<6000;++poll) {
        result=ReadNativeStatus(&status);if(result!=ESP_OK) goto cleanup;
        if(status&4) { result=ESP_FAIL;goto cleanup; }
        if(!(status&1)) {
            if(!(status&2)) { result=ESP_ERR_INVALID_RESPONSE;goto cleanup; }
            StateUncertain=false;
            result=ESP_OK;
            if(!load) {
                // The native state is valid even if the SD copy fails.
                FpgaStateOwner=slot;
                LastStateSdFailed=ExportStateToSd(slot)!=ESP_OK;
                if(!LastStateSdFailed) StateOnSdMask|=bit;
            }
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    result=ESP_ERR_TIMEOUT;
cleanup:
    if(paused && (!submitted || !StateUncertain)) {
        PrintNativeDebug("transfer-complete");
        const esp_err_t resume=ResumeNativeGameChecked();
        PrintNativeDebug("resume-command");
        if(resume!=ESP_OK) {
            StateUncertain=true;
            result=resume;
        }
    }
    if(bulk) CartBulk_End();
    if(mounted) {
        esp_err_t unmount=SDCard_Unmount(card);
        if(result==ESP_OK) result=unmount;
    }
    (void)SetRunning(false);
    return result;
}

static unsigned ConsoleSlot(int argc,char **argv)
{
    return argc>1 ? (unsigned)strtoul(argv[1],NULL,10) : 1;
}
static int NativeSaveCommand(int argc,char **argv)
{
    esp_err_t result=VirtualCart_StateCaptureSlot(false,ConsoleSlot(argc,argv),0);
    printf("STATE_SAVE=%s\n",esp_err_to_name(result));return result!=ESP_OK;
}
static int NativeLoadCommand(int argc,char **argv)
{
    esp_err_t result=VirtualCart_StateCaptureSlot(true,ConsoleSlot(argc,argv),0);
    printf("STATE_LOAD=%s\n",esp_err_to_name(result));return result!=ESP_OK;
}
static int NativeStatusCommand(int argc,char **argv)
{
    (void)argc;(void)argv;uint32_t status=0;
    esp_err_t result=ReadNativeStatus(&status);
    printf("STATE_STATUS=%08lx uncertain=%u error=%s\n",(unsigned long)status,StateUncertain,esp_err_to_name(result));
    PrintNativeDebug("status");
    return result!=ESP_OK;
}
static int LoadCommand(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: cartload <file.gb|file.gbc|file.gg>\n");
        return 1;
    }
    const esp_err_t Result = VirtualCart_Start(argv[1]);
    printf("VIRTUAL_CART_LAUNCH=%s error=%s\n",
           Result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(Result));
    return Result == ESP_OK ? 0 : 1;
}

static int StopCommand(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_err_t Result = VirtualCart_Stop();
    printf("VIRTUAL_CART_STOP=%s error=%s\n",
           Result == ESP_OK ? "PASS" : "FAIL", esp_err_to_name(Result));
    return Result == ESP_OK ? 0 : 1;
}

esp_err_t VirtualCart_RegisterConsoleCommands(void)
{
    const esp_console_cmd_t Load = {
        .command = "cartload",
        .help = "Load a supported GB/GBC backup from /sdcard/CHROMAGIC/BACKUPS",
        .hint = "<file.gb|file.gbc|file.gg>",
        .func = LoadCommand,
        .argtable = NULL,
    };
    const esp_console_cmd_t Stop = {
        .command = "cartstop",
        .help = "Stop the active SD-backed virtual cartridge",
        .hint = NULL,
        .func = StopCommand,
        .argtable = NULL,
    };
    const esp_console_cmd_t NativeCommands[]={
      {.command="statesave",.help="Save native state to slot 1-3 (default 1)",.hint="[slot]",.func=NativeSaveCommand},
      {.command="stateload",.help="Load native state from slot 1-3 (default 1) into a separate battery-save branch",.hint="[slot]",.func=NativeLoadCommand},
      {.command="statestatus",.help="Report native snapshot capability and status",.func=NativeStatusCommand}
    };
    for(unsigned i=0;i<3;++i) {
      esp_err_t err=esp_console_cmd_register(&NativeCommands[i]);if(err!=ESP_OK) return err;
    }
    esp_err_t Result = esp_console_cmd_register(&Load);
    if (Result == ESP_OK) Result = esp_console_cmd_register(&Stop);
    if (Result != ESP_OK) return Result;
    return xTaskCreate(SaveTask, "virtual_save", kSaveTaskStack, NULL,
                       kSaveTaskPriority, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
