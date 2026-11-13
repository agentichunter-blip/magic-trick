// Game Gear save-state engine test: save through the 1 KiB block buffer, restore
// from a payload staged in modelled PSRAM, and require the restored game to
// replay the saved game's CPU bus, video and audio exactly.
#include "Vgg_ss_sim_top.h"
#include "verilated.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
static void check(bool x, const char *s) { if (!x) throw std::runtime_error(s); }
enum { BLOCKS = 41, PAYLOAD = BLOCKS * 1024, ROM_BASE = 0x020000, STATE_BASE = 0x410000 };
using Payload = std::vector<uint8_t>;
using Sample = std::array<uint32_t, 12>;

struct Sim {
    Vgg_ss_sim_top d;
    std::vector<uint8_t> psram = std::vector<uint8_t>(8u << 20, 0);
    uint64_t cycles = 0;
    uint16_t sequence = 1;
    struct { bool active = false; uint32_t addr = 0; int delay = 0, word = 0; } bus;
    unsigned buttons = 0, px = 0, py = 0;
    std::array<uint16_t, 160 * 144> screen{};
    Sim(const char *path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), {});
        check(!rom.empty(), "ROM missing");
        // Mirror the ROM through the 4 MiB ROM window like a power-of-two image.
        for (uint32_t a = 0; a < 0x400000; a++) psram[ROM_BASE + a] = rom[a % rom.size()];
        d.clk = 0; d.reset_n = 0; d.buttons = 0;
        d.freeze_t = d.release_t = d.load_t = d.block_t = 0;
        d.block_number = 0; d.block_seq = 0; d.qaddress = 0;
        d.bus_done = d.bus_valid = 0; d.bus_data = 0;
        d.eval();
        for (int i = 0; i < 200; i++) tick();
        d.reset_n = 1;
    }
    void tick() {
        // PSRAM model: 64-byte bursts, 6 cycles latency, done with the last word.
        if (bus.active && bus.delay == 0) {
            const uint32_t a = bus.addr + 2 * bus.word;
            d.bus_valid = 1; d.bus_data = psram[a] | (psram[a + 1] << 8); d.bus_done = bus.word == 31;
        } else { d.bus_valid = 0; d.bus_done = 0; }
        d.buttons = buttons;
        d.clk = 0; d.eval();
        if (d.ce_pix) {
            if (py >= 24 && py < 168 && px >= 48 && px < 208) screen[(py - 24) * 160 + px - 48] = d.color;
            px = d.x; py = d.y;
        }
        d.clk = 1; d.eval(); cycles++;
        if (bus.active) { if (bus.delay) bus.delay--; else if (++bus.word == 32) bus.active = false; }
        if (!bus.active && d.bus_request) { bus.active = true; bus.addr = d.bus_address; bus.delay = 6; bus.word = 0; }
    }
    void screenshot(const char *path) {
        std::ofstream f(path, std::ios::binary); f << "P6\n160 144\n255\n";
        for (auto c : screen) { char rgb[3] = {char((c & 15) * 17), char(((c >> 4) & 15) * 17), char(((c >> 8) & 15) * 17)}; f.write(rgb, 3); }
    }
    void run(uint64_t n) { for (uint64_t i = 0; i < n; i++) tick(); }
    template <class F> void until(F f, uint64_t limit, const char *what) {
        for (uint64_t i = 0; i < limit && !f(); i++) tick();
        check(f(), what);
    }
    void freeze() {
        d.freeze_t ^= 1;
        until([&] { return bool(d.frozen); }, 4000000, "freeze timeout");
        until([&] { return !d.busy; }, 1000, "freeze busy");
    }
    void release() {
        d.release_t ^= 1;
        until([&] { return !d.frozen; }, 1000, "release timeout");
    }
    Payload save() {
        check(d.frozen, "save without freeze");
        Payload p(PAYLOAD);
        for (unsigned b = 0; b < BLOCKS; b++) {
            d.block_number = b; d.block_seq = ++sequence; d.block_t ^= 1;
            until([&] { return d.block_ready && d.block_ready_seq == sequence; }, 200000, "block timeout");
            for (unsigned i = 0; i < 1024; i++) { d.qaddress = i; tick(); p[b * 1024 + i] = d.qdata; }
        }
        check(p[4] == 0x67 && p[5] == 0x67 && p[6] == 1, "payload signature");
        return p;
    }
    void load(const Payload &p, bool expect_fault = false) {
        check(d.frozen, "load without freeze");
        std::copy(p.begin(), p.end(), psram.begin() + STATE_BASE);
        d.load_t ^= 1;
        if (expect_fault) { until([&] { return bool(d.fault); }, 200000, "bad payload accepted"); return; }
        for (uint64_t i = 0; i < 20000000 && !(d.loaded && !d.busy); i++) tick();
        if (!(d.loaded && !d.busy)) std::printf("load stuck: state=%u position=%u fault=%u rom_fault=%u busy=%u\n", d.dbg_state, d.dbg_position, d.fault, d.dbg_rom_fault, d.busy);
        check(d.loaded && !d.busy, "load timeout");
        check(!d.fault, "load fault");
    }
    std::vector<Sample> trace(uint64_t n) {
        std::vector<Sample> v; v.reserve(n);
        for (uint64_t i = 0; i < n; i++) {
            buttons = (i > n / 3 && i < 2 * n / 3) ? 24 : 8;
            tick();
            v.push_back({d.rom_a, d.rom_rd, d.x, d.y, d.ce_pix, d.color, d.audioL, d.audioR,
                         d.ram_we, d.ram_a, d.ram_we ? uint32_t(d.ram_d) : 0u, d.rom_ready});
        }
        return v;
    }
};

