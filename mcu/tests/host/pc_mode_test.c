#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../../main/pc_backup.c"
#include "mutex.h"

static void (*Worker)(void *);
static int Wakeups, Ticks, EnterCalls, ExitCalls, SleepCount, Baud, Clock;
static int Commits, SavedSetting, SettingsQueued;
static bool InMenu, GateBusy, PhysicalActive;
static esp_err_t ModeResult;
static jmp_buf StopWorker;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p) { return p; }
int xSemaphoreTake(SemaphoreHandle_t p, TickType_t t) {
    assert(!InMenu);
    if (GateBusy) { assert(t == 0); return 0; }
    assert(!p->held); p->held = 1; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t p) { assert(p->held); p->held = 0; return pdTRUE; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out) {
    Worker = fn; *out = (void *)1; return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task); ++Wakeups; }
void FPGA_Tx_SendSysCtl(void) { ++SettingsQueued; }
static bool HasVersion = true; static int VersionRequests, Delays, ArriveAfter;
bool Firmware_HasFPGAVersion(void) { return HasVersion; }
void FPGA_Tx_RequestFWVer(void) { assert(!PhysicalActive); ++VersionRequests; }
void vTaskDelay(TickType_t t) { assert(!PhysicalActive); ++Delays; if (ArriveAfter && Delays >= ArriveAfter) HasVersion = true; }
uint32_t ulTaskNotifyTake(int clear, TickType_t timeout) {
    assert(timeout == 250); if (Ticks-- == 0) longjmp(StopWorker, 1); return Wakeups ? Wakeups-- : 0;
}
static void Run(int count) { Ticks = count; if (!setjmp(StopWorker)) Worker(NULL); }
MutexResult_t Mutex_Take(MutexKey_t key) { return kMutexResult_Ok; }
MutexResult_t Mutex_Give(MutexKey_t key) { return kMutexResult_Ok; }
OSD_Result_t Settings_Update(SettingKey_t key, uint32_t value) { SavedSetting = value; return kOSD_Result_Ok; }
OSD_Result_t Settings_Commit(void) { ++Commits; return kOSD_Result_Ok; }
void PwrMgr_InhibitSleep(void) { ++SleepCount; }
void PwrMgr_AllowSleep(void) { assert(SleepCount > 0); --SleepCount; }
bool VirtualCart_IsActive(void) { return false; }
bool VirtualCart_IsBusy(void) { return false; }
esp_err_t VirtualCart_Stop(void) { return ESP_OK; }
bool CartBackup_IsPCModeActive(void) { return PhysicalActive; }
esp_err_t CartBackup_SetPCMode(bool enabled) {
    if (enabled) ++EnterCalls; else ++ExitCalls;
    if (ModeResult == ESP_OK) PhysicalActive = enabled;
    return ModeResult;
}
esp_err_t uart_wait_tx_done(int n, TickType_t t) { return ESP_OK; }
esp_err_t uart_param_config(int n, const uart_config_t *c) { Baud = c->baud_rate; Clock = c->source_clk; return ESP_OK; }
esp_err_t uart_flush_input(int n) {
    assert(!"The idle REPL holds the UART RX mutex: flushing here deadlocks"); return ESP_FAIL;
}
bool WifiFilesUI_IsBusy(void) { return WirelessOwned; }
static void Toggle(void) {
    InMenu = true; assert(PCBackupMode_OnButton(kButton_A, kButtonState_Pressed, NULL) == kOSD_Result_Ok); InMenu = false;
}
int main(void) {
    assert(PCBackup_Init() == ESP_OK); PCBackup_TransportReady();
    PCBackup_ConsoleReady(); assert(Baud == 115200 && Clock == UART_SCLK_REF_TICK);
    Toggle(); assert(PCBackup_IsEnabled() && SavedSetting == 1 && !PhysicalActive && SettingsQueued == 1);
    Run(1); assert(PhysicalActive && Baud == 2000000 && Clock == UART_SCLK_DEFAULT && SleepCount == 1);
    int calls = EnterCalls; Run(44); assert(EnterCalls == calls + 44 && PhysicalActive && SleepCount == 1);
    Toggle(); assert(SettingsQueued == 2);
    Run(1); assert(!PhysicalActive && Baud == 115200 && SleepCount == 0);
    Toggle(); Run(1); GateBusy = true;
    Toggle(); Run(3); assert(!PCBackup_IsEnabled() && PhysicalActive);
    GateBusy = false; Run(1); assert(!PhysicalActive && SleepCount == 0);
    Toggle(); Toggle(); Run(1); assert(!PhysicalActive && SleepCount == 0);
    ModeResult = ESP_ERR_INVALID_STATE; Toggle(); Run(2); assert(!PhysicalActive && SleepCount == 1);
    ModeResult = ESP_OK; Run(1); assert(PhysicalActive && SleepCount == 1);
    Toggle(); Run(1); assert(SleepCount == 0);
    assert(Commits == 8);
    assert(SettingsQueued == Commits);
    assert(PCBackup_BeginWireless() == ESP_OK);
    assert(PCBackup_IsEnabled() && PhysicalActive && SleepCount == 1);
    assert(PCBackupMode_GetState() == kPCBackupModeState_Off);
#ifdef CONFIG_CHROMAGIC_WIFI_FILES
    int BeforeCommits=Commits;Toggle();
    assert(Commits==BeforeCommits && PCBackupMode_GetState()==kPCBackupModeState_Off);
#endif
    GateBusy = true; calls = EnterCalls; Run(2); assert(EnterCalls == calls);
    assert(PCBackup_BeginWireless() == ESP_ERR_INVALID_STATE);
    GateBusy = false;
    assert(PCBackup_EndWireless() == ESP_OK);
    assert(!PCBackup_IsEnabled() && !PhysicalActive && SleepCount == 0);
    ModeResult = ESP_ERR_INVALID_STATE;
    assert(PCBackup_BeginWireless() == ESP_ERR_INVALID_STATE);
    assert(!PCBackup_IsEnabled() && !PhysicalActive && SleepCount == 0);
    ModeResult = ESP_OK;
    assert(PCBackup_BeginWireless() == ESP_OK);
    assert(PCBackup_EndWireless() == ESP_OK);
    /* Version unknown (stale FPGA session after an MCU-only restart): close the
     * session and read the version before entering PC mode. */
    assert(VersionRequests == 0 && Delays == 0);
    HasVersion = false; ArriveAfter = 3;
    int exits = ExitCalls; calls = EnterCalls;
    assert(PCBackup_BeginWireless() == ESP_OK);
    assert(ExitCalls == exits + 1 && VersionRequests == 1 && Delays == 3 && HasVersion);
    assert(EnterCalls == calls + 1 && PhysicalActive);
    assert(PCBackup_EndWireless() == ESP_OK);
    /* A core that never answers: bounded wait, then PC mode starts anyway. */
    HasVersion = false; ArriveAfter = 0; Delays = 0;
    assert(PCBackup_BeginWireless() == ESP_OK);
    assert(Delays == kVersionWait_ms / kVersionPoll_ms && PhysicalActive);
    assert(PCBackup_EndWireless() == ESP_OK);
    /* Version known: no extra exit, request or wait. */
    HasVersion = true; Delays = 0; VersionRequests = 0; exits = ExitCalls;
    assert(PCBackup_BeginWireless() == ESP_OK);
    assert(Delays == 0 && VersionRequests == 0 && ExitCalls == exits && PhysicalActive);
    assert(PCBackup_EndWireless() == ESP_OK);
    puts("PASS: idle RX, immediate toggles, persistent keepalive, transfer contention, rapid changes, retry, FPGA version before PC mode");
    return 0;
}
