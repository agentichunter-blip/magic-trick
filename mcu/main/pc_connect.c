/* Device side of the web ChroMagician's "connect" protocol (ChroMagic 2),
 * on top of the same cartridge and SD code as the 1.x PC commands (pcbackup,
 * pcimport, pcsd), which stay for the desktop ChroMagician.
 *
 * Every command prints CONNECT records, then "CONNECT PASS" or
 * "CONNECT FAIL error=.. detail=<hex>", then "> ok" or "> err". Binary goes
 * straight to the UART after a DATA record, and comes from the host after a
 * SEND record. See chromagic-2-web/PROTOCOL.md in the workspace. */
#include "pc_connect.h"

#include "cart_backup.h"
#include "cart_mapper.h"
#include "pc_backup.h"
#include "pc_connect_proto.h"
#include "pc_sd.h"
#include "player_num.h"

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_CHROMAGIC_WIFI_FILES
#include "wifi_credentials.h"
#endif

#include <stdio.h>
#include <string.h>

enum {
    kLineSize = 2 * 0x50 + 256,
    kPathSize = 768,
    kSdRefresh_us = 10 * 1000 * 1000,
};

static const char *TAG = "PCConnect";
static char Line[kLineSize];
static char PathA[kPathSize], PathB[kPathSize];
static char Version[48];

/* SD presence for STATUS. Mounting takes a moment, so STATUS reports the
 * last known state and the check runs after the command has answered. */
static bool SdKnown, SdPresent;
static int64_t SdCheckedAt;

static void Emit(size_t Length)
{
    if (Length) fputs(Line, stdout);
}

static void Pass(void)
{
    printf("CONNECT PASS\n> ok\n");
    fflush(stdout);
}

static void Fail(esp_err_t Error, const char *pDetail)
{
    Emit(ConnectProto_FormatFail(Line, sizeof(Line), esp_err_to_name(Error), pDetail));
    printf("> err\n");
    fflush(stdout);
}

static int Finish(esp_err_t Result, const char *pDetail)
{
    if (Result == ESP_OK) Pass();
    else
    {
        if (pDetail == NULL && Result == ESP_ERR_INVALID_STATE && !PCBackup_IsUSBModeOn())
            pDetail = CONNECT_DETAIL_OFF;
        Fail(Result, pDetail);
    }
    return 0;
}

static const char *VersionString(void)
{
    if (Version[0] == '\0')
    {
        const esp_app_desc_t *pDesc = esp_app_get_description();
        snprintf(Version, sizeof(Version), "MagicTrick-%s", pDesc->version);
        for (char *p = Version; *p; ++p) if (*p == ' ') *p = '-';
    }
    return Version;
}

static void RefreshSd(bool Force)
{
    if (!PCBackup_IsUSBModeOn()) return;
    const int64_t Now = esp_timer_get_time();
    if (!Force && SdKnown && Now - SdCheckedAt < kSdRefresh_us) return;
    bool Present;
    if (PCSD_CardPresent(&Present))
    {
        SdPresent = Present;
        SdKnown = true;
    }
    SdCheckedAt = Now;
}

static void NoteSdResult(esp_err_t Result, const char *pDetail)
{
    if (pDetail != NULL && strcmp(pDetail, CONNECT_DETAIL_NO_SD) == 0)
    { SdPresent = false; SdKnown = true; }
    else if (Result == ESP_OK) { SdPresent = true; SdKnown = true; }
    SdCheckedAt = esp_timer_get_time();
}

/* ---- cartridge ---------------------------------------------------------- */

static CartBackupHeaderInfo_t Header;
static const char *CartDetail;
static bool NoCart;
static char CartDetailText[48];

