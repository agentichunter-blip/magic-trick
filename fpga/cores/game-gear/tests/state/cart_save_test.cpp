// Game Gear battery save: import at start, live reads, dirty flag, and two
// separate 16 KiB cartridge RAM banks. A small test program copies the first
// byte of each bank to work RAM as its first instructions, then writes one
// byte into bank 1:
//   di; ld sp,$DFF0; ld a,$08; ld ($FFFC),a      ; cartridge RAM bank 0
//   ld a,($8000); ld ($C000),a
//   ld a,$0C; ld ($FFFC),a                        ; bank 1
//   ld a,($8000); ld ($C001),a; inc a; ld ($8001),a; jr $
// With the import armed, the program must see the staged save from its first
// read; the save must then be readable while the game runs, and the game's
// write must set (and a block 9 read clear) the dirty flag.
#include "Vgg_ss_sim_top.h"
#include "verilated.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
static void check(bool x, const char *s) { if (!x) throw std::runtime_error(s); }
enum { ROM_BASE = 0x020000, STATE_BASE = 0x410000, CART = 9 * 1024, CART_SIZE = 32 * 1024 };

struct Sim {
    Vgg_ss_sim_top d;
    std::vector<uint8_t> psram = std::vector<uint8_t>(8u << 20, 0);
    struct { bool active = false; uint32_t addr = 0; int delay = 0, word = 0; } bus;
    uint16_t sequence = 1;
    Sim() {
        static const uint8_t program[] = {0xF3, 0x31, 0xF0, 0xDF, 0x3E, 0x08, 0x32, 0xFC, 0xFF,
            0x3A, 0x00, 0x80, 0x32, 0x00, 0xC0, 0x3E, 0x0C, 0x32, 0xFC, 0xFF,
            0x3A, 0x00, 0x80, 0x32, 0x01, 0xC0, 0x3C, 0x32, 0x01, 0x80, 0x18, 0xFE};
        std::vector<uint8_t> rom(32 * 1024, 0);
        std::copy(std::begin(program), std::end(program), rom.begin());
        for (uint32_t a = 0; a < 0x400000; a++) psram[ROM_BASE + a] = rom[a % rom.size()];
        d.clk = 0; d.reset_n = 0; d.buttons = 0; d.cart_import = 0;
        d.freeze_t = d.release_t = d.load_t = d.block_t = 0;
        d.block_number = 0; d.block_seq = 0; d.qaddress = 0;
        d.bus_done = d.bus_valid = 0; d.bus_data = 0;
        d.eval();
    }
    void tick() {
        if (bus.active && bus.delay == 0) {
            const uint32_t a = bus.addr + 2 * bus.word;
            d.bus_valid = 1; d.bus_data = psram[a] | (psram[a + 1] << 8); d.bus_done = bus.word == 31;
        } else { d.bus_valid = 0; d.bus_done = 0; }
        d.clk = 0; d.eval(); d.clk = 1; d.eval();
        if (bus.active) { if (bus.delay) bus.delay--; else if (++bus.word == 32) bus.active = false; }
        if (!bus.active && d.bus_request) { bus.active = true; bus.addr = d.bus_address; bus.delay = 6; bus.word = 0; }
    }
    void run(uint64_t n) { for (uint64_t i = 0; i < n; i++) tick(); }
    template <class F> void until(F f, uint64_t limit, const char *what) {
        for (uint64_t i = 0; i < limit && !f(); i++) tick();
        check(f(), what);
    }
    void boot(bool import) {
        d.cart_import = import; d.reset_n = 0; run(200); d.reset_n = 1;
        if (import) {
            check(d.frozen, "core did not start frozen");
            until([&] { return bool(d.imported) && !d.frozen && !d.busy; }, 400000, "import timeout");
            check(!d.fault, "import fault");
        } else check(!d.frozen, "core frozen without import");
        d.cart_import = 0;
    }
    std::vector<uint8_t> block(unsigned b) {
        d.block_number = b; d.block_seq = ++sequence; d.block_t ^= 1;
        until([&] { return d.block_ready && d.block_ready_seq == sequence; }, 200000, "block timeout");
        std::vector<uint8_t> v(1024);
        for (unsigned i = 0; i < 1024; i++) { d.qaddress = i; tick(); v[i] = d.qdata; }
        return v;
    }
    std::vector<uint8_t> cart() {
        std::vector<uint8_t> v;
        for (unsigned b = 9; b <= 40; b++) { auto x = block(b); v.insert(v.end(), x.begin(), x.end()); }
        return v;
    }
    std::vector<uint8_t> wram_frozen() {
        d.freeze_t ^= 1;
        until([&] { return bool(d.frozen); }, 4000000, "freeze timeout");
        until([&] { return !d.busy; }, 1000, "freeze busy");
        auto w = block(1);
        d.release_t ^= 1;
        until([&] { return !d.frozen; }, 1000000, "release timeout");
        return w;
    }
};

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    Sim s;
    std::vector<uint8_t> save(CART_SIZE);
    for (unsigned i = 0; i < CART_SIZE; i++) save[i] = uint8_t(i * 29 + (i >> 8) * 7 + 3);
    save[0] = 0x5A; save[0x4000] = 0xA7;   // first byte of each bank
    std::copy(save.begin(), save.end(), s.psram.begin() + STATE_BASE + CART);

    // Import, then the program's first reads see the save.
    s.boot(true);
    s.run(200000);
    check(s.d.cart_dirty, "game write did not set dirty");
    check(s.d.cart_written == (1u << 16), "write not tracked to its 1 KiB block (0x4001 -> 16)");
    auto c = s.cart();                 // live read: game keeps running
    check(!s.d.cart_dirty, "block 9 read did not clear dirty");
    check(s.d.cart_written == 0 && s.d.cart_snapshot == (1u << 16), "copy start did not move written blocks to the snapshot");
    std::vector<uint8_t> expect = save; expect[0x4001] = 0xA8;   // written in bank 1
    unsigned diffs = 0;
    for (unsigned i = 0; i < CART_SIZE; i++) if (c[i] != expect[i]) { if (diffs < 8) std::printf("cart diff %04x: %02x %02x\n", i, c[i], expect[i]); diffs++; }
    check(diffs == 0, "live cartridge RAM read differs");
    auto w = s.wram_frozen();
    std::printf("wram c000=%02x c001=%02x\n", w[0], w[1]);
    check(w[0] == 0x5A && w[1] == 0xA7, "program did not read both imported banks at start");
    s.run(200000);
    check(!s.d.cart_dirty, "dirty without a game write");

    // Without the import, the core starts at once and the RAM is untouched by
    // the engine (the program's own write lands again, nothing else changes).
    s.boot(false);
    s.run(200000);
    check(s.d.cart_dirty && !s.d.imported && s.d.cart_written == (1u << 16), "plain start");
    c = s.cart();
    check(c == expect, "plain start changed cartridge RAM unexpectedly");
    std::printf("PASS GG battery save: frozen start import, first-instruction visibility, two 16 KiB banks, live read, dirty set/clear, per-block write tracking\n");
    return 0;
}