static unsigned diff(const Payload &a, const Payload &b, const char *name) {
    unsigned n = 0;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i]) { if (n < 8) std::printf("%s diff at 0x%05zx: %02x %02x\n", name, i, a[i], b[i]); n++; }
    return n;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    check(argc >= 2, "ROM required");
    Sim s(argv[1]);
    const uint64_t warm = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 6000000;
    const uint64_t length = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2500000;
    // Reach gameplay: press Start twice, then hold right.
    for (uint64_t i = 0; i < warm; i++) {
        // Hold each Start press for about four frames so the game polls it.
        const uint64_t p1 = warm * 6 / 10, p2 = warm * 75 / 100;
        s.buttons = (i > p1 && i < p1 + 4500000) || (i > p2 && i < p2 + 4500000) ? 64 : (i > warm * 85 / 100 ? 8 : 0);
        s.tick();
    }
    s.freeze();
    s.screenshot("captured.ppm");
    Payload a = s.save();
    check(diff(a, s.save(), "frozen re-read") == 0, "frozen state changed while reading");
    std::printf("captured cycle=%llu pc=%02x%02x\n", (unsigned long long)s.cycles, a[8 + 5], a[8 + 4]);
    if (const char *out = std::getenv("GG_SS_OUT")) { std::ofstream f(out, std::ios::binary); f.write((const char *)a.data(), a.size()); }

    s.release();
    auto first = s.trace(length);
    s.screenshot("saved-run.ppm");
    s.freeze(); Payload end1 = s.save();

    // Move on, then scramble every region and register with a valid-looking payload.
    s.release(); s.buttons = 64; s.run(2000000); s.buttons = 0;
    s.freeze();
    Payload junk(PAYLOAD);
    for (size_t i = 0; i < junk.size(); i++) junk[i] = uint8_t(i * 37 + (i >> 8) * 11 + 5);
    junk[4] = junk[5] = 0x67;
    s.load(junk);
    check(diff(s.save(), a, "junk") > 30000, "junk load did not reach the memories");
    // A payload without the Game Gear signature is refused before any write.
    Payload bad = a; bad[5] = 0x00;
    Payload before = s.save();
    s.load(bad, true);
    check(diff(s.save(), before, "after refusal") == 0, "refused payload changed state");
    s.load(a);
    check(diff(s.save(), a, "round trip") == 0, "restored state differs from the saved payload");
    s.release();
    s.screenshot("before-restore-run.ppm");
    auto second = s.trace(length);
    s.screenshot("restored-run.ppm");
    s.freeze(); Payload end2 = s.save();

    unsigned mismatches = 0; size_t first_diff = 0;
    for (size_t i = 0; i < first.size(); i++)
        if (first[i] != second[i]) {
            if (!mismatches) first_diff = i;
            if (mismatches < 8) {
                std::printf("trace diff cycle=%zu", i);
                for (unsigned j = 0; j < first[i].size(); j++)
                    if (first[i][j] != second[i][j]) std::printf(" f%u:%u/%u", j, first[i][j], second[i][j]);
                std::printf("\n");
            }
            mismatches++;
        }
    std::printf("trace cycles=%zu mismatches=%u first_diff=%zu\n", first.size(), mismatches, first_diff);
    const unsigned end_diffs = diff(end1, end2, "end state");
    check(mismatches == 0, "restored replay differs");
    check(end_diffs == 0, "end states differ");
    std::printf("PASS GG save-state engine: block save, PSRAM-staged load, signature refusal, exact %llu-cycle replay\n",
                (unsigned long long)length);
    return 0;
}
