// NES board-core simulation: loads a .nes file into a modelled PSRAM (as the MCU would:
// info block + PRG + CHR), runs frames through the real caches/stall path, writes PPM
// screenshots, reports blargg-style results, and optionally round-trips a save state.
//
// usage: nes_test ROM.nes OUTDIR [frames]
// environment:
//   NES_LAT=n         extra random PSRAM latency 0..n cycles (default 24; base latency 6)
//   NES_SEED=n        RNG seed
//   NES_PRESS=f:m,..  from frame f hold buttons m (hex; bit0 A,1 B,2 Select,3 Start,4 Up,...)
//   NES_SHOT=k        also write frame_<n>.ppm every k frames
//   NES_BLARGG=1      stop when $6000 reports a final result (status < 0x80 with DE B0 61)
//   NES_SS=A:K        save state at frame A, run K frames, load, run K frames, compare
//   NES_BATT=file     stage this 8 KiB .sav and import it at reset (battery test)
//   NES_ZP=a,b,..     print these CPU-RAM bytes (hex addresses) at the end
#include "Vnes_sim_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <random>

static Vnes_sim_top *t;
static uint64_t cycle = 0;
static std::vector<uint8_t> psram(1 << 23, 0);
static std::mt19937 rng(1);
static int extra_lat = 24;
static std::string outdir;

// ---- PSRAM model: one request at a time; base latency 6 + random extra; reads deliver one
// 16-bit word per cycle (sometimes with gaps), x_done with the last word (or one cycle after);
// writes: one x_write_next per word, data sampled with it, then x_done.
struct Bus {
    bool active = false, rd = false; uint32_t addr = 0; int words = 0, wait = 0, sent = 0;
    bool done_late = false;
} bus;
static uint64_t bus_bursts = 0, bus_busy_cycles = 0;
static bool pause_test = false; static uint64_t pause_len = 0, pauses = 0;

static void bus_inputs() {
    t->x_read_valid = 0; t->x_done = 0; t->x_write_next = 0;
    if (!bus.active) return;
    bus_busy_cycles++;
    if (bus.wait > 0) { bus.wait--; return; }
    if (bus.sent < bus.words) {
        if ((rng() & 7) == 0) return;   // occasional gap
        uint32_t a = (bus.addr + bus.sent * 2) & ((1 << 23) - 1);
        if (bus.rd) {
            t->x_read_valid = 1;
            t->x_read_data = psram[a] | (psram[a + 1] << 8);
        } else {
            t->x_write_next = 1;   // the word on x_write_data now is consumed with this strobe
            psram[a] = t->x_write_data & 255; psram[a + 1] = t->x_write_data >> 8;
        }
        bus.sent++;
        if (bus.sent == bus.words && !bus.done_late) { t->x_done = 1; bus.active = false; }
    } else { t->x_done = 1; bus.active = false; }
}

static uint64_t frame_no = 0;
static const char *dump_next = nullptr;
static FILE *audio_log = nullptr;
// NES_RTAUDIO: the shell's view of the audio, sampled every 128 xclk (524,288 Hz) in real time
static FILE *rt_log = nullptr;
// NES_ATIME: xclk cycle of every 16th APU sample (atime.u64) and of every played value (aplay.u64)
static FILE *atime_log = nullptr, *aplay_log = nullptr; static int atime_n = 0;
static void dump(const std::string &name);
static uint16_t img[240][256];
static std::vector<uint64_t> frame_hashes;
static uint64_t audio_hash = 1469598103934665603ull, frame_audio_hash = 0;
static std::vector<uint64_t> audio_hashes;
static uint64_t stall_cycles = 0, ce_count = 0;
static std::vector<uint64_t> frame_cycles;

static uint64_t hash_frame() {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 256; x++) { h ^= img[y][x]; h *= 1099511628211ull; }
    return h;
}
static void dump(const std::string &name) {
    std::string p = outdir + "/" + name;
    FILE *f = fopen(p.c_str(), "wb");
    fprintf(f, "P6 256 240 255\n");
    for (int y = 0; y < 240; y++) for (int x = 0; x < 256; x++) {
        uint16_t c = img[y][x];
        fputc((c & 31) * 255 / 31, f); fputc(((c >> 5) & 31) * 255 / 31, f); fputc(((c >> 10) & 31) * 255 / 31, f);
    }
    fclose(f);
}