static void ToConnectInfo(const CartBackupHeaderInfo_t *pIn,
                          const CartBackupMetadata_t *pMetadata,
                          ConnectCartInfo_t *pOut)
{
    memset(pOut, 0, sizeof(*pOut));
    memcpy(pOut->Title, pMetadata->Title, sizeof(pOut->Title) - 1);
    pOut->Type = pMetadata->Type;
    pOut->Color = pMetadata->IsColor;
    pOut->RomSize = pMetadata->RomSize;
    pOut->SaveSize = pMetadata->SaveSize;
    pOut->Rtc = pMetadata->HasRtc;
    memcpy(pOut->Header, pIn->Header, sizeof(pOut->Header));
}

static void PrintInfo(const CartBackupMetadata_t *pMetadata)
{
    ConnectCartInfo_t Info;
    ToConnectInfo(&Header, pMetadata, &Info);
    Emit(ConnectProto_FormatInfo(Line, sizeof(Line), &Info));
    fflush(stdout);
}

/* Reads the header into Header and sets CartDetail for the page. */
static esp_err_t ReadHeader(void)
{
    CartDetail = NULL;
    const esp_err_t Result = CartBackup_ReadHeaderInfo(&Header);
    NoCart = Result == ESP_ERR_INVALID_RESPONSE || Result == ESP_ERR_INVALID_CRC;
    if (NoCart)
        CartDetail = CONNECT_DETAIL_NO_CART;
    else if (Result == ESP_ERR_NOT_SUPPORTED)
    {
        const uint8_t Type = Header.Header[0x47];
        const char *pName = CartMapper_TypeName(Type);
        snprintf(CartDetailText, sizeof(CartDetailText), "TYPE %02X %s", Type,
                 pName != NULL ? pName : "Unknown");
        CartDetail = CartDetailText;
    }
    return Result;
}

static esp_err_t InfoNow(void *pContext)
{
    (void)pContext;
    const esp_err_t Result = ReadHeader();
    /* An unsupported mapper still shows; its backup fails with TYPE xx. */
    if (Result == ESP_OK || Result == ESP_ERR_NOT_SUPPORTED)
    {
        PrintInfo(&Header.Metadata);
        return ESP_OK;
    }
    if (NoCart)
    {
        Emit(ConnectProto_FormatNoCart(Line, sizeof(Line)));
        return ESP_OK;
    }
    return Result;
}

static int Info(void)
{
    if (PCBackup_IsUSBModeOn() && PCBackup_CartridgePresent() == 0)
    {
        Emit(ConnectProto_FormatNoCart(Line, sizeof(Line)));
        return Finish(ESP_OK, NULL);
    }
    const esp_err_t Result = PCBackup_RunGated(kPCBackupGate_Cartridge, InfoNow, NULL);
    return Finish(Result, Result == ESP_OK ? NULL : CartDetail);
}

typedef struct {
    bool Open;
    uint32_t Sequence, Size, Crc;
} BackupStream_t;

static const char *KindName(CartBackupArtifact_t Artifact)
{
    return Artifact == kCartBackupArtifact_Rom ? "rom"
         : Artifact == kCartBackupArtifact_Save ? "sav" : "rtc";
}

static esp_err_t StreamInfo(void *pContext, const CartBackupMetadata_t *pMetadata)
{
    (void)pContext;
    PrintInfo(pMetadata);
    return ESP_OK;
}

static esp_err_t StreamBegin(void *pContext, CartBackupArtifact_t Artifact, uint32_t Size)
{
    BackupStream_t *const p = pContext;
    if (p->Open) return ESP_ERR_INVALID_STATE;
    *p = (BackupStream_t){ .Open = true };
    printf("CONNECT BEGIN kind=%s size=%lu\n", KindName(Artifact), (unsigned long)Size);
    fflush(stdout);
    return ESP_OK;
}

static esp_err_t StreamData(void *pContext, CartBackupArtifact_t Artifact,
                            const uint8_t *pData, size_t Size)
{
    BackupStream_t *const p = pContext;
    if (!p->Open || pData == NULL || Size == 0) return ESP_ERR_INVALID_STATE;
    printf("CONNECT DATA kind=%s seq=%lu size=%u crc=%08lx\n", KindName(Artifact),
           (unsigned long)p->Sequence++, (unsigned)Size,
           (unsigned long)esp_rom_crc32_le(0, pData, Size));
    ESP_RETURN_ON_ERROR(PCBackup_WriteRaw(pData, Size), TAG, "send block");
    p->Crc = esp_rom_crc32_le(p->Crc, pData, Size);
    p->Size += Size;
    return ESP_OK;
}

