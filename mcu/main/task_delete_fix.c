/* ESP-IDF 5.3 vTaskDeleteWithCaps() leaks when a task deletes itself: vTaskDelete(self)
 * never returns, so the stack and TCB it allocated are never freed. The HTTP server's
 * task ends that way, so every Wi-Fi Files session lost ~6.5 KB of internal RAM and the
 * 4th session in a row failed to start (2026-11-18). The linker wraps the function
 * (main/CMakeLists.txt, --wrap) with the fix ESP-IDF later shipped: a self-deleting task
 * suspends, and a short-lived helper task on the same core deletes it and frees both. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_cpu.h"

void __real_vTaskDeleteWithCaps(TaskHandle_t Task);

static void DeleteLater(void *Arg)
{
    TaskHandle_t Task = Arg;
    /* The helper runs at idle priority on the task's core, so the task has normally
     * suspended already; wait until it has, never delete it while it runs. */
    while (eTaskGetState(Task) != eSuspended) vTaskDelay(1);
    __real_vTaskDeleteWithCaps(Task);
    vTaskDelete(NULL);
}

void __wrap_vTaskDeleteWithCaps(TaskHandle_t Task)
{
    TaskHandle_t Self = xTaskGetCurrentTaskHandle();
    if (Task == NULL || Task == Self)
    {
        if (xTaskCreatePinnedToCore(DeleteLater, "task_reaper", 2048, Self, tskIDLE_PRIORITY, NULL,
                                    (BaseType_t)esp_cpu_get_core_id()) == pdPASS)
        {
            vTaskSuspend(NULL);
        }
        /* No memory for the helper: fall back to the old behaviour (leaks, but ends the task). */
        vTaskDelete(NULL);
    }
    __real_vTaskDeleteWithCaps(Task);
}