static FILE *trace = nullptr; static uint64_t trace_left = 0;
static void pstate(const char *w) {
    printf("[%s] cyc=%d sl=%d paused_ctr=%d/%d corepaused=%d div_cpu=%d div_ppu=%d ppu_tick=%d odd=%d pc=%04x cycle=%llu\n", w, t->dbg_cyc, t->dbg_sl,
           t->dbg_cycp, t->dbg_slp, t->dbg_cpaused, t->dbg_divcpu, t->dbg_divppu, t->dbg_ppu_tick, t->dbg_odd, t->dbg_cpu_addr, (unsigned long long)cycle);
}
static int track = 0; static int last_cp = 0;
static void step() {
    if (track && last_cp && !t->dbg_cpaused) { pstate("resumed"); track--; }
    last_cp = t->dbg_cpaused;
    // CPU bus trace: sampled before the edge on which the CPU (cpu_ce) takes it
    if (trace && trace_left && t->dbg_cpu_ce) {
        fprintf(trace, "%04x %c %02x  %d/%d\n", t->dbg_cpu_addr, t->dbg_cpu_rnw ? 'r' : 'W', t->dbg_cpu_data, t->dbg_sl, t->dbg_cyc); trace_left--;
    }
    t->clk = 1;
    t->eval();
    // outputs registered on this edge
    if (t->dbg_ce) ce_count++;
    if (t->dbg_stall) stall_cycles++;
    if (t->pix_strobe) img[t->pix_y][t->pix_x] = t->pix_rgb;
    if (t->audio_strobe) { audio_hash ^= t->audio_sample; audio_hash *= 1099511628211ull; if (audio_log) fwrite(&t->audio_sample, 2, 1, audio_log); }
    if (t->frame_strobe) {
        frame_no++;
        frame_hashes.push_back(hash_frame());
        audio_hashes.push_back(audio_hash);
        audio_hash = 1469598103934665603ull;   // per-frame audio hash
        if (dump_next) { dump(dump_next); dump_next = nullptr; }
        frame_cycles.push_back(cycle);
    }
    // shell quiesce model: random pauses; while paused_now && x_idle nothing may be requested
    if (pause_test) {
        if (!t->pause && (rng() % 200000) == 0) { t->pause = 1; pause_len = 2000 + rng() % 50000; pauses++; }
        else if (t->pause && t->paused_now && t->x_idle && !bus.active) {
            if (pause_len) pause_len--; else t->pause = 0;
        }
        if (t->pause && t->paused_now && t->x_idle && t->x_request) { printf("FAIL: request while quiesced\n"); exit(1); }
    }
    if (t->x_request && !bus.active) {
        bus.active = true; bus.rd = t->x_read; bus.addr = t->x_address; bus.words = (t->x_length + 1) / 2;
        bus.wait = 6 + (extra_lat ? (int)(rng() % (extra_lat + 1)) : 0); bus.sent = 0;
        bus.done_late = (rng() & 1);
        bus_bursts++;
    } else if (t->x_request) { printf("FAIL: request while PSRAM busy at %llu\n", (unsigned long long)cycle); exit(1); }
    if (atime_log && t->audio_strobe && ++atime_n % 16 == 0) fwrite(&cycle, 8, 1, atime_log);
    if (aplay_log && t->dbg_audio_tick) fwrite(&cycle, 8, 1, aplay_log);
    if (rt_log && (cycle & 127) == 0) fwrite(&t->audio_out, 2, 1, rt_log);
    t->clk = 0;
    t->eval();
    bus_inputs();
    t->eval();
    cycle++;
}

static void run_frames(uint64_t n, uint64_t limit_cycles = 0) {
    uint64_t target = frame_no + n;
    uint64_t limit = cycle + (limit_cycles ? limit_cycles : n * 4000000ull + 8000000ull);
    while (frame_no < target && cycle < limit) step();
}

