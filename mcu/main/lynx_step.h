#pragma once
/* Step markers for code paths that can run with the Atari Lynx core loaded, which
 * has no USB console: they go to the persistent diagnostic log (read it back with
 * the "diaglog" console command after switching to a core with USB). */
#ifdef ESP_PLATFORM
#include "core_switch.h"
#include "diag_log.h"
#define LYNX_STEP(...) do { if (CoreSwitch_IsCore(kCoreSystem_Lynx)) DiagLog_Append(__VA_ARGS__); } while (0)
#else
#define LYNX_STEP(...) do { } while (0)
#endif
