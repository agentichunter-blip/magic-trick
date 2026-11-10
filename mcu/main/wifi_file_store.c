#include "wifi_file_store.h"
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

static int Hex(unsigned char C)
{
    if (C >= '0' && C <= '9') return C - '0';
    if (C >= 'a' && C <= 'f') return C - 'a' + 10;
    if (C >= 'A' && C <= 'F') return C - 'A' + 10;
    return -1;
}
bool WifiFile_IsInternal(const char *Name)
{
    return strncasecmp(Name, ".cmwifi-", 8) == 0;
}
bool WifiFile_DecodePath(const char *Encoded, char *Relative, size_t Capacity)
{
    if (!Encoded || !Relative || Capacity < 2) return false;
    size_t N = 0;
    for (size_t I = 0; Encoded[I]; ++I)
    {
        unsigned char C = Encoded[I];
        if (C == '%')
        {
            if (!Encoded[I+1] || !Encoded[I+2]) return false;
            int A = Hex(Encoded[I+1]), B = Hex(Encoded[I+2]);
            if (A < 0 || B < 0) return false;
            C = (A << 4) | B; I += 2;
        }
        else if (C == '+') C = ' ';
        if (C < 32 || C == 127 || strchr("\\:*?\"<>|", C) || N + 1 >= Capacity)
            return false;
        Relative[N++] = C;
    }
    Relative[N] = 0;
    if (!N || Relative[0] != '/') return false;
    if (N == 1) return true;
    size_t Start = 1;
    for (size_t I = 1; I <= N; ++I)
    {
        if (Relative[I] && Relative[I] != '/') continue;
        size_t Len = I - Start;
        if (!Len || Len > 255 || Relative[I-1] == '.' || Relative[I-1] == ' ' ||
            WifiFile_IsInternal(Relative + Start)) return false;
        Start = I + 1;
    }
    return true;
}
bool WifiFile_Join(const char *Root, const char *Relative, char *Out, size_t Capacity)
{
    if (!Root || !Relative || Relative[0] != '/') return false;
    int N = snprintf(Out, Capacity, "%s%s", Root, Relative);
    return N >= 0 && (size_t)N < Capacity;
}
void WifiFile_Abort(WifiFileUpload *Upload)
{
    if (Upload->File) fclose(Upload->File);
    Upload->File = NULL;
    if (Upload->Temp[0]) unlink(Upload->Temp);
    Upload->Temp[0] = 0;
}
bool WifiFile_Begin(WifiFileUpload *Upload, const char *Final, size_t Size, unsigned Nonce)
{
    memset(Upload, 0, sizeof(*Upload));
    struct stat Info;
    if (strlen(Final) >= sizeof(Upload->Final) || stat(Final, &Info) == 0 || errno != ENOENT)
        return false;
    strcpy(Upload->Final, Final);
    const char *Slash = strrchr(Final, '/');
    if (!Slash) return false;
    int N = snprintf(Upload->Temp, sizeof(Upload->Temp), "%.*s/.cmwifi-%08x.tmp",
                     (int)(Slash - Final), Final, Nonce);
    if (N < 0 || (size_t)N >= sizeof(Upload->Temp)) { Upload->Temp[0] = 0; return false; }
    Upload->File = fopen(Upload->Temp, "wbx");
    if (!Upload->File) { Upload->Temp[0] = 0; return false; }
    Upload->Expected = Size;
    return true;
}
bool WifiFile_Write(WifiFileUpload *Upload, const void *Data, size_t Size)
{
    if (!Upload->File || Size > Upload->Expected - Upload->Written) return false;
    if (fwrite(Data, 1, Size, Upload->File) != Size) return false;
    Upload->Written += Size;
    return true;
}
bool WifiFile_Finish(WifiFileUpload *Upload)
{
    bool Ok = Upload->File && Upload->Written == Upload->Expected;
    if (Ok) Ok = fflush(Upload->File) == 0 && fsync(fileno(Upload->File)) == 0;
    if (Upload->File && fclose(Upload->File) != 0) Ok = false;
    Upload->File = NULL;
    /* The transfer mode holds exclusive ownership of the SD card, so no other
     * task can create Final between this check and rename. */
    struct stat Info;
    if (Ok) Ok = stat(Upload->Final, &Info) != 0 && errno == ENOENT;
    if (Ok) Ok = rename(Upload->Temp, Upload->Final) == 0;
    if (Ok) Upload->Temp[0] = 0;
    WifiFile_Abort(Upload);
    return Ok;
}