static uint8_t prgram(int a) { t->dbg_addr = a; t->eval(); return t->dbg_prgram; }
static uint8_t cpuram(int a) { t->dbg_addr = a & 0x7ff; t->eval(); return t->dbg_cpuram; }

static bool wait_cond(bool (*f)(), uint64_t limit) {
    for (uint64_t i = 0; i < limit; i++) { if (f()) return true; step(); }
    return false;
}
static int seq = 100;
static std::vector<uint8_t> read_blocks(int first, int count) {
    std::vector<uint8_t> st(count * 1024);
    for (int b = 0; b < count; b++) {
        seq++; t->block_n = first + b; t->block_seq = seq; t->block_t = !t->block_t;
        uint64_t lim = cycle + 4000000;
        while (!(t->block_ready && t->block_ready_seq == seq) && cycle < lim) step();
        if (cycle >= lim) { printf("FAIL block %d\n", first + b); exit(1); }
        for (int i = 0; i < 1024; i++) { t->qaddress = i; step(); step(); st[b * 1024 + i] = t->qdata; }
    }
    return st;
}

// press schedule
static std::vector<std::pair<uint64_t, int>> presses;
static void apply_buttons() {
    int m = 0;
    for (auto &p : presses) if (frame_no >= p.first) m = p.second;
    t->buttons = m;
}

