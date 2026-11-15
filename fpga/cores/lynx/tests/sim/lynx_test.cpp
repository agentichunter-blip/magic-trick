// Lynx core simulation: boot a BS93 program, measure panel/Lynx frame timing,
// dump frames, and round-trip a save state through the MCU command path.
// usage: lynx_test <file> <outdir> [frames] [lock60:0/1]
#include "Vlynx_sim_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static Vlynx_sim_top *t;
static uint64_t cycle = 0;
static int frame_no = 0, row = -1, col = 0;
static uint16_t img[144][160];
static std::vector<uint64_t> frame_starts;
static int repeat_pixels = 0, frame_pixels = 0;
static std::string outdir;
static int last_mode = 0;
static long audio_changes = 0; static int last_left = 0;

static void dump(const char *name) {
    std::string p = outdir + "/" + name;
    FILE *f = fopen(p.c_str(), "wb");
    fprintf(f, "P6 160 144 255\n");
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) {
            uint16_t c = img[y][x];
            unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            fputc(r * 255 / 31, f); fputc(g * 255 / 31, f); fputc(b * 255 / 31, f);
        }
    fclose(f);
}

static void step() {
    // xclk period 1 unit; hclk = xclk/4
    t->xclk = 1;
    if ((cycle & 3) == 0) t->hclk = 1;
    t->eval();
    if ((cycle & 3) == 0) {
        // sample hclk-domain LCD outputs after the edge
        if (t->lcd_vsync && t->lcd_mode == 3 && last_mode != 3) {
            if (row >= 0) { frame_no++; }
            frame_starts.push_back(cycle);
            row = 0; col = 0;
        } else if (t->lcd_mode == 3 && last_mode != 3 && row >= 0) { row++; col = 0; }
        last_mode = t->lcd_mode;
        if (t->lcd_pixel && row >= 0 && row < 144 && col < 160) {
            frame_pixels++;
            if (t->lcd_repeat) repeat_pixels++;
            else img[row][col] = t->lcd_data;
            col++;
        }
        if ((int)(int16_t)t->left != last_left) { audio_changes++; last_left = (int16_t)t->left; }
    }
    t->xclk = 0;
    if ((cycle & 3) == 2) t->hclk = 0;
    t->eval();
    cycle++;
}

static void run(uint64_t n) { for (uint64_t i = 0; i < n; i++) step(); }

static void psram_write(uint32_t a, uint8_t v) {
    t->host_we = 1; t->host_addr = a; t->host_data = v; step(); t->host_we = 0;
}
static uint8_t psram_read(uint32_t a) { t->host_addr = a; t->eval(); return t->host_q; }

static bool wait_status(unsigned mask, unsigned want, uint64_t limit) {
    for (uint64_t i = 0; i < limit; i++) { if ((t->state_status & mask) == want) return true; step(); }
    return false;
}

