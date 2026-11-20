#pragma once
#include <stdint.h>
typedef unsigned int UINT;
typedef uint32_t DWORD;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1, FR_NOT_ENOUGH_CORE = 17 } FRESULT;
#define FM_ANY 0x07
#define FM_SFD 0x08
typedef struct { unsigned char fmt; unsigned char n_fat; UINT align; UINT n_root; DWORD au_size; } MKFS_PARM;
FRESULT f_mkfs(const char *path, const MKFS_PARM *opt, void *work, UINT len);
