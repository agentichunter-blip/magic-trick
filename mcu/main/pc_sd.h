#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t PCSD_Run(void);

/* Web ChroMagician SD operations; paths are the page's ("/GB/x.gb"). On
 * failure *ppDetail names a failure the page knows, or is NULL. */
bool PCSD_CardPresent(bool *pPresent);  /* false: card busy, unknown */
esp_err_t PCSD_ConnectList(const char *pPath, const char **ppDetail);
esp_err_t PCSD_ConnectGet(const char *pPath, const char **ppDetail);
esp_err_t PCSD_ConnectPut(const char *pPath, uint32_t Size, uint32_t Crc,
                          const char **ppDetail);
esp_err_t PCSD_ConnectMkdir(const char *pPath, const char **ppDetail);
esp_err_t PCSD_ConnectRemove(const char *pPath, const char **ppDetail);
esp_err_t PCSD_ConnectMove(const char *pFrom, const char *pTo, const char **ppDetail);
/* Cartridge to /CHROMAGIC/BACKUPS/<Stem>.gb(c) (+ .sav/.rtc), or the save
 * alone; prints CONNECT PROGRESS and CONNECT SAVED. */
esp_err_t PCSD_ConnectBackup(bool SaveOnly, const char *pStem, bool Color,
                             const char **ppDetail);
