#include "nvs.h"
#include "core_switch.h"
#ifdef ESP_PLATFORM
#include "diag_log.h"
#define LYNX_DIAG(...) do { if (CoreSwitch_IsCore(kCoreSystem_Lynx)) DiagLog_Append(__VA_ARGS__); } while (0)
/* Dedicated-cart boot steps: also kept in NVS ("diaglog" reads them back),
 * since a USB console reconnect restarts the MCU and loses the power-on log. */
#define CART_DIAG(...) do { printf(__VA_ARGS__); printf("\n"); DiagLog_Append(__VA_ARGS__); } while (0)
#else
#define LYNX_DIAG(...) do { } while (0)
#define CART_DIAG(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif
#include <string.h>
#include <strings.h>
#include "boot_resume.h"
#include "last_game.h"
#include "button.h"
#include "cart_backup.h"
#include "cart_bulk.h"
#include "cart_link.h"
#include "cart_store.h"
#include "fpga_tx.h"
#include "osd.h"
#include "pc_backup.h"
#include "pwrmgr.h"
#include "virtual_cart.h"
#include "intro_video.h"
#include "startup_mode.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

static bool Interrupted(void)
{
    return PCBackup_IsEnabled() || CartBackup_IsPCModeActive() ||
        VirtualCart_IsBusy() || VirtualCart_IsActive() || OSD_IsVisible();
}

static bool OpenBusHeader(const uint8_t *Block)
{
    /* This device's empty slot reads F0, despite its presence flag staying high.
     * Also allow fully pulled-up/down buses. Any other header keeps cart priority,
     * including homebrew and cartridges with nonstandard or damaged headers. */
    const uint8_t Value = Block[0x100];
    if (Value != 0xf0 && Value != 0xff && Value != 0x00) return false;
    for (unsigned I=0x101; I<0x150; ++I)
        if (Block[I] != Value) return false;
    return true;
}

/* tools/mtcart.py titles a dedicated cart's explanation program this way. */
static bool MagicTrickHeader(const uint8_t *Block)
{
    static const char kTitle[] = "MAGIC TRICK";
    return !memcmp(Block + 0x134, kTitle, sizeof(kTitle));
}

/* A physical cartridge found at power-on stays in maintenance (core held in
 * reset) until the intro has played, so it boots once, silently, afterwards
 * instead of under the intro (BootResume_ReleaseCartridge). The engine's own
 * ~10 s idle watchdog releases it if the MCU never does. */
static bool HeldForIntro;

static esp_err_t CheckPhysicalHeader(bool *Present, bool *pMagicTrick, bool HoldCartridge)
{
    uint8_t *Block = malloc(kCartBulkBlockSize);
    if (!Block) return ESP_ERR_NO_MEM;
    esp_err_t Result = CartBulk_Begin();
    const bool Bulk = Result == ESP_OK;
    bool EnterAttempted = false;
    CartLinkResponse_t Reply = {0};
    if (Result == ESP_OK)
    {
        EnterAttempted = true;
        Result = CartLink_Transaction(kCartLinkOp_Enter, 0, 0, &Reply);
        if (Result == ESP_OK && Reply.Status) Result = ESP_ERR_INVALID_RESPONSE;
    }
    if (Result == ESP_OK) Result = CartBulk_ReadBlock(0, Block);
    if (Result == ESP_OK && pMagicTrick) *pMagicTrick = MagicTrickHeader(Block);
    if (Result == ESP_OK && OpenBusHeader(Block))
    {
        const uint8_t First = Block[0x100];
        vTaskDelay(pdMS_TO_TICKS(10));
        Result = CartBulk_ReadBlock(0, Block);
        if (Result == ESP_OK && OpenBusHeader(Block))
        {
            if (Block[0x100] != First) Result = ESP_ERR_INVALID_RESPONSE;
            else *Present = false;
        }
    }
    /* Release the physical bus even if an enter/read reply timed out. No mapper,
     * RAM, or flash writes are performed by this probe. A plain cartridge that
     * read cleanly may stay held for the intro. */
    const bool Hold = HoldCartridge && Result == ESP_OK && *Present &&
                      !(pMagicTrick && *pMagicTrick);
    if (Hold)
        HeldForIntro = true;
    else if (EnterAttempted)
    {
        const esp_err_t ExitResult = CartLink_Transaction(kCartLinkOp_Exit, 0, 0, &Reply);
        if (ExitResult != ESP_OK) Result = ExitResult;
        else if (Reply.Status) Result = ESP_ERR_INVALID_RESPONSE;
    }
    if (Bulk) CartBulk_End();
    free(Block);
    if (!*Present && pMagicTrick) *pMagicTrick = false;
    printf("BOOT_RESUME header=%s error=%s\n", !*Present ? "open_bus" :
           pMagicTrick && *pMagicTrick ? "magic_trick" : "cartridge", esp_err_to_name(Result));
    return Result;
}

