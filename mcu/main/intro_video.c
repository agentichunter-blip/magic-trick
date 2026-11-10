#include "intro_video.h"
#include <string.h>

enum { kHeaderBytes = 12, kSkip = 0xff, kPixels = INTRO_WIDTH * INTRO_HEIGHT };

bool IntroVideo_Open(IntroVideo_t *V, const uint8_t *Data, size_t Size)
{
    *V = (IntroVideo_t){0};
    if (!Data || Size < kHeaderBytes || memcmp(Data, "CMIN", 4) || Data[4] != 2)
        return false;
    const unsigned FPS = Data[5];
    const unsigned Frames = Data[6] | ((unsigned)Data[7] << 8);
    const unsigned Colors = Data[8];
    if (!FPS || FPS > 60 || !Frames || !Colors || Colors > 255 ||
        Size < kHeaderBytes + 2u * Colors)
        return false;
    *V = (IntroVideo_t){ .Data = Data, .Palette = Data + kHeaderBytes,
        .Cursor = Data + kHeaderBytes + 2u * Colors, .End = Data + Size,
        .Colors = Colors, .Frames = Frames, .FPS = FPS };
    return true;
}

bool IntroVideo_Next(IntroVideo_t *V, uint16_t Frame[INTRO_WIDTH * INTRO_HEIGHT])
{
    if (!V->Data || V->Index >= V->Frames) return false;
    unsigned Pixel = 0;
    while (Pixel < kPixels) {
        if (V->End - V->Cursor < 2) return false;
        const unsigned Op = V->Cursor[0];
        const unsigned Run = V->Cursor[1] + 1u;
        V->Cursor += 2;
        if (Run > kPixels - Pixel) return false;
        if (Op == kSkip) {
            if (V->Index == 0) return false; /* nothing to keep in frame 0 */
        } else {
            if (Op >= V->Colors) return false;
            const uint16_t Color = V->Palette[2 * Op] | ((uint16_t)V->Palette[2 * Op + 1] << 8);
            for (unsigned I = 0; I < Run; ++I) Frame[Pixel + I] = Color;
        }
        Pixel += Run;
    }
    ++V->Index;
    return true;
}
