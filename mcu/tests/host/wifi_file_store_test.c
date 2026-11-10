#include "wifi_file_store.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void Contents(const char *Path, const char *Expected)
{
    FILE *F = fopen(Path, "rb"); assert(F);
    char Data[32] = {0}; size_t N = fread(Data, 1, sizeof(Data), F); fclose(F);
    assert(N == strlen(Expected) && !memcmp(Data, Expected, N));
}
int main(void)
{
    char Relative[768], Path[768];
    const char *Bad[] = {"", "x", "/..", "/a/../b", "/%2e%2e/b", "/a//b", "/a/", "/a%00b", "/a%5cb", "/a:", "/a.", "/a%20", "/a%", "/a%0", "/a%gg", "/.cmwifi-123.tmp", "/.CMWIFI-123.tmp", "/a/%0ab"};
    for (unsigned I = 0; I < sizeof(Bad)/sizeof(Bad[0]); ++I)
        assert(!WifiFile_DecodePath(Bad[I], Relative, sizeof(Relative)));
    assert(WifiFile_DecodePath("/", Relative, sizeof(Relative)));
    assert(WifiFile_DecodePath("%2FCHROMAGIC%2FBACKUPS%2FGame%20%26%20Watch.gbc", Relative, sizeof(Relative)));
    assert(!strcmp(Relative, "/CHROMAGIC/BACKUPS/Game & Watch.gbc"));
    assert(WifiFile_DecodePath("/100%25.gbc", Relative, sizeof(Relative)));
    assert(!strcmp(Relative, "/100%.gbc"));
    assert(!WifiFile_DecodePath("/long", Relative, 3));
    char Long[260]; memset(Long, 'a', sizeof(Long)); Long[0] = '/'; Long[259] = 0;
    assert(!WifiFile_DecodePath(Long, Relative, sizeof(Relative)));
    char Root[] = "/tmp/chromagic-wifi-test-XXXXXX"; assert(mkdtemp(Root));
    assert(WifiFile_Join(Root, "/save.sav", Path, sizeof(Path)));
    assert(!WifiFile_Join(Root, "/save.sav", Relative, 4));
    WifiFileUpload Upload;
    assert(WifiFile_Begin(&Upload, Path, 4, 1));
    assert(WifiFile_Write(&Upload, "save", 4));
    assert(!WifiFile_Write(&Upload, "!", 1));
    assert(WifiFile_Finish(&Upload)); Contents(Path, "save");
    assert(!WifiFile_Begin(&Upload, Path, 4, 2)); Contents(Path, "save");
    assert(WifiFile_Join(Root, "/game.gbc", Path, sizeof(Path)));
    assert(WifiFile_Begin(&Upload, Path, 8, 3));
    assert(WifiFile_Write(&Upload, "part", 4));
    char Temp[768]; strcpy(Temp, Upload.Temp);
    assert(!WifiFile_Finish(&Upload)); assert(access(Path, F_OK) != 0); assert(access(Temp, F_OK) != 0);
    assert(WifiFile_Begin(&Upload, Path, 8, 4)); assert(WifiFile_Write(&Upload, "part", 4));
    strcpy(Temp, Upload.Temp); WifiFile_Abort(&Upload); assert(access(Path, F_OK) != 0 && access(Temp, F_OK) != 0);
    /* A destination appearing before publication must never be overwritten. */
    assert(WifiFile_Begin(&Upload, Path, 3, 5)); assert(WifiFile_Write(&Upload, "new", 3));
    FILE *F = fopen(Path, "wb"); assert(F); fputs("old", F); fclose(F);
    assert(!WifiFile_Finish(&Upload)); Contents(Path, "old"); unlink(Path);
    assert(WifiFile_Begin(&Upload, Path, 0, 6)); assert(WifiFile_Finish(&Upload)); Contents(Path, ""); unlink(Path);
    assert(WifiFile_Join(Root, "/save.sav", Path, sizeof(Path))); unlink(Path); assert(rmdir(Root) == 0);
    puts("PASS: paths, traversal, bounded decoding, upload, no overwrite, partial/cancel cleanup, empty files");
}
