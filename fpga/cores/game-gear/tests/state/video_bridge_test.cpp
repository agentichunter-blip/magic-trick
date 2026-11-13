// gg_video_bridge while a save state freezes the game: stand-in frames must
// carry exactly 144 rows of 160 pixels, all flagged repeat, at the GG frame
// rate, and resume_ok must come once per frame in vertical blank.
#include "Vgg_video_bridge.h"
#include <cstdio>
#include <cstdlib>
static void check(bool x, const char *s) { if (!x) { std::printf("FAIL %s\n", s); std::exit(1); } }
int main() {
    Vgg_video_bridge d;
    d.xclk = d.hclk = 0; d.xreset = d.hreset = 1; d.enable = 1; d.hold = 0; d.pixel = 0; d.x = d.y = 0; d.color = 0;
    auto tick = [&] { d.xclk = d.hclk = 0; d.eval(); d.xclk = d.hclk = 1; d.eval(); };
    for (int i = 0; i < 10; i++) tick();
    d.xreset = d.hreset = 0; d.hold = 1;
    unsigned long cycle = 0, frame_start = 0; int frames = 0, rows = 0, pixels = 0, row_pixels = 0, resumes = 0;
    bool in_row = false, prev_vsync = false, prev_resume = false, repeat_ok = true;
    unsigned periods[4] = {};
    while (frames < 4 && cycle < 10000000) {
        tick(); cycle++;
        if (d.lcd_pixel) { pixels++; row_pixels++; if (!d.lcd_repeat) repeat_ok = false; }
        const bool row = d.lcd_mode == 3;
        if (row && !in_row) row_pixels = 0;
        if (!row && in_row) { rows++; check(row_pixels == 160, "row length"); }
        in_row = row;
        if (d.lcd_vsync && !prev_vsync) {
            if (frame_start) { periods[frames] = cycle - frame_start; check(rows == 144 && pixels == 144 * 160, "frame size"); frames++; }
            frame_start = cycle; rows = 0; pixels = 0;
        }
        prev_vsync = d.lcd_vsync;
        if (d.resume_ok && !prev_resume) resumes++;
        prev_resume = d.resume_ok;
    }
    check(frames == 4 && repeat_ok, "four repeat frames");
    for (int i = 1; i < 4; i++) check(periods[i] > 1110000 && periods[i] < 1130000, "frame period ~16.7 ms");
    check(resumes >= 4, "resume window every frame");
    std::printf("PASS video bridge stand-in frames: 4 x 144x160 repeat pixels, %u xclk per frame, resume window each frame\n", periods[1]);
    return 0;
}