static esp_err_t StreamEnd(void *pContext, CartBackupArtifact_t Artifact)
{
    BackupStream_t *const p = pContext;
    if (!p->Open) return ESP_ERR_INVALID_STATE;
    printf("CONNECT END kind=%s size=%lu crc=%08lx\n", KindName(Artifact),
           (unsigned long)p->Size, (unsigned long)p->Crc);
    fflush(stdout);
    p->Open = false;
    return ESP_OK;
}

static esp_err_t BackupNow(void *pContext)
{
    const uint32_t Selection = *(const uint32_t *)pContext;
    esp_err_t Result = ReadHeader();
    if (Result != ESP_OK) return Result;
    BackupStream_t Stream = {0};
    const CartBackupStreamSink_t Sink = {
        .OnInfo = StreamInfo, .OnBegin = StreamBegin,
        .OnData = StreamData, .OnEnd = StreamEnd,
    };
    /* The SD variant reads into the MCU without mirroring to the FPGA's own
     * USB endpoint, which has no reader here. */
    Result = CartBackup_StreamToSD(Selection, &Sink, &Stream);
    if (Result == ESP_ERR_NOT_FOUND) CartDetail = CONNECT_DETAIL_NO_SAVE;
    return Result;
}

static int Backup(int argc, char **argv)
{
    uint32_t Selection = 0;
    if (argc == 3 && strcmp(argv[2], "rom") == 0) Selection = kCartBackupSelectRom;
    else if (argc == 3 && strcmp(argv[2], "sav") == 0) Selection = kCartBackupSelectSave;
    else return Finish(ESP_ERR_INVALID_ARG, NULL);
    CartDetail = NULL;
    const esp_err_t Result = PCBackup_RunGated(kPCBackupGate_Cartridge, BackupNow, &Selection);
    return Finish(Result, Result == ESP_OK ? NULL : CartDetail);
}

typedef struct {
    CartSaveImportPhase_t Phase;
    uint32_t SaveRemaining, SaveOffset;
    bool RtcPending;
    uint32_t Size, Crc, RtcCrc;
    bool HasRtc;
} Restore_t;

static esp_err_t RestoreInfo(void *pContext, const CartBackupMetadata_t *pMetadata)
{
    (void)pContext;
    PrintInfo(pMetadata);
    return ESP_OK;
}

static esp_err_t RestoreReady(void *pContext, CartSaveImportPhase_t Phase,
                              uint32_t SaveSize, bool HasRtc)
{
    Restore_t *const p = pContext;
    p->Phase = Phase;
    p->SaveRemaining = SaveSize;
    p->SaveOffset = 0;
    p->RtcPending = HasRtc;
    PCBackup_FlushInput();
    printf("CONNECT READY phase=%s sav=%lu rtc=%u\n",
           Phase == kCartSaveImportPhase_Validate ? "validate" : "write",
           (unsigned long)SaveSize, HasRtc ? 1u : 0u);
    fflush(stdout);
    return ESP_OK;
}

static esp_err_t RestoreRead(void *pContext, uint8_t *pData, size_t Size)
{
    Restore_t *const p = pContext;
    const bool IsSave = p->SaveRemaining != 0;
    if (pData == NULL || Size == 0 || (IsSave && Size > p->SaveRemaining) ||
        (!IsSave && (!p->RtcPending || Size != 4)))
        return ESP_ERR_INVALID_STATE;
    const esp_err_t Result = PCBackup_ReceiveSends(IsSave ? "sav" : "rtc",
                                                   IsSave ? p->SaveOffset : 0, pData, Size);
    if (Result == ESP_ERR_TIMEOUT) CartDetail = CONNECT_DETAIL_UPLOAD_TIMEOUT;
    ESP_RETURN_ON_ERROR(Result, TAG, "receive restore block");
    if (IsSave) { p->SaveRemaining -= Size; p->SaveOffset += Size; }
    else p->RtcPending = false;
    return ESP_OK;
}