static const uint32_t ROM_BASE = 0x020000, STAGE = 0x3F0000, IMPORT = 0x3F8000;
static const int STATE_BLOCKS = 21;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: nes_test ROM.nes OUTDIR [frames]\n"); return 2; }
    Verilated::commandArgs(argc, argv);
    t = new Vnes_sim_top;
    outdir = argv[2];
    int frames = argc > 3 ? atoi(argv[3]) : 60;
    if (getenv("NES_LAT")) extra_lat = atoi(getenv("NES_LAT"));
    if (getenv("NES_SEED")) rng.seed(atoi(getenv("NES_SEED")));
    int shot = getenv("NES_SHOT") ? atoi(getenv("NES_SHOT")) : 0;
    if (getenv("NES_PRESS")) {
        std::string s = getenv("NES_PRESS"); size_t i = 0;
        while (i < s.size()) {
            size_t c = s.find(':', i), e = s.find(',', i); if (e == std::string::npos) e = s.size();
            presses.push_back({strtoull(s.substr(i, c - i).c_str(), 0, 10), (int)strtol(s.substr(c + 1, e - c - 1).c_str(), 0, 16)});
            i = e + 1;
        }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    std::vector<uint8_t> file; int ch; while ((ch = fgetc(f)) != EOF) file.push_back(ch); fclose(f);
    if (file.size() < 16 || memcmp(file.data(), "NES\x1a", 4)) { printf("FAIL not an iNES file\n"); return 2; }
    // ---- header parsing, as the MCU will do it
    const uint8_t *h = file.data();
    bool nes20 = (h[7] & 0x0C) == 0x08;
    bool dirty = !nes20 && (h[12] | h[13] | h[14] | h[15]);
    unsigned mapper = (h[6] >> 4) | (dirty ? 0 : (h[7] & 0xF0)) | (nes20 ? (h[8] & 0x0F) << 8 : 0);
    unsigned sub = nes20 ? h[8] >> 4 : 0;
    unsigned prg_kib = (h[4] | (nes20 ? (h[9] & 0x0F) << 8 : 0)) * 16;
    unsigned chr_kib = (h[5] | (nes20 ? (h[9] & 0xF0) << 4 : 0)) * 8;
    bool trainer = h[6] & 4;
    size_t off = 16 + (trainer ? 512 : 0);
    if (off + (prg_kib + chr_kib) * 1024 > file.size()) { printf("FAIL truncated file\n"); return 2; }
    uint8_t info[16] = {'N', 'E', 'S', '1'};
    info[4] = mapper & 255; info[5] = mapper >> 8; info[6] = sub;
    info[7] = (h[6] & 1) | ((h[6] >> 3) & 1) << 1 | ((h[6] >> 1) & 1) << 2 | (chr_kib == 0) << 3 | nes20 << 4;
    info[8] = prg_kib & 255; info[9] = prg_kib >> 8; info[10] = chr_kib & 255; info[11] = chr_kib >> 8;
    // iNES 1.0 does not give the PRG RAM size; like most emulators (and unlike MiSTer, which
    // maps no $6000 RAM on Mapper28 boards then) assume 8 KiB: shift count 7 (64 << 7).
    info[12] = nes20 ? h[10] : 0x07; info[13] = nes20 ? h[11] : 0;
    printf("rom: mapper %u.%u PRG %u KiB CHR %u KiB flags %02x\n", mapper, sub, prg_kib, chr_kib, info[7]);
    memcpy(&psram[ROM_BASE], info, 16);
    memcpy(&psram[ROM_BASE + 0x1000], &file[off], (prg_kib + chr_kib) * 1024);
    bool batt = getenv("NES_BATT") != nullptr;
    if (batt) {
        FILE *b = fopen(getenv("NES_BATT"), "rb");
        if (!b || fread(&psram[ROM_BASE + IMPORT], 1, 8192, b) != 8192) { printf("FAIL battery file\n"); return 2; }
        fclose(b);
    }

    if (getenv("NES_TRACE")) { trace = fopen((outdir + "/trace.txt").c_str(), "w"); trace_left = strtoull(getenv("NES_TRACE"), 0, 10); }
    pause_test = getenv("NES_PAUSETEST") != nullptr;
    if (getenv("NES_ALOGALL")) audio_log = fopen((outdir + "/audio_all.raw").c_str(), "wb");
    if (getenv("NES_ATIME")) { atime_log = fopen((outdir + "/atime.u64").c_str(), "wb"); aplay_log = fopen((outdir + "/aplay.u64").c_str(), "wb"); }
    if (getenv("NES_RTAUDIO")) rt_log = fopen((outdir + "/audio_rt.raw").c_str(), "wb");
    t->video_resume_ok = 1; t->cart_dirty_clear = 0;
    t->reset_n = 0; t->pause = 0; t->buttons = 0; t->cart_import = batt;
    for (int i = 0; i < 64; i++) step();
    // NES_GG="AAAA:VV[:CC],...": Game Genie codes (CPU address, replacement, optional compare),
    // loaded while the core is in reset the way gg_cheat_loader does: a clear pulse, then one
    // load pulse (bit 128) per code on the free-running clock.
    t->gg_clear = 1; step(); t->gg_clear = 0; step();
    if (getenv("NES_GG")) {
        std::string s = getenv("NES_GG"); size_t i = 0;
        while (i < s.size()) {
            size_t e = s.find(',', i); if (e == std::string::npos) e = s.size();
            unsigned a = 0, v = 0, cmp = 0; int n = sscanf(s.substr(i, e - i).c_str(), "%x:%x:%x", &a, &v, &cmp);
            for (int w = 0; w < 5; w++) t->gg_code[w] = 0;
            t->gg_code[0] = v & 0xFF; t->gg_code[1] = cmp & 0xFF; t->gg_code[2] = a & 0xFFFF;
            t->gg_code[3] = n >= 3 ? 1 : 0; t->gg_code[4] = 1;
            step(); t->gg_code[4] = 0; step(); step();
            printf("game genie %04x=%02x", a, v); if (n >= 3) printf(" if %02x", cmp); printf("\n");
            i = e + 1;
        }
    }
    t->reset_n = 1;
    bool blargg = getenv("NES_BLARGG") != nullptr;
    bool f8mode = getenv("NES_F8") != nullptr;   // 2005-era blargg tests: result byte at $F8 (1 = pass)
    int stable = 0; bool f8done = false;
    int resaddr = getenv("NES_RESADDR") ? strtol(getenv("NES_RESADDR"), 0, 16) : 0xF8;
    int result = -1;
    std::string text;
    uint64_t last_shot = 0;
    const char *ss = getenv("NES_SS");
    int ss_a = 0, ss_k = 0;
    if (ss) sscanf(ss, "%d:%d", &ss_a, &ss_k);
    uint64_t target = ss ? (uint64_t)ss_a : (uint64_t)frames;
    while (frame_no < target) {
        apply_buttons();
        uint64_t before = frame_no;
        run_frames(1);
        if (frame_no == before) {
            printf("FAIL no frame (cycle %llu running=%d bad=%d fault=%d)\n", (unsigned long long)cycle,
                   t->running, t->bad_image, t->rom_fault);
            dump("stuck.ppm");
            return 1;
        }
        if (getenv("NES_TRACE_FROM") && frame_no == strtoull(getenv("NES_TRACE_FROM"), 0, 10)) {
            trace = fopen((outdir + "/trace_from.txt").c_str(), "w"); trace_left = getenv("NES_TRACE_N") ? strtoull(getenv("NES_TRACE_N"), 0, 10) : 200000; }
        if (shot && frame_no - last_shot >= (uint64_t)shot) { last_shot = frame_no; char n[64]; snprintf(n, 64, "frame_%04llu.ppm", (unsigned long long)frame_no); dump(n); }
        if (f8mode && cpuram(resaddr) != 0 && frame_hashes.size() > 2) {
            if (frame_hashes.back() == frame_hashes[frame_hashes.size() - 2]) stable++; else stable = 0;
            if (stable >= 120) { result = cpuram(resaddr); f8done = true; break; }
        }
        if (blargg && prgram(1) == 0xDE && prgram(2) == 0xB0 && prgram(3) == 0x61 && prgram(0) < 0x80) {
            result = prgram(0);
            // a few more frames so the screen shows the final message
            run_frames(4);
            break;
        }
    }
    dump("last.ppm");
    if (f8mode) printf("f8 result=%d (%s)\n", cpuram(resaddr), f8done ? "final, screen stable" : "NOT final");
    if (!f8mode && prgram(1) == 0xDE && prgram(2) == 0xB0 && prgram(3) == 0x61) {
        for (int a = 4; a < 0x1000; a++) { uint8_t c = prgram(a); if (!c) break; text += (char)c; }
        if (result < 0) result = prgram(0);
    }
    double hz = 0;
    if (frame_cycles.size() > 4) hz = 67108864.0 * (frame_cycles.size() - 2) / (frame_cycles.back() - frame_cycles[1]);
    printf("frames=%llu cycles=%llu avg %.3f Hz  nes_edges=%llu stall_cycles=%llu bursts=%llu bus_busy=%.1f%%\n",
           (unsigned long long)frame_no, (unsigned long long)cycle, hz, (unsigned long long)ce_count,
           (unsigned long long)stall_cycles, (unsigned long long)bus_bursts, 100.0 * bus_busy_cycles / cycle);
    {
        uint64_t hv = 1469598103934665603ull, ha = hv;
        for (auto x : frame_hashes) { hv ^= x; hv *= 1099511628211ull; }
        for (auto x : audio_hashes) { ha ^= x; ha *= 1099511628211ull; }
        printf("hash video=%016llx audio=%016llx pauses=%llu\n", (unsigned long long)hv, (unsigned long long)ha, (unsigned long long)pauses);
    }
    if (getenv("NES_NT")) {
        for (int r = 0; r < 30; r++) { std::string l; for (int c = 0; c < 32; c++) { t->dbg_addr = r * 32 + c; t->eval(); uint8_t v = t->dbg_ciram; l += (v >= 32 && v < 127) ? (char)v : '.'; } printf("NT %2d |%s|\n", r, l.c_str()); }
    }
    if (getenv("NES_ZP")) {
        std::string s = getenv("NES_ZP"); size_t i = 0;
        while (i < s.size()) { size_t e = s.find(',', i); if (e == std::string::npos) e = s.size();
            int a = strtol(s.substr(i, e - i).c_str(), 0, 16); printf("ram[%04x]=%02x\n", a, cpuram(a)); i = e + 1; }
    }
    if (blargg && result < 0) {
        // Fallback for boards whose $6000 RAM is disabled (MMC3 powers up with PRG RAM off in
        // MiSTer's mapper): read the result text the shell printed into the nametable (CIRAM).
        std::string nt;
        for (int a = 0; a < 2048; a++) { t->dbg_addr = a; t->eval(); uint8_t v = t->dbg_ciram; nt += (v >= 32 && v < 127) ? (char)v : ' '; }
        size_t p = nt.find("Passed"), q = nt.find("Failed");
        if (p != std::string::npos || q != std::string::npos) {
            result = (p != std::string::npos && q == std::string::npos) ? 0 : 1;
            std::string compact; char last = 0;
            for (char c : nt) { if (c == ' ' && last == ' ') continue; compact += c; last = c; }
            text = "(nametable) " + compact.substr(0, 200);
        }
    }
    if (result >= 0) {
        for (auto &c : text) if (c == '\n') c = '|';
        printf("blargg result=%d text=\"%s\"\n", result, text.c_str());
    }
    if (t->bad_image) { printf("FAIL bad image\n"); return 1; }
    if (t->rom_fault) { printf("FAIL rom fault\n"); return 1; }

    if (ss) {
        // ---- save-state round trip
        auto frozen = []() { return (bool)t->st_frozen && !t->st_busy; };
        auto released = []() { return !t->st_frozen && !t->st_busy && !t->paused_now; };
        t->freeze_t = !t->freeze_t;
        if (!wait_cond(frozen, 40000000)) { printf("FAIL freeze (busy=%d fault=%d)\n", t->st_busy, t->st_fault); return 1; }
        if (t->st_fault) { printf("FAIL freeze fault\n"); return 1; }
        uint64_t audio_at_save = audio_hash;
        pstate("frozen1");
        std::vector<uint8_t> st = read_blocks(0, STATE_BLOCKS);
        { FILE *sf = fopen((outdir + "/state.bin").c_str(), "wb"); fwrite(st.data(), 1, st.size(), sf); fclose(sf); }
        // NES_LOADIMG=file: load this image (a .state file from the card, 64-byte header skipped) instead
        if (getenv("NES_LOADIMG")) {
            FILE *lf = fopen(getenv("NES_LOADIMG"), "rb"); std::vector<uint8_t> f(40000);
            size_t n = lf ? fread(f.data(), 1, f.size(), lf) : 0; if (lf) fclose(lf);
            size_t off = n == STATE_BLOCKS * 1024 + 64 ? 64 : 0;
            if (n - off != STATE_BLOCKS * 1024) { printf("FAIL image size %zu\n", n); return 1; }
            memcpy(st.data(), f.data() + off, STATE_BLOCKS * 1024);
            printf("loading image %s\n", getenv("NES_LOADIMG"));
        }
        uint32_t sizeword = st[4] | st[5] << 8 | st[6] << 16 | st[7] << 24;
        uint64_t cpu = 0; for (int i = 7; i >= 0; i--) cpu = cpu << 8 | st[8 + i];
        printf("state: size word %08x, regs[0] %016llx\n", sizeword, (unsigned long long)cpu);
        if (sizeword != 0x1500) { printf("FAIL size word\n"); return 1; }
        size_t f0 = frame_hashes.size();
        track = 1;
        if (getenv("NES_TRACE_SS")) { trace = fopen((outdir + "/trace_a.txt").c_str(), "w"); trace_left = strtoull(getenv("NES_TRACE_SS"), 0, 10); }
        t->release_t = !t->release_t;
        dump_next = "ss_a_first.ppm";
        if (getenv("NES_ALOG")) audio_log = fopen((outdir + "/audio_a.raw").c_str(), "wb");
        run_frames(ss_k);
        std::vector<uint64_t> fa(frame_hashes.begin() + f0, frame_hashes.end());
        std::vector<uint64_t> aa(audio_hashes.begin() + f0, audio_hashes.end());
        dump("ss_after_save.ppm");
        // second freeze, stage the saved image as the MCU does, load
        t->freeze_t = !t->freeze_t;
        if (!wait_cond(frozen, 40000000)) { printf("FAIL freeze2\n"); return 1; }
        pstate("frozen2");
        std::vector<uint8_t> st_a2 = read_blocks(0, STATE_BLOCKS);
        for (int i = 0; i < STATE_BLOCKS * 1024; i++) psram[ROM_BASE + STAGE + i] = st[i];
        t->load_t = !t->load_t;
        auto loaded = []() { return !t->st_busy && ((bool)t->st_loaded || (bool)t->st_fault); };
        for (int i = 0; i < 8; i++) step();
        if (!wait_cond(loaded, 40000000) || !t->st_loaded) { printf("FAIL load (fault=%d)\n", t->st_fault); return 1; }
        // the audio hash chain restarts from the value it had at the save
        audio_hash = audio_at_save;
        pstate("loaded"); track = 1;
        if (getenv("NES_TRACE_SS")) { fclose(trace); trace = fopen((outdir + "/trace_b.txt").c_str(), "w"); trace_left = strtoull(getenv("NES_TRACE_SS"), 0, 10); }
        // keep the image of the loaded state's frame comparable: reset the frame buffer view
        size_t f1 = frame_hashes.size();
        t->release_t = !t->release_t;
        dump_next = "ss_b_first.ppm";
        if (audio_log) { fclose(audio_log); audio_log = fopen((outdir + "/audio_b.raw").c_str(), "wb"); }
        run_frames(ss_k);
        if (audio_log) { fclose(audio_log); audio_log = nullptr; }
        std::vector<uint64_t> fb(frame_hashes.begin() + f1, frame_hashes.end());
        std::vector<uint64_t> ab(audio_hashes.begin() + f1, audio_hashes.end());
        dump("ss_after_load.ppm");
        // freeze again and compare the whole state with the one at the end of run A
        t->freeze_t = !t->freeze_t;
        if (!wait_cond(frozen, 40000000)) { printf("FAIL freeze3\n"); return 1; }
        std::vector<uint8_t> st_b2 = read_blocks(0, STATE_BLOCKS);
        { int nd = 0; for (int i = 8; i < STATE_BLOCKS * 1024; i++) if (st_a2[i] != st_b2[i]) {
            if (nd++ < 24) printf("state diff at 0x%04x (%s %d): %02x vs %02x\n", i, i < 0x208 ? "reg" : "mem", i < 0x208 ? (i - 8) / 8 : i, st_a2[i], st_b2[i]); }
          printf("state images after K frames: %d bytes differ\n", nd); }
        t->release_t = !t->release_t;
        int same = 0, asame = 0, changes = 0;
        for (size_t i = 0; i < fa.size() && i < fb.size(); i++) { same += fa[i] == fb[i]; asame += aa[i] == ab[i]; }
        for (size_t i = 1; i < fa.size(); i++) changes += fa[i] != fa[i - 1];
        printf("save-state: %d/%zu frames identical, %d/%zu audio frames identical (%d frame changes in the run)\n",
               same, fa.size(), asame, aa.size(), changes);
        bool ok = fa.size() == (size_t)ss_k && fb.size() == (size_t)ss_k && same == ss_k && asame == ss_k;
        printf("%s\n", ok ? "PASS save-state round trip" : "FAIL save-state round trip");
        if (!ok) {
            for (size_t i = 0; i < fa.size() && i < fb.size(); i++)
                if (fa[i] != fb[i] || aa[i] != ab[i]) printf("difference at frame %zu (video %d audio %d)\n", i, fa[i] != fb[i], aa[i] != ab[i]);
            return 1;
        }
    }
    if (batt) {
        printf("battery: imported=%d dirty=%d written=%02x\n", t->cart_imported, t->cart_dirty, t->cart_written);
        t->cart_dirty_clear = 1; step(); t->cart_dirty_clear = 0;
        std::vector<uint8_t> live = read_blocks(64, 8);
        FILE *sf = fopen((outdir + "/battery.sav").c_str(), "wb"); fwrite(live.data(), 1, live.size(), sf); fclose(sf);
        printf("battery: after cart_dirty_clear dirty=%d snapshot=%02x\n", t->cart_dirty, t->cart_snapshot);
    }
    t->final();
    return 0;
}
