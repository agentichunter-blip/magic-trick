#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
/* Portable file operations, also exercised against a temporary host directory. */
#define WIFI_FILE_PATH_SIZE 768
bool WifiFile_DecodePath(const char *Encoded, char *Relative, size_t Capacity);
bool WifiFile_Join(const char *Root, const char *Relative, char *Out, size_t Capacity);
bool WifiFile_IsInternal(const char *Name);
typedef struct {
    FILE *File;
    char Final[WIFI_FILE_PATH_SIZE], Temp[WIFI_FILE_PATH_SIZE];
    size_t Expected, Written;
} WifiFileUpload;
/* New files only: an existing file is never removed or replaced. */
bool WifiFile_Begin(WifiFileUpload *Upload, const char *Final, size_t Size, unsigned Nonce);
bool WifiFile_Write(WifiFileUpload *Upload, const void *Data, size_t Size);
bool WifiFile_Finish(WifiFileUpload *Upload);
void WifiFile_Abort(WifiFileUpload *Upload);