static void RestoreProgress(void *pContext, uint32_t Written, uint32_t Total)
{
    (void)pContext;
    printf("CONNECT PROGRESS written=%lu total=%lu\n", (unsigned long)Written,
           (unsigned long)Total);
    fflush(stdout);
}

static esp_err_t RestoreNow(void *pContext)
{
    Restore_t *const p = pContext;
    esp_err_t Result = ReadHeader();
    if (Result != ESP_OK) return Result;
    const CartSaveImportSource_t Source = {
        .OnInfo = RestoreInfo, .OnReady = RestoreReady,
        .Read = RestoreRead, .OnProgress = RestoreProgress,
    };
    Result = CartBackup_ImportSaveFromPC(p->Size, p->Crc, p->HasRtc, p->RtcCrc,
                                         &Source, p);
    if (Result == ESP_ERR_INVALID_SIZE && CartDetail == NULL)
        CartDetail = p->HasRtc && !Header.Metadata.HasRtc ? CONNECT_DETAIL_NO_RTC
                                                          : CONNECT_DETAIL_SAVE_SIZE;
    return Result;
}

static int Restore(int argc, char **argv)
{
    Restore_t Context = {0};
    const char *pRtc = ConnectProto_Field(argc, argv, 2, "rtccrc");
    if (!ConnectProto_ParseU32(ConnectProto_Field(argc, argv, 2, "size"), 10, &Context.Size) ||
        !ConnectProto_ParseU32(ConnectProto_Field(argc, argv, 2, "crc"), 16, &Context.Crc) ||
        (pRtc != NULL && !ConnectProto_ParseU32(pRtc, 16, &Context.RtcCrc)))
        return Finish(ESP_ERR_INVALID_ARG, NULL);
    Context.HasRtc = pRtc != NULL;
    CartDetail = NULL;
    const esp_err_t Result = PCBackup_RunGated(kPCBackupGate_Cartridge, RestoreNow, &Context);
    PCBackup_FlushInput();
    return Finish(Result, Result == ESP_OK ? NULL : CartDetail);
}

static esp_err_t SdBackupNow(void *pContext)
{
    const bool SaveOnly = *(const bool *)pContext;
    esp_err_t Result = ReadHeader();
    if (Result != ESP_OK) return Result;
    char Stem[kCartBackupFileStemSize];
    CartBackup_MakeFileStem(Header.Metadata.Title, Stem);
    const char *pDetail = NULL;
    Result = PCSD_ConnectBackup(SaveOnly, Stem, Header.Metadata.IsColor, &pDetail);
    NoteSdResult(Result, pDetail);
    if (pDetail != NULL) CartDetail = pDetail;
    return Result;
}

static int SdBackup(int argc, char **argv)
{
    bool SaveOnly;
    if (argc >= 3 && strcmp(argv[2], "rom") == 0) SaveOnly = false;
    else if (argc >= 3 && strcmp(argv[2], "sav") == 0) SaveOnly = true;
    else return Finish(ESP_ERR_INVALID_ARG, NULL);
    CartDetail = NULL;
    const esp_err_t Result = PCBackup_RunGated(kPCBackupGate_Cartridge, SdBackupNow, &SaveOnly);
    return Finish(Result, Result == ESP_OK ? NULL : CartDetail);
}

/* ---- SD files ----------------------------------------------------------- */

typedef struct {
    int Op;
    uint32_t Size, Crc;
    const char *pDetail;
} SdOp_t;

enum { kSdList, kSdGet, kSdPut, kSdMkdir, kSdRemove, kSdMove };

