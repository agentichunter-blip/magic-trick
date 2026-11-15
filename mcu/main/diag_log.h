#pragma once
/* Small persistent diagnostic log in NVS (namespace "diaglog"), for cores
 * without a USB console (Atari Lynx): append lines there, read them back
 * with the "diaglog" console command on a core that has USB. */
void DiagLog_Append(const char *Format, ...) __attribute__((format(printf, 1, 2)));
void DiagLog_RegisterConsoleCommand(void);
