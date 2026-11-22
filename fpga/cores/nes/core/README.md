# NES core for the ModRetro Chromatic (GW5A-25) — simulation-validated board module

MiSTer NES core (`NES_MiSTer/`, revision in `REVISION`) adapted to the Chromatic: everything runs
from xclk (67.108864 MHz), all RAM is on chip, PRG/CHR ROM come from PSRAM through two line caches,
save states use MiSTer's engine with a 21 KiB image staged in PSRAM. Nothing here has touched hardware.
`FEASIBILITY.md` is the earlier synthesis probe.

## Files

| Path | What |
|---|---|
| `rtl/nes_board_core.sv` | **the module to instantiate** (top of this work) |
| `rtl/nes_rom_cache.sv` | PSRAM line cache (used twice: PRG, CHR) |
| `rtl/nes_state_engine.sv` | MCU save-state / battery transfer engine (lynx_state_engine command set) |
| `rtl/nes_savestates.sv` | MiSTer `savestates.vhd` ported to SV, region table reduced |
| `rtl/nes_ss_support.sv` | SV `eReg_SavestateV` and `statemanager` (no rewind) |
| `rtl/nes_clock_gate.sv` | DCE wrapper (Gowin build). `sim/nes_clock_gate_model.sv` is its sim model |
| `rtl/t65_nes.vhd`, `rtl/t65_wrap.sv` | T65 as 2A03 (VHDL wrapper for GHDL; SV shim named `T65`) |
| `rtl/gowin_dpb_snapshot_model.sv` | inferred stand-in for the shell's `gowin_dpb_snapshot` (sim + standalone synth only; **leave out** of the integrated build, the shell has the real one) |
| `rtl/gen/` | **generated** core sources — never edit; regenerate with `tools/regen.sh` |
| `tools/regen.sh` | `syn/tools/prep.py` (probe's Gowin fixes) → `tools/gen_rtl.py` → GHDL `--synth` of T65 |
| `sim/` | Verilator harness (`run.sh`, `nes_test.cpp`, `nes_sim_top.sv`), suite runner `run_tests.py`, OAM A/B `ab_sprites.sh` |
| `syn/board/` | Gowin synthesis-only project of `nes_board_core` (`build.tcl`, `board.sdc`) |
| `testroms/nes-test-roms/` | downloaded test ROMs (christopherpow/nes-test-roms; data only) |
| `testroms/holy-mapperel/` | Holy Mapperel 0.02 binaries (pinobatch, zlib licence): mapper/bank/RAM checks |

Gowin file list for the integration (order matters only for the package):
`rtl/gen/regs_savestates.sv, rtl/nes_ss_support.sv, rtl/nes_savestates.sv, rtl/gen/t65_nes_netlist.v,
rtl/t65_wrap.sv, rtl/gen/{nes,ppu,apu,cart,mappers,cheatcodes,nes_palette_rom}.sv, rtl/nes_clock_gate.sv,
rtl/nes_rom_cache.sv, rtl/nes_state_engine.sv, rtl/nes_board_core.sv` (see `syn/board/build.tcl`).
T65 goes in as the GHDL netlist (like the Lynx core), not through Gowin's VHDL front end.
`rtl/gen/ppu_stock.sv` (upstream OAMEval, ~16k logic) is for sim A/B only.

### What gen_rtl.py changes relative to upstream (besides the probe's Gowin fixes)
* **PPU**: OAM + secondary OAM in one block-RAM port (the probe's rewrite: no extra sprites, no
  OAM-row-corruption emulation, 32-byte secondary OAM); composite-video generator removed.
* **cart/mappers**: only MMC1, Mapper28 (0/2/3/7/…), MMC2, MMC3, MMC4, Mapper66 (11/66/…),
  Mapper69 (no 5B audio), Mapper71, VRC24 (21/22/23/25) + vrcIRQ; `tri0/tri1` buses → explicit
  OR/AND merges, mapper `inout *_b` ports → outputs.
* **APU**: square/triangle/noise channel state added to the savestate bus (indices 20–23, 25).
  Upstream does not save it, so after a load notes were held/cut and phases differed.
* **T65**: the 2-cycle reset synchronizer is released immediately during a savestate load.
  Upstream resumed a loaded state 2 CPU cycles late relative to PPU/APU.
* **Mapper69 (FME-7)**: a write to the IRQ control register ($D) now acknowledges the IRQ (as on
  the real chip). Upstream kept it pending while enabled; Holy Mapperel's FME-7 test hung in an IRQ storm.
* **nes.sv**: composite board removed; outputs `cpu_rom_hint` (PRG prefetch) and `ppu_dot` added.
* Board side (nes_board_core): after the info block is decoded the NES gets exactly 255 reset
  clocks, the first 128 with the mapper number hidden (255) so every mapper module runs its
  "not selected" initialisation with the real header flags (MiSTer does this with flags = 0 during
  the download, which leaves MMC3 in horizontal mirroring; with no initialisation at all the MMC3
  fixed $C000 bank was 0).
* Game Genie (`CODES`) is a stub. Vs./PAL/Dendy/FDS/expansion audio not built (sys_type 0).

## nes_board_core interface (all xclk unless noted)

| Port | Dir | Meaning |
|---|---|---|
| `clk` | in | xclk 67.108864 MHz |
| `reset_n` | in | low: NES in reset, caches swept, info block re-read on release |
| `pause` | in | shell quiesce. Stops the NES clock immediately (invisible to the game: frames and audio are bit-identical with or without pauses, verified) and starts no new PSRAM request. Safe to take the PSRAM when `paused_now && x_idle`. |
| `buttons[7:0]` | in | controller 1, 1 = pressed, bit 0..7 = A, B, Select, Start, Up, Down, Left, Right (already synchronised to xclk). Port 2 = empty controller. |
| `x_request` `x_read` `x_address[22:0]` `x_length[10:0]` `x_write_data` / `x_write_next` `x_done` `x_read_valid` `x_read_data[15:0]` | out / in | PSRAM port, same signals as the shell's virtual xport. 64-byte read bursts (cache fills), 8-byte writes (state staging, data consumed with `x_write_next`). `x_request` is a 1-cycle pulse, one transaction at a time; `x_done` may come with or after the last word. `x_address` = PSRAM byte address = `ROM_BASE` (parameter, default 0x020000) + ROM-window offset. |
| `x_idle` | out | no transaction in flight and none about to start (while `pause`: none will start) |
| `pix_strobe`, `pix_x[7:0]`, `pix_y[7:0]`, `pix_rgb[14:0]` | out | one strobe per visible pixel, x 0–255, y 0–239, all 240 lines; BGR555 with R in bits 4:0. Strobes are ≥ 12 xclk apart. None while the core is paused for a save state. |
| `frame_strobe` | out | start of vblank (after line 239) |
| `audio_sample[15:0]`, `audio_strobe` | out | APU output (MiSTer mixer, unsigned), new value each strobe = every CPU cycle (≈1.79 MHz nominal, 21.44196 MHz/12 here) |
| `running` | out | info block accepted, NES out of reset |
| `paused_now` | out | `pause` is in effect (NES clock stopped) |
| `bad_image` | out | info block magic/sizes rejected (core held in reset) |
| `rom_fault` | out | a PSRAM transfer did not finish within 2^18 xclk (sticky until reset) |
| `mapper_flags_o[63:0]` | out | MiSTer mapper_flags as decoded (debug) |
| `freeze_toggle` `release_toggle` `load_toggle` `block_toggle` `block_number[6:0]` `block_sequence[15:0]` / `block_ready` `block_ready_sequence` | in / out | MCU state commands (toggles, as lynx_state_engine) |
| `state_busy` `state_fault` `state_frozen` `state_loaded` | out | status |
| `qclk`, `qaddress[9:0]` / `qdata[7:0]` | in / out | 1 KiB block buffer read port (on qclk) |
| `video_resume_ok` | in | RELEASE un-freezes only while this is high (tie 1 if unused) |
| `cart_import` | in | hold high across `reset_n` release to import the staged battery RAM before the first instruction |
| `cart_imported` | out | import done |
| `cart_dirty_clear` | in | 1-cycle pulse when the MCU starts copying battery RAM: `cart_snapshot <= cart_written` (including a write in that same cycle), `cart_written <= 0`, `cart_dirty <= 0`. Nothing else clears them (except reset). |
| `cart_dirty`, `cart_written[7:0]`, `cart_snapshot[7:0]` | out | game wrote PRG RAM since last clear; bit n = 1 KiB block $6000+n·1K written |

### Clocking / timing constraints
The NES runs on `nclk` = DCE(xclk, `ce_reg`). `ce_reg` is a flip-flop; an xclk edge with
`ce_reg`=1 is also a NES edge. A 26-bit accumulator adds 21,441,960/xclk → exactly 357,366 master
clocks × 60.000 Hz (frames measured at 60.000 Hz in sim). NES edges are ≥ 3 xclk apart; the enable
is held low while a ROM byte the next edge consumes is missing, while `pause`, before the info block
is decoded and during the battery import. Missed edges are made up later at up to 1/3 xclk
(credit capped at 2047), so the average rate stays 60 Hz.
Constraints for the integration: nclk→nclk paths (the whole MiSTer core) are multicycle 3; paths
from nclk registers (the NES address buses) into the xclk memories/caches are also multicycle 3
(addresses settle ≥ 3 xclk before their data is consumed); xclk→nclk single cycle. The probe's
Fmax estimate for the core alone was 63–66 MHz single-cycle.

### Memory system
Every NES memory access completes in exactly **one NES clock** (data latched on the NES edge after
the strobe rises), whatever the source; the clock waits for the caches. So NES-visible behaviour
is independent of PSRAM latency (verified: identical video/audio hashes with 6..66-cycle random
latency and random pauses). As on MiSTer, `cpumem_din`/`ppumem_din` hold the last byte read or
written; they are in savestate register 28 (SS_Ext, bits 15:0), which also carries both
controller shift registers (bits 63:16).

| Bus | Linear address (mapper output) | Goes to |
|---|---|---|
| CPU | 0x000000–0x1FFFFF | PRG ROM: PRG cache, ROM window 0x001000 + addr |
| CPU | 0x38xxxx | CPU RAM 2 KiB BSRAM |
| CPU | anything else (0x3Cxxxx = $6000) | PRG RAM 8 KiB BSRAM (dual port; port B = battery engine) |
| PPU | 0x3Axxxx | CIRAM 2 KiB BSRAM |
| PPU | 0x2xxxxx, CHR ROM | CHR cache, ROM window CHR base + addr |
| PPU | 0x2xxxxx with CHR RAM, 0x3FE000–0x3FFFFF (MMC3 four-screen / TQROM), rest | CHR RAM 8 KiB BSRAM |

Caches: direct-mapped, 64-byte lines, 128 lines (8 KiB) each, tags+valid in one BSRAM. The CPU
cache prefetches as soon as the CPU puts a cartridge read address on the bus (`cpu_rom_hint`,
~5 NES clocks before the strobe); the CHR cache looks up every CHR-ROM address the PPU presents.
In the test ROMs and demos the NES clock was stalled 0.01–0.05 % of the time.

### PSRAM ROM window (byte offsets; PSRAM address = 0x020000 + offset)

| Offset | Contents |
|---|---|
| 0x000000 | info block, 16 bytes (below) |
| 0x001000 | PRG ROM, then CHR ROM immediately after it (0x1000 + PRG size) |
| … | PRG + CHR must end ≤ 0x3EFFFF (≤ 3.93 MiB; PRG ≤ 2 MiB, CHR ≤ 1 MiB) |
| 0x3F0000 | save-state staging (21 KiB) |
| 0x3F8000 | battery RAM import area (8 KiB) |

Info block (written by the MCU; the iNES/NES 2.0 parsing is the MCU's — `sim/nes_test.cpp` has a
reference implementation):

| Byte | Value |
|---|---|
| 0–3 | `'N','E','S','1'` |
| 4–5 | mapper number, little endian (iNES: flags6/7 nibbles; NES 2.0: + byte 8 low nibble) |
| 6 | submapper (NES 2.0 byte 8 high nibble, else 0) |
| 7 | bit0 vertical mirroring (flags6.0), bit1 four-screen (flags6.3), bit2 battery (flags6.1), bit3 CHR RAM (no CHR ROM), bit4 NES 2.0 header |
| 8–9 | PRG ROM size in KiB (LE), 8..2048 |
| 10–11 | CHR ROM size in KiB (LE), 0 = CHR RAM |
| 12 | NES 2.0 header byte 10 (PRG-RAM / PRG-NVRAM shift counts). **For iNES 1.0 files write 0x07** (8 KiB PRG RAM): upstream's Mapper28 (NROM/UxROM/CNROM/AxROM) maps no $6000 RAM otherwise; the blargg ROMs need it. MMC1/MMC3 always have RAM. |
| 13 | NES 2.0 header byte 11 (CHR-RAM shifts), else 0 (CHR RAM is always 8 KiB here) |
| 14–15 | 0 |

The MCU should refuse files whose mapper is not in the supported set (the core does not check).
A trainer (flags6.2) is skipped by the MCU.

## Save states

Same command path as the Lynx core: FREEZE → the core pauses at vblank on an instruction boundary
(up to one frame), MiSTer's savestates engine writes the image over the 64-bit bus to the staging
area (8-byte PSRAM writes), the core stays frozen; BLOCK n copies 1 KiB to the MCU buffer; LOAD
(while frozen) reads a staged image back (refused — `state_fault`, game unchanged — if the size
word is wrong); RELEASE continues once `video_resume_ok`.

Image: **21 blocks = 21,504 bytes** (`STATESIZE` = 0x1500 words):

| Offset | Size | Content |
|---|---|---|
| 0x0000 | 8 | header: bytes 0–3 save counter, 4–7 = 0x00001500 (size check on load) |
| 0x0008 | 512 | 64 savestate registers × 8 bytes (index n at 0x0008 + 8n): CPU 0–2, PPU 4–12, APU 16–19, APU channels 20–23/25 (new), TOP 24, EXT 28 (bus latches + joypads), mapper 32–40 |
| 0x0208 | 256 | OAM |
| 0x0308 | 8192 | CHR RAM (also four-screen/TQROM RAM; zeros for CHR-ROM games) |
| 0x2308 | 2048 | CIRAM |
| 0x2B08 | 2048 | CPU RAM |
| 0x3308 | 8192 | PRG RAM (battery RAM) |
| 0x5308 | 504 | padding to 21 KiB (zero) |

Block numbers: 0–20 staged image; **64–71 live PRG RAM** ($6000 + (n−64)·1 KiB) read through the
second RAM port at any time, without pausing the game; others read zeros.
Battery: with `cart_import` high when `reset_n` rises, the 8 KiB at window 0x3F8000 are copied into
PRG RAM before the NES leaves reset (`cart_imported`). For saving, the MCU pulses `cart_dirty_clear`,
reads blocks 64–71 (only those set in `cart_snapshot` if it wants) and repeats while `cart_dirty`.

## Test results (Verilator, through the caches with 6 + 0..24 cycles random PSRAM latency)

Full list with per-ROM text: `sim/out/results.txt` (`python3 -I sim/run_tests.py`; screenshots in
`sim/out/<test>/last.png`). Frames averaged exactly 60.000 Hz in every run except one pathological
case (below). The NES clock was stalled 0.001–0.2 % of xclk cycles in tests, games and demos.

| Group | Result | Notes |
|---|---|---|
| nestest (official + unofficial pages) | **PASS** | $02 = $03 = 0 |
| instr_test-v5 (16 singles + all_instrs on MMC1) | **17/17** | |
| instr_misc, instr_timing, cpu_interrupts_v2, branch_timing | **4/4, 2/2, 5/5, 3/3** | |
| cpu_dummy_reads (CNROM), cpu_dummy_writes oam/ppumem, cpu_exec_space ppuio/apu | **5/5** | |
| cpu_reset registers / ram_after_reset | n/a | need a Reset button (none on this core) |
| ppu_vbl_nmi (01–10) | **10/10** | |
| vbl_nmi_timing (2005) | 4/7 | 1 #5, 2 #7, 5 #8 — power-up CPU/PPU alignment, see gaps |
| blargg_ppu_tests 2005 (palette_ram, sprite_ram, vbl_clear_time, vram_access) | **4/4** | power_up_palette #2 = "values unique to my NES" (author), n/a |
| ppu_open_bus, ppu_read_buffer (CNROM) | **2/2** | |
| sprite_hit_tests 01–11 | **11/11** | OAM rewrite |
| sprite_overflow_tests 1–5 | 4/5 | 3.Timing #13 — same with upstream OAMEval (A/B below) |
| oam_read, oam_stress | **2/2** | OAM rewrite |
| sprdma_and_dmc_dma (+_512) | **2/2** | |
| apu_test 1–8 | **8/8** | |
| mmc3_test_2 1–6 / mmc3_test 1–6 | 5/6 / 5/6 | 6-MMC3_alt tests the other MMC3 revision (5-MMC3 passes; both cannot); 6-MMC6 needs the NES 2.0 MMC6 submapper, the ROM is iNES 1.0 |
| Holy Mapperel: 0, 1 (SJROM 128K, SUROM 512K), 2, 3, 4, 7, 9, 10, 11, 66, 69 | **all mappers detected, banks/RAM OK, detailed test 0000** | except MMC1 detailed 1000 = WRAM-disable bit not implemented upstream; 69 passes after the FME-7 IRQ fix |
| Visual: nes15 (NROM), 240p test suite (UxROM + CHR RAM), BladeBuster (MMC3), smwstomp (MMC3), high-hopes (MMC3), pulsar (MMC1), quantum_disco_brothers (CNROM), oam3 (AxROM), fceuxd (mapper 11), m22chrbankingtest (VRC2, 22), mmc1_a12, full_palette | correct pictures | |
| **Save-state round trip** (save at frame A, run 60, freeze, stage the saved image, load, run 60): quantum_disco_brothers, high-hopes, BladeBuster, smwstomp, pulsar (MMC1 + CHR RAM), 240pee, nes15 | **7/7: 60/60 frames and 60/60 per-frame audio hashes identical**; full state images after the two runs byte-identical | 40-cycle random PSRAM latency |
| Shell pause + PSRAM latency invisibility (high-hopes, 150 frames, ~800 random pauses, 6..66-cycle latency vs none) | **identical video and audio hashes** | no request seen while `paused_now && x_idle` |
| Battery: import 8 KiB at reset, game writes, `cart_dirty_clear`, live block 64–71 read | **PASS** | only the bytes the game wrote changed; written/snapshot = block 0 |

Performance note: Holy Mapperel's SUROM 512 KiB test scans bank tags 4 KiB apart over the whole
ROM, which misses the 8 KiB PRG cache on almost every read (1.7 M bursts in 600 frames); that run
averaged 55.7 Hz. Normal code stays at 60.000 Hz.

A/B of the OAM rewrite (`sim/ab_sprites.sh`, same harness, rtl/gen/ppu.sv vs upstream
OAMEval in rtl/gen/ppu_stock.sv):

| Test | OAM rewrite (this build) | upstream OAMEval |
|---|---|---|
| sprite_hit 01–11 | 11/11 PASS | 11/11 PASS |
| sprite_overflow 1, 2, 4 | PASS | PASS |
| sprite_overflow 3.Timing | FAIL #13 | FAIL #13 (same) |
| sprite_overflow 5.Emulator | PASS | FAIL #3 |
| oam_read, cpu_dummy_writes_oam, sprdma_and_dmc_dma, blargg sprite_ram | PASS | PASS |
| oam_stress | not finished in the 1,500-frame A/B budget in either build; PASSES in the full suite (3,000 frames) | |

So the rewrite (one OAM port in block RAM, registered read, no extra sprites, no OAM-row
corruption) loses nothing these tests check; nothing here exercises the dropped OAM-corruption
emulation (rendering enabled mid-frame / $2003 writes during rendering).


## Resources (GowinSynthesis, GW5A-25, `run syn`, `syn/board/`)

Device GW5A-EV25UG256CC1/I0 (23,040 logic, 56 BSRAM, 28 DSP). Whole `nes_board_core` including the
state engine and its 1 KiB MCU buffer, caches, all RAMs and the palette ROM:

| | Logic (LUT+ALU) | LUT | ALU | FF | BSRAM | DSP |
|---|---|---|---|---|---|---|
| **nes_board_core** | **10,644** | 10,369 | 275 | 4,737 | **23** | 0 |
| └ NES (MiSTer core) | 8,997 | 8,822 | 175 | 3,836 | 1 (OAM) | |
| &nbsp;&nbsp; ppu / apu / cart+mappers / T65 / savestates.sv | | 2,368 / 1,810 / 1,516 / 1,016 / 1,754 | | 1,413 / 872 / 792 / 333 / 293 | | |
| └ PRG cache / CHR cache | 90 / 96 | | | 56 / 56 | 5 / 5 | |
| └ state engine (+MCU buffer) | 319 | | | 345 | 1 | |
| └ glue: RAM decode, clock enable, loader, joypads, video, SS_Ext | ~1,140 | | | ~440 | 10 (CPU RAM 1, CIRAM 1, CHR RAM 4, PRG RAM 4) + palette 1 | |

Budget was ≤ ~12.5k logic / ≤ ~26 BSRAM. The `savestates` LUT count is mostly the 64-bit
savestate-register readback mux (~45 registers), inherent in MiSTer's scheme. GowinSynthesis'
single-cycle Fmax estimate on xclk is 48 MHz because it times the nclk domain as plain xclk: the
multicycle constraints above are required (needed: 3 × 14.9 ns). Warnings: 3 × EX1998 on
never-driven spare savestate bits (upstream, harmless); EX2478 (output-reg initialisers ignored in
SV mode: all such registers are reset or power up 0).

## Known gaps / open issues

* **Timing not closed** (synthesis only). Needs the multicycle-3 constraints on the DCE clock
  domain and on nclk→xclk memory-address paths; the DCE output must be a global clock.
* **Old blargg vbl_nmi_timing (2005)**: 1.frame_basics #5, 2.vbl_timing #7, 5.nmi_suppression #8 fail;
  its readme says these depend on the CPU/PPU power-up alignment. The newer `ppu_vbl_nmi` suite
  (same author, alignment-independent) passes 10/10. Upstream MiSTer alignment, unchanged.
* **sprite_overflow 3.Timing #13** (overflow on 2nd scanline too early) — see the A/B table: same
  result with upstream's OAMEval, so not caused by the OAM rewrite.
* **MMC3 powers up with PRG RAM disabled** (upstream); games enable it. blargg's MMC3 tests
  never enable it, so their $6000 report is lost: the harness reads the result text from the
  nametable instead.
* **MMC1 WRAM disable** not implemented upstream (Holy Mapperel MMC1 detailed result 1000 =
  MAPTEST_WRAMEN); all other MMC1 checks OK.
* **No Reset button** (`reset_n` is a cold reset; cpu_reset tests need a soft reset). Easy to add
  later (pulse `reset_nes` without clearing RAM).
* **Memory limits**: PRG RAM 8 KiB (no SOROM/SXROM 16/32 KiB), CHR RAM 8 KiB (no 16/32 KiB CHR RAM
  boards), four-screen only for CHR-ROM boards (shares the CHR RAM block).
* **Untested mappers**: 21/23/25 (VRC2a/VRC4) and 71 (Camerica) — no test ROM available here;
  they are upstream modules unchanged except for the bus-merge rewrite. 22 (VRC2a) passed
  m22chrbankingtest visually.
* **Frame rate** is exactly 60.000 Hz (0.16 % slower than NTSC 60.0988 Hz; audio pitch the same).
* **Pause for save states** waits for vblank + an instruction boundary (≤ 1 frame), as upstream;
  no pixel strobes while frozen.
* The MCU must parse iNES/NES 2.0 (incl. writing info byte 12 = 0x07 for iNES 1.0), reject
  unsupported mappers and sizes, and skip trainers.