static esp_err_t SdNow(void *pContext)
{
    SdOp_t *const p = pContext;
    esp_err_t Result;
    switch (p->Op)
    {
        case kSdList: Result = PCSD_ConnectList(PathA, &p->pDetail); break;
        case kSdGet: Result = PCSD_ConnectGet(PathA, &p->pDetail); break;
        case kSdPut: Result = PCSD_ConnectPut(PathA, p->Size, p->Crc, &p->pDetail); break;
        case kSdMkdir: Result = PCSD_ConnectMkdir(PathA, &p->pDetail); break;
        case kSdRemove: Result = PCSD_ConnectRemove(PathA, &p->pDetail); break;
        default: Result = PCSD_ConnectMove(PathA, PathB, &p->pDetail); break;
    }
    NoteSdResult(Result, p->pDetail);
    return Result;
}

static int SdCommand(int Op, int argc, char **argv)
{
    SdOp_t Context = { .Op = Op };
    const int Paths = Op == kSdMove ? 2 : 1;
    if (argc < 2 + Paths || !ConnectProto_HexText(argv[2], PathA, sizeof(PathA)) ||
        (Paths == 2 && !ConnectProto_HexText(argv[3], PathB, sizeof(PathB))))
        return Finish(ESP_ERR_INVALID_ARG, NULL);
    if (Op == kSdPut &&
        (!ConnectProto_ParseU32(ConnectProto_Field(argc, argv, 3, "size"), 10, &Context.Size) ||
         !ConnectProto_ParseU32(ConnectProto_Field(argc, argv, 3, "crc"), 16, &Context.Crc)))
        return Finish(ESP_ERR_INVALID_ARG, NULL);
    const esp_err_t Result = PCBackup_RunGated(kPCBackupGate_Storage, SdNow, &Context);
    if (Op == kSdPut) PCBackup_FlushInput();
    return Finish(Result, Result == ESP_OK ? NULL : Context.pDetail);
}

/* ---- status and dispatch ------------------------------------------------ */

static void PrintStatus(void);

/* Called by fwversion before its JSON (components/osd/status/fw.c). */
void Firmware_ReportConnectStatus(void)
{
    PrintStatus();
}

static int Status(void)
{
    PrintStatus();
    Pass();
    RefreshSd(false);
    return 0;
}

static void PrintStatus(void)
{
    const bool Enabled = PCBackup_IsUSBModeOn();
    if (!Enabled) SdKnown = false;
    const ConnectStatus_t Status = {
        .Enabled = Enabled,
        .Active = Enabled && CartBackup_IsPCModeActive(),
        .SD = Enabled && SdKnown && SdPresent,
        .Player = PlayerNum_GetNum(),
        .Version = VersionString(),
    };
    Emit(ConnectProto_FormatStatus(Line, sizeof(Line), &Status));
    fflush(stdout);
}

static int ConnectCommand(int argc, char **argv)
{
    const char *pSub = argc >= 2 ? argv[1] : "";
    if (strcmp(pSub, "status") == 0) return Status();
    if (strcmp(pSub, "info") == 0) return Info();
    if (strcmp(pSub, "backup") == 0) return Backup(argc, argv);
    if (strcmp(pSub, "restore") == 0) return Restore(argc, argv);
    if (strcmp(pSub, "sdbackup") == 0) return SdBackup(argc, argv);
    if (strcmp(pSub, "ls") == 0) return SdCommand(kSdList, argc, argv);
    if (strcmp(pSub, "get") == 0) return SdCommand(kSdGet, argc, argv);
    if (strcmp(pSub, "put") == 0) return SdCommand(kSdPut, argc, argv);
    if (strcmp(pSub, "mkdir") == 0) return SdCommand(kSdMkdir, argc, argv);
    if (strcmp(pSub, "rm") == 0) return SdCommand(kSdRemove, argc, argv);
    if (strcmp(pSub, "mv") == 0) return SdCommand(kSdMove, argc, argv);
    if (strcmp(pSub, "flashid") == 0)
    {
        /* Homebrew writing is not offered yet; the page hides it. */
        printf("CONNECT FLASH supported=0\n");
        return Finish(ESP_OK, NULL);
    }
    if (strcmp(pSub, "writerom") == 0)
        return Finish(ESP_ERR_NOT_SUPPORTED, "Writing homebrew is not available in Magic Trick yet");
    /* Nothing touches the card while USB TRANSFERS is on. */
    if (strcmp(pSub, "sdhold") == 0) return Finish(ESP_OK, NULL);
    /* "resync" lands here on purpose: the page expects UNKNOWN COMMAND. */
    return Finish(ESP_ERR_NOT_SUPPORTED, CONNECT_DETAIL_UNKNOWN);
}

