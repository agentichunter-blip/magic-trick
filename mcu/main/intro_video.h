#pragma once
/* Built-in power-on intro (tools/intro/encode_intro.py). 160x144 RGB565
 * frames, delta + run-length coded against one global palette. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTRO_WIDTH  160
#define INTRO_HEIGHT 144

extern const uint8_t IntroVideoData[];
extern const size_t IntroVideoSize;

typedef struct {
    const uint8_t *Data, *Cursor, *End;
    const uint8_t *Palette;   /* Count little-endian RGB565 entries */
    unsigned Colors, Frames, FPS, Index;
} IntroVideo_t;

/* Validates the header; false for corrupt or unsupported data. */
bool IntroVideo_Open(IntroVideo_t *V, const uint8_t *Data, size_t Size);
/* Applies the next frame onto Frame (which must hold the previous frame;
 * frame 0 overwrites every pixel). False at the end or on corrupt data. */
bool IntroVideo_Next(IntroVideo_t *V, uint16_t Frame[INTRO_WIDTH * INTRO_HEIGHT]);