static void wait_frames(int n) {
    int target = frame_no + n;
    uint64_t limit = cycle + (uint64_t)n * 3000000ull;
    while (frame_no < target && cycle < limit) step();
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
    Verilated::commandArgs(argc, argv);
    t = new Vlynx_sim_top;
    outdir = argv[2];
    int frames = argc > 3 ? atoi(argv[3]) : 12;
    int lock = argc > 4 ? atoi(argv[4]) : 1;
    std::vector<uint8_t> bios;
    if (argc > 5) { FILE *b = fopen(argv[5], "rb"); int ch; while (b && (ch = fgetc(b)) != EOF) bios.push_back(ch); if (b) fclose(b); }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("file"); return 2; }
    std::vector<uint8_t> file; int c; while ((c = fgetc(f)) != EOF) file.push_back(c); fclose(f);

    t->xreset = 1; t->reset_n = 0; t->keys = 0; t->quiesce = 0;
    run(64);
    // image: info block, then file at 0x1000 (ROM window base = PSRAM 0x020000)
    const uint32_t base = 0x020000;
    const char magic[4] = {'L', 'X', 'C', '1'};
    for (int i = 0; i < 4; i++) psram_write(base + i, magic[i]);
    uint32_t n = file.size();
    for (int i = 0; i < 4; i++) psram_write(base + 4 + i, (n >> (8 * i)) & 255);
    psram_write(base + 8, (lock ? 2 : 0) | (bios.size() == 512 ? 1 : 0));
    for (size_t i = 0; i < bios.size() && i < 512; i++) psram_write(base + 0x200 + i, bios[i]);
    for (uint32_t i = 0; i < n; i++) psram_write(base + 0x1000 + i, file[i]);
    t->xreset = 0; run(64); t->reset_n = 1;

    // boot
    uint64_t boot_limit = 40000000;
    while (frame_no < 2 && cycle < boot_limit) step();
    if (frame_no < 2) { printf("FAIL no frames after %llu cycles (status=%x init=%d)\n", (unsigned long long)cycle, t->state_status, t->initialized); return 1; }
    wait_frames(frames);
    dump("frame_run.ppm");
    // frame timing
    double xclk_hz = 67108864.0;
    printf("frames=%d\n", frame_no);
    for (size_t i = frame_starts.size() > 8 ? frame_starts.size() - 8 : 1; i < frame_starts.size(); i++)
        printf("panel frame %zu: %.3f ms (%.2f Hz)\n", i, (frame_starts[i] - frame_starts[i - 1]) / xclk_hz * 1000,
               xclk_hz / (frame_starts[i] - frame_starts[i - 1]));
    printf("repeat_pixels=%d of %d, audio_changes=%ld\n", repeat_pixels, frame_pixels, audio_changes);

    // ---- save state round trip ----
    std::vector<uint8_t> state(65 * 1024);
    t->freeze_t = !t->freeze_t;
    if (!wait_status(2, 2, 20000000)) { printf("FAIL freeze status=%x\n", t->state_status); return 1; }
    printf("frozen after save, status=%x\n", t->state_status);
    for (int b = 0; b < 65; b++) {
        t->block_n = b; t->block_seq = b + 1; t->block_t = !t->block_t;
        uint64_t lim = cycle + 2000000;
        while (!(t->block_ready && t->block_ready_seq == b + 1) && cycle < lim) step();
        if (cycle >= lim) { printf("FAIL block %d\n", b); return 1; }
        for (int i = 0; i < 1024; i++) { t->qaddress = i; step(); step(); state[b * 1024 + i] = t->qdata; }
    }
    { std::string sp = outdir + "/state.bin"; FILE *sf = fopen(sp.c_str(), "wb"); fwrite(state.data(), 1, state.size(), sf); fclose(sf); }
    uint32_t sizeword = state[4] | state[5] << 8 | state[6] << 16 | state[7] << 24;
    unsigned frames_saved = state[1024 + 0xF0] | state[1024 + 0xF1] << 8;
    unsigned bar_saved = state[1024 + 0xF2];
    printf("state size word=%08x frames@save=%u bar=%u\n", sizeword, frames_saved, bar_saved);
    if (sizeword != 0x4100) { printf("FAIL size word\n"); return 1; }
    t->release_t = !t->release_t;
    if (!wait_status(2, 0, 1000000)) { printf("FAIL release\n"); return 1; }
    wait_frames(20);
    // read the live frame counter by saving again
    t->freeze_t = !t->freeze_t;
    if (!wait_status(2, 2, 20000000)) { printf("FAIL freeze2\n"); return 1; }
    unsigned frames_later = psram_read(0x220000 + 1024 + 0xF0) | psram_read(0x220000 + 1024 + 0xF1) << 8;
    printf("frames before load=%u\n", frames_later);
    // stage the saved payload (as the MCU does) and load it
    for (int i = 0; i < 65 * 1024; i++) psram_write(0x220000 + i, state[i]);
    t->load_t = !t->load_t;
    if (!wait_status(0x0c, 0x08, 30000000)) { printf("FAIL load status=%x\n", t->state_status); return 1; }
    printf("loaded, status=%x\n", t->state_status);
    t->release_t = !t->release_t;
    if (!wait_status(2, 0, 1000000)) { printf("FAIL release2\n"); return 1; }
    wait_frames(3);
    dump("frame_after_load.ppm");
    // check the counter resumed from the saved value
    t->freeze_t = !t->freeze_t;
    if (!wait_status(2, 2, 20000000)) { printf("FAIL freeze3\n"); return 1; }
    unsigned frames_now = psram_read(0x220000 + 1024 + 0xF0) | psram_read(0x220000 + 1024 + 0xF1) << 8;
    printf("frames after load+3 panel frames=%u (saved %u, before load %u)\n", frames_now, frames_saved, frames_later);
    t->release_t = !t->release_t;
    bool ok = frames_now >= frames_saved && frames_now < frames_saved + 10 && frames_later > frames_saved + 10;
    printf("%s\n", ok ? "PASS" : "FAIL state round trip");
    return ok ? 0 : 1;
}
