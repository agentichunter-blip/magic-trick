#include "boot_resume.h"
#include "last_game.h"
#include "button.h"
#include "cart_backup.h"
#include "cart_bulk.h"
#include "cart_link.h"
#include "fpga_tx.h"
#include "osd.h"
#include "pc_backup.h"
#include "pwrmgr.h"
#include "virtual_cart.h"
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

static esp_err_t CheckPhysicalHeader(bool *Present)
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
     * RAM, or flash writes are performed by this probe. */
    if (EnterAttempted)
    {
        const esp_err_t ExitResult = CartLink_Transaction(kCartLinkOp_Exit, 0, 0, &Reply);
        if (ExitResult != ESP_OK) Result = ExitResult;
        else if (Reply.Status) Result = ESP_ERR_INVALID_RESPONSE;
    }
    if (Bulk) CartBulk_End();
    free(Block);
    printf("BOOT_RESUME header=%s error=%s\n", *Present ? "cartridge" : "open_bus", esp_err_to_name(Result));
    return Result;
}

static esp_err_t CartridgePresent(bool *Present)
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
    return *Present ? CheckPhysicalHeader(Present) : ESP_OK;
}

BootResumeCartridge_t BootResume_CheckCartridge(void)
{
    BootResumeCartridge_t Cartridge = {ESP_ERR_INVALID_STATE, true};
    if (Interrupted()) return Cartridge;
    PwrMgr_InhibitSleep();
    Cartridge.Result = CartridgePresent(&Cartridge.Present);
    PwrMgr_AllowSleep();
    return Cartridge;
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

void BootResume_Run(const BootResumeCartridge_t *Cartridge)
{
    if (Interrupted()) return;
    PwrMgr_InhibitSleep();
    bool Present = Cartridge ? Cartridge->Present : true;
    esp_err_t Result = Cartridge ? Cartridge->Result : ESP_ERR_INVALID_STATE;
    if (Result != ESP_OK) { printf("BOOT_RESUME skip=cartridge_unknown\n"); goto Fallback; }
    if (Present) { printf("BOOT_RESUME skip=cartridge\n"); goto Done; }
    char Path[LAST_GAME_PATH_SIZE];
    Result = LastGame_Read(Path);
    if (Result != ESP_OK) { printf("BOOT_RESUME skip=no_last_game\n"); goto Fallback; }

    /* A one-second opportunity to hold B; use fresh real FPGA levels even when
     * the menu is hidden. Manual interaction and PC mode take priority too. */
    for (unsigned Sample=0; Sample<10; ++Sample)
    {
        uint16_t Held;
        if (Interrupted()) goto Done;
        if (!FreshButtons(&Held)) { printf("BOOT_RESUME skip=buttons_unknown\n"); goto Fallback; }
        if (Held & kButtonBits_B) { printf("BOOT_RESUME skip=held_b\n"); goto Fallback; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* Only an initially empty slot reaches this point. Recheck before queuing
     * the load in case a cartridge was inserted during setup or the B window.
     * A cartridge detected at early startup never receives this second probe. */
    if (Interrupted()) goto Done;
    Result = CartridgePresent(&Present);
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
    OpenMenu();
Done:
    PwrMgr_AllowSleep();
}
