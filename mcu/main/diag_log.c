#include "diag_log.h"
#include "esp_console.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { kLogSize = 1800 };
static SemaphoreHandle_t Lock;
static char Buffer[kLogSize + 1];

void DiagLog_Append(const char *Format, ...)
{
    char Line[200];
    va_list Args;
    va_start(Args, Format);
    vsnprintf(Line, sizeof(Line), Format, Args);
    va_end(Args);
    printf("DIAGLOG %s\n", Line);
    if (!Lock) Lock = xSemaphoreCreateMutex();
    if (!Lock || xSemaphoreTake(Lock, pdMS_TO_TICKS(500)) != pdTRUE) return;
    nvs_handle_t Handle;
    if (nvs_open("diaglog", NVS_READWRITE, &Handle) == ESP_OK) {
        size_t Size = sizeof(Buffer);
        if (nvs_get_str(Handle, "log", Buffer, &Size) != ESP_OK) Buffer[0] = 0;
        size_t Have = strlen(Buffer), Add = strlen(Line) + 1;
        if (Have + Add > kLogSize) { /* keep the newest lines */
            size_t Drop = Have + Add - kLogSize;
            const char *Cut = strchr(Buffer + Drop, '\n');
            Have = Cut ? strlen(Cut + 1) : 0;
            memmove(Buffer, Cut ? Cut + 1 : Buffer + Have, Have + 1);
        }
        snprintf(Buffer + Have, sizeof(Buffer) - Have, "%s\n", Line);
        (void)nvs_set_str(Handle, "log", Buffer);
        (void)nvs_commit(Handle);
        nvs_close(Handle);
    }
    xSemaphoreGive(Lock);
}

static int Command(int argc, char **argv)
{
    nvs_handle_t Handle;
    if (nvs_open("diaglog", NVS_READWRITE, &Handle) != ESP_OK) { printf("DIAGLOG none\n"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        (void)nvs_erase_key(Handle, "log"); (void)nvs_commit(Handle);
        printf("DIAGLOG cleared\n");
    } else {
        size_t Size = sizeof(Buffer);
        if (nvs_get_str(Handle, "log", Buffer, &Size) == ESP_OK) printf("%sDIAGLOG END\n", Buffer);
        else printf("DIAGLOG empty\nDIAGLOG END\n");
    }
    nvs_close(Handle);
    return 0;
}

void DiagLog_RegisterConsoleCommand(void)
{
    const esp_console_cmd_t Cmd = {.command = "diaglog", .help = "Print (or clear) the persistent diagnostic log",
                                   .hint = "[clear]", .func = Command};
    (void)esp_console_cmd_register(&Cmd);
}
