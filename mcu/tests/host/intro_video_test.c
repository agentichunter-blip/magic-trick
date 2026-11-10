/* Boot intro decoder: full decode of the embedded video plus corruption checks. */
#include "intro_video.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint32_t Crc32(const uint8_t *P, size_t N) {
    uint32_t C = 0xffffffffu;
    for (size_t I = 0; I < N; ++I) { C ^= P[I]; for (int K = 0; K < 8; ++K) C = (C >> 1) ^ (0xedb88320u & -(C & 1)); }
    return ~C;
}
static uint16_t Frame[INTRO_WIDTH * INTRO_HEIGHT];
int main(void) {
    IntroVideo_t V;
    assert(IntroVideo_Open(&V, IntroVideoData, IntroVideoSize));
    assert(V.FPS == 30 && V.Frames == 144 && V.Colors >= 1);
    for (unsigned C = 0; C < V.Colors; ++C) /* never the OSD transparent key */
        assert((V.Palette[2*C] | (V.Palette[2*C+1] << 8)) != 0xf81f);
    unsigned N = 0;
    while (IntroVideo_Next(&V, Frame)) ++N;
    assert(N == 144 && V.Cursor == V.End);
    /* Independent Python decode of the same data (update when re-encoding). */
    assert(Crc32((const uint8_t *)Frame, sizeof(Frame)) == 0x9c81e431u);
    /* Corruption must fail cleanly, never write out of bounds. */
    uint8_t *Copy = malloc(IntroVideoSize); memcpy(Copy, IntroVideoData, IntroVideoSize);
    assert(!IntroVideo_Open(&V, Copy, 8));
    Copy[4] = 9; assert(!IntroVideo_Open(&V, Copy, IntroVideoSize)); Copy[4] = IntroVideoData[4];
    const size_t First = 12 + 2u * Copy[8];
    assert(IntroVideo_Open(&V, Copy, First + 10));             /* truncated */
    assert(!IntroVideo_Next(&V, Frame));
    Copy[First] = 0xff;                                          /* skip in frame 0 */
    assert(IntroVideo_Open(&V, Copy, IntroVideoSize) && !IntroVideo_Next(&V, Frame));
    Copy[First] = Copy[8];                                       /* colour out of range */
    assert(IntroVideo_Open(&V, Copy, IntroVideoSize) && !IntroVideo_Next(&V, Frame));
    free(Copy);
    /* Synthetic stream: 90 runs of 255 px leave 90 px, then a 256 px run overflows. */
    static uint8_t Syn[12 + 2 + 2 * 91];
    memcpy(Syn, "CMIN", 4); Syn[4] = 2; Syn[5] = 30; Syn[6] = 1; Syn[8] = 1; Syn[12] = 0x34; Syn[13] = 0x12;
    for (unsigned I = 0; I < 90; ++I) { Syn[14 + 2*I] = 0; Syn[15 + 2*I] = 254; }
    Syn[14 + 180] = 0; Syn[15 + 180] = 255;
    assert(IntroVideo_Open(&V, Syn, sizeof(Syn)) && !IntroVideo_Next(&V, Frame));
    Syn[15 + 180] = 89;                                         /* exact fit decodes */
    assert(IntroVideo_Open(&V, Syn, sizeof(Syn)) && IntroVideo_Next(&V, Frame) && Frame[23039] == 0x1234);
    puts("PASS: boot intro decodes 144 frames exactly; corrupt data rejected");
    return 0;
}