/* Console commands the page sends outside "connect". Replies are "> " lines
 * ending in "> ok" or "> err". */
/* A failure is one line "> error=..." (or "> err"): the page's terminator
 * pattern /^> (ok|err)/ already matches it, so nothing may follow it. */
static int Reply(bool Ok, const char *pText)
{
    if (pText != NULL && (Ok || strncmp(pText, "error=", 6) != 0))
        printf("> %s\n", pText);
    if (Ok) printf("> ok\n");
    else printf("> %s\n", pText != NULL && strncmp(pText, "error=", 6) == 0 ? pText : "err");
    fflush(stdout);
    return 0;
}

static int WifiCommand(int argc, char **argv)
{
#ifdef CONFIG_CHROMAGIC_WIFI_FILES
    if (argc == 1)
    {
        WifiCredentials Saved = {0};
        const bool Have = WifiCredentials_Read(&Saved) == ESP_OK && WifiCredentials_Valid(&Saved);
        /* Magic Trick turns Wi-Fi on only on its Wi-Fi transfer screen. */
        printf("> wifi setting=off state=off ssid=%s ip=- rssi=0\n", Have ? Saved.SSID : "-");
        return Reply(true, NULL);
    }
    if (argc == 4 && strcmp(argv[1], "setx") == 0)
    {
        WifiCredentials New = {0};
        if (!ConnectProto_HexText(argv[2], New.SSID, sizeof(New.SSID)) ||
            !ConnectProto_HexText(argv[3], New.Password, sizeof(New.Password)) ||
            !WifiCredentials_Valid(&New))
            return Reply(false, "error=invalid network or password");
        return Reply(WifiCredentials_Save(&New) == ESP_OK, NULL);
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) return Reply(true, NULL);
    if (argc == 2 && strcmp(argv[1], "on") == 0)
        return Reply(false, "error=Magic Trick turns Wi-Fi on from its Wi-Fi transfer screen; the network is saved");
    return Reply(false, "error=unsupported");
#else
    (void)argc; (void)argv;
    return Reply(false, "error=this build has no Wi-Fi");
#endif
}

static int UnsupportedCommand(int argc, char **argv)
{
    (void)argc;
    if (strcmp(argv[0], "tabs") == 0) return Reply(false, "error=Magic Trick has its own menu");
    return Reply(false, "error=RetroAchievements is not supported by Magic Trick");
}

esp_err_t PCConnect_RegisterConsoleCommands(void)
{
    const esp_console_cmd_t Commands[] = {
        { .command = "connect", .help = "Web ChroMagician protocol", .func = ConnectCommand },
        { .command = "wifi", .help = "Web ChroMagician Wi-Fi network", .func = WifiCommand },
        { .command = "tabs", .help = "Web ChroMagician menu tabs (unsupported)", .func = UnsupportedCommand },
        { .command = "ra", .help = "RetroAchievements (unsupported)", .func = UnsupportedCommand },
        { .command = "set", .help = "ChroMagic settings (unsupported)", .func = UnsupportedCommand },
    };
    for (size_t i = 0; i < sizeof(Commands) / sizeof(Commands[0]); ++i)
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&Commands[i]), TAG, "register %s",
                            Commands[i].command);
    return ESP_OK;
}