static esp_err_t CartridgePresent(bool *Present, bool *pMagicTrick, bool HoldCartridge)
{
    CartLinkResponse_t Reply = {0};
    esp_err_t Result = ESP_ERR_TIMEOUT;
    for (unsigned Attempt=0; Attempt<5 && Result==ESP_ERR_TIMEOUT; ++Attempt)
    {
        Result = CartLink_TransactionOnce(kCartLinkOp_Ping, 0, 0, &Reply);
        if (Result == ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (Result != ESP_OK) return Result;
    if (Reply.Status || Reply.Count != 4 || Reply.Data[2] != 3)
        return ESP_ERR_INVALID_RESPONSE;
    if (Reply.Data[1] & 1u) return ESP_ERR_INVALID_STATE; /* maintenance in progress */
    *Present = (Reply.Data[1] & 2u) != 0;
    return *Present ? CheckPhysicalHeader(Present, pMagicTrick, HoldCartridge) : ESP_OK;
}

BootResumeCartridge_t BootResume_CheckCartridge(void)
{
    BootResumeCartridge_t Cartridge = {ESP_ERR_INVALID_STATE, true, false};
    if (Interrupted()) return Cartridge;
    PwrMgr_InhibitSleep();
    Cartridge.Result = CartridgePresent(&Cartridge.Present, &Cartridge.MagicTrick, true);
    PwrMgr_AllowSleep();
    return Cartridge;
}

void BootResume_ReleaseCartridge(void)
{
    if (!HeldForIntro) return;
    HeldForIntro = false;
    CartLinkResponse_t Reply = {0};
    const bool Bulk = CartBulk_Begin() == ESP_OK;
    const esp_err_t Result = CartLink_Transaction(kCartLinkOp_Exit, 0, 0, &Reply);
    if (Bulk) CartBulk_End();
    printf("BOOT_RESUME cartridge_release=%s status=%u\n", esp_err_to_name(Result), (unsigned)Reply.Status);
}

bool BootResume_TakeHeldCartridge(void)
{
    const bool Held = HeldForIntro;
    HeldForIntro = false;
    return Held;
}

static bool FreshButtons(uint16_t *Held)
{
    const uint32_t Before = Button_GetSnapshot(Held);
    if (FPGA_Tx_RequestButtons() != ESP_OK) return false;
    for (unsigned Poll=0; Poll<20; ++Poll)
    {
        if (Button_GetSnapshot(Held) != Before) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false; /* Never assume B is released when telemetry is missing. */
}

static void OpenMenu(void)
{
    /* The existing core cancels a menu toggle whenever a game button is held.
     * Latch the bypass in this path and wait for release before pulsing Menu.
     * Never resume the game just because B was subsequently released. */
    unsigned Released = 0;
    bool Waiting = false;
    while (!Interrupted())
    {
        uint16_t Held;
        if (!FreshButtons(&Held))
        {
            printf("BOOT_RESUME menu=buttons_unknown\n");
            return;
        }
        if (Interrupted()) return;
        /* Bits 0..7 are game buttons; bit 8 is the physical Menu button.
         * Bit 9 reports menu-closed state and must not block opening it. */
        if (Held & 0x1ffu)
        {
            Released = 0;
            if (!Waiting) printf("BOOT_RESUME menu=waiting_for_release\n");
            Waiting = true;
        }
        else if (++Released == 2)
        {
            const esp_err_t Result = FPGA_Tx_PulseButtons(kButtonBits_MenuEnAlt);
            if (Result != ESP_OK)
            {
                printf("BOOT_RESUME menu=%s\n", esp_err_to_name(Result));
                return;
            }
            /* Sending a pulse alone does not prove that the core accepted it. */
            for (unsigned Poll=0; Poll<20; ++Poll)
            {
                if (FreshButtons(&Held) && !(Held & (kButtonBits_MenuEn | kButtonBits_MenuEnAlt)))
                {
                    printf("BOOT_RESUME menu=visible\n");
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            printf("BOOT_RESUME menu=unconfirmed\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Bit 9 (kButtonBits_MenuEn) reports the menu-closed state; bit 8
 * (kButtonBits_MenuEnAlt) is the physical Menu button, also used to pulse. */
static bool WaitMenu(bool Visible)
{
    for (unsigned Poll=0; Poll<20; ++Poll)
    {
        uint16_t Held;
        if (FreshButtons(&Held) &&
            (Visible ? !(Held & (kButtonBits_MenuEn | kButtonBits_MenuEnAlt))
                     : (Held & kButtonBits_MenuEn) != 0)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool BootResume_PlayIntro(const BootResumeCartridge_t *Cartridge,
                          uint16_t *Frame, void (*Present)(void))
{
    IntroVideo_t Video;
    (void)Cartridge; /* plays with or without a cartridge; the menu overlay pauses it */
    if (CoreSwitch_HasPending()) { printf("BOOT_INTRO skip=core_switch\n"); return false; }
    if (PCBackup_IsEnabled()) { printf("BOOT_INTRO skip=pc_mode\n"); return false; }
    if (!Frame || !Present || !IntroVideo_Open(&Video, IntroVideoData, IntroVideoSize)) { printf("BOOT_INTRO skip=data\n"); return false; }
    uint16_t Held;
    if (!FreshButtons(&Held)) { printf("BOOT_INTRO skip=buttons_unknown\n"); return false; }
    if (Held & 0xffu) { printf("BOOT_INTRO skip=button_held\n"); return false; }
    if (Held & kButtonBits_MenuEnAlt) { printf("BOOT_INTRO skip=button_held\n"); return false; }
    /* The overlay may already be up at power-on; then leave it as found. */
    const bool WasOpen = !(Held & kButtonBits_MenuEn);

    PwrMgr_InhibitSleep();
    /* Draw frame 0 before revealing the overlay so no stale pixels flash. */
    if (!IntroVideo_Next(&Video, Frame)) { PwrMgr_AllowSleep(); return false; }
    Present();
    if (!WasOpen && (FPGA_Tx_PulseButtons(kButtonBits_MenuEnAlt) != ESP_OK || !WaitMenu(true))) {
        printf("BOOT_INTRO skip=menu_unconfirmed\n");
        if (WaitMenu(true)) { (void)FPGA_Tx_PulseButtons(kButtonBits_MenuEnAlt); (void)WaitMenu(false); }
        PwrMgr_AllowSleep();
        return false;
    }
    const int64_t Start = esp_timer_get_time();
    const int64_t Period = 1000000 / Video.FPS;
    unsigned Shown = 1;
    bool Skipped = false;
    while (IntroVideo_Next(&Video, Frame))
    {
        const int64_t Due = Start + (int64_t)Shown * Period;
        const int64_t Now = esp_timer_get_time();
        if (Due > Now) vTaskDelay(pdMS_TO_TICKS((Due - Now + 999) / 1000));
        Present();
        ++Shown;
        if ((Shown % 4) == 0) (void)FPGA_Tx_RequestButtons();
        (void)Button_GetSnapshot(&Held);
        if ((Held & 0xffu) || PCBackup_IsEnabled()) { Skipped = true; break; }
    }
    if (!Skipped) vTaskDelay(pdMS_TO_TICKS(300)); /* rest on the final card */
    const bool Restored = WasOpen ||
        (FPGA_Tx_PulseButtons(kButtonBits_MenuEnAlt) == ESP_OK && WaitMenu(false));
    Button_ResetAll();
    printf("BOOT_INTRO played frames=%u/%u ms=%lld skipped=%d menu_was_open=%d restored=%d\n", Shown, Video.Frames,
           (esp_timer_get_time() - Start) / 1000, Skipped, WasOpen, Restored);
    PwrMgr_AllowSleep();
    return true;
}

typedef enum { kWindowClear, kWindowInterrupted, kWindowSkip } SkipWindow_t;

/* A one-second opportunity to hold B; use fresh real FPGA levels even when
 * the menu is hidden. Manual interaction and PC mode take priority too. */
static SkipWindow_t SkipWindow(const char *pTag)
{
    for (unsigned Sample=0; Sample<10; ++Sample)
    {
        uint16_t Held;
        if (Interrupted()) return kWindowInterrupted;
        if (!FreshButtons(&Held)) { printf("%s skip=buttons_unknown\n", pTag); return kWindowSkip; }
        if (Held & kButtonBits_B) { printf("%s skip=held_b\n", pTag); return kWindowSkip; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return kWindowClear;
}

/* A dedicated Magic Trick cart is in: boot its game (staged by this GB core,
 * which then switches cores) instead of leaving its explanation program up.
 * Returns true when the menu should open instead (B held, several games,
 * or a failed load). */
static bool BootCartGame(void)
{
    if (!CoreSwitch_IsCore(kCoreSystem_GB)) { CART_DIAG("BOOT_CART skip=core"); return true; }
    const SkipWindow_t Window = SkipWindow("BOOT_CART");
    if (Window != kWindowClear) CART_DIAG("BOOT_CART window=%s", Window == kWindowSkip ? "skip" : "interrupted");
    if (Window == kWindowInterrupted) return false;
    if (Window == kWindowSkip) return true;
    char Path[LAST_GAME_PATH_SIZE];
    esp_err_t Result = CartStore_AutoBootPath(Path, sizeof(Path));
    if (Result != ESP_OK) { CART_DIAG("BOOT_CART skip=%s", Result == ESP_ERR_INVALID_SIZE ? "several_games" : esp_err_to_name(Result)); return true; }
    if (Interrupted()) { CART_DIAG("BOOT_CART interrupted"); return false; }
    CART_DIAG("BOOT_CART start=%s", Path);
    Result = VirtualCart_Start(Path);
    if (Result != ESP_OK) { CART_DIAG("BOOT_CART start_error=%s", esp_err_to_name(Result)); return true; }
    while (VirtualCart_IsBusy()) vTaskDelay(pdMS_TO_TICKS(50));
    if (!VirtualCart_IsActive()) { CART_DIAG("BOOT_CART load_failed"); return true; }
    CART_DIAG("BOOT_CART complete");
    return false;
}

void BootResume_Run(const BootResumeCartridge_t *Cartridge)
{
    SkipWindow_t Window;
    char Path[LAST_GAME_PATH_SIZE];
    uint8_t Hops = 1;
    CoreSystem_t Target = kCoreSystem_None;
    const esp_err_t PendingResult = CoreSwitch_TakePendingTarget(Path, &Hops, &Target);
    LYNX_DIAG("boot pending=%s hops=%u path=%s", esp_err_to_name(PendingResult), (unsigned)Hops, Path);
    /* main kept the start-up hold for a core switch: a cartridge game staged
     * here takes it over (CartStore_HoldCart), anything else lets the
     * cartridge go now. */
    if (!(PendingResult == ESP_OK && CartStore_IsCartPath(Path) && CoreSwitch_IsCore(Target) &&
          CartStore_CanReadCart()))
        BootResume_ReleaseCartridge();
    if (Interrupted()) { LYNX_DIAG("boot interrupted"); BootResume_ReleaseCartridge(); return; }
    PwrMgr_InhibitSleep();
    if (PendingResult == ESP_OK) {
        if (!CoreSwitch_IsCore(Target)) {
            /* Three cores in a cycle: this reload may have landed on the core
             * between. Reload once more (bounded); a refusal falls back. */
            printf("CORE_SWITCH resume=intermediate hop=%u\n", (unsigned)Hops);
            const esp_err_t ContinueResult = CoreSwitch_ContinueTo(Path, Hops, Target);
            printf("CORE_SWITCH resume=wrong_core continue=%s; pending consumed\n", esp_err_to_name(ContinueResult));
            goto Fallback;
        }
        /* Explicit selection outranks cartridge auto-resume. The pending record
         * is already durably removed: a failed load cannot cause a reboot loop. */
        printf("CORE_SWITCH resume=%s\n", Path);
        const esp_err_t StartResult = VirtualCart_Start(Path);
        LYNX_DIAG("boot start=%s", esp_err_to_name(StartResult));
        if (StartResult != ESP_OK) goto Fallback;
        while (VirtualCart_IsBusy()) vTaskDelay(pdMS_TO_TICKS(50));
        LYNX_DIAG("boot active=%u", VirtualCart_IsActive() ? 1u : 0u);
        if (!VirtualCart_IsActive()) goto Fallback;
        goto Done;
    }
    if (PendingResult != ESP_ERR_NVS_NOT_FOUND) {
        printf("CORE_SWITCH pending_error=%s\n", esp_err_to_name(PendingResult));
        goto Fallback;
    }
    bool Present = Cartridge ? Cartridge->Present : true;
    esp_err_t Result = Cartridge ? Cartridge->Result : ESP_ERR_INVALID_STATE;
    if (Result != ESP_OK) { printf("BOOT_RESUME skip=cartridge_unknown\n"); goto Fallback; }
    if (Present && Cartridge && Cartridge->MagicTrick) {
        CART_DIAG("BOOT_CART magic_trick_cart");
        if (BootCartGame()) goto Fallback;
        goto Done;
    }
    if (Present) { printf("BOOT_RESUME skip=cartridge\n"); goto Done; }
    if (StartUp_GetMode() == kStartUpMode_GamesMenu) { printf("BOOT_RESUME skip=startup_menu\n"); goto Fallback; }
    Result = LastGame_Read(Path);
    if (Result != ESP_OK) { printf("BOOT_RESUME skip=no_last_game\n"); goto Fallback; }

    Window = SkipWindow("BOOT_RESUME");
    if (Window == kWindowInterrupted) goto Done;
    if (Window == kWindowSkip) goto Fallback;
    /* Only an initially empty slot reaches this point. Recheck before queuing
     * the load in case a cartridge was inserted during setup or the B window.
     * A cartridge detected at early startup never receives this second probe. */
    if (Interrupted()) goto Done;
    Result = CartridgePresent(&Present, NULL, false);
    if (Result != ESP_OK) { printf("BOOT_RESUME skip=cartridge_unknown\n"); goto Fallback; }
    if (Present) { printf("BOOT_RESUME skip=cartridge\n"); goto Done; }
    uint16_t Held;
    if (!FreshButtons(&Held)) { printf("BOOT_RESUME skip=buttons_unknown\n"); goto Fallback; }
    if (Held & kButtonBits_B) { printf("BOOT_RESUME skip=held_b\n"); goto Fallback; }
    if (Interrupted()) goto Done;
    printf("BOOT_RESUME start=last_game\n");
    Result = VirtualCart_Start(Path);
    if (Result != ESP_OK) { printf("BOOT_RESUME start_error=%s\n", esp_err_to_name(Result)); goto Fallback; }
    while (VirtualCart_IsBusy()) vTaskDelay(pdMS_TO_TICKS(50));
    if (!VirtualCart_IsActive()) { printf("BOOT_RESUME load_failed\n"); goto Fallback; }
    printf("BOOT_RESUME complete\n");
    goto Done;
Fallback:
    BootResume_ReleaseCartridge();
    OpenMenu();
Done:
    BootResume_ReleaseCartridge(); /* no-op unless a cart staging never took it */
    PwrMgr_AllowSleep();
}
