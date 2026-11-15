# Atari Lynx core

An Atari Lynx for the Chromatic, built from [AtariLynx_MiSTer](https://github.com/MiSTer-devel/AtariLynx_MiSTer) (GPL-2.0, revision in `vendor/AtariLynx_MiSTer/REVISION`) on the Game Gear core's board shell. FPGA version `50.3`, status identity `'L'`.

## Layout

| Path | What |
|---|---|
| `vendor/AtariLynx_MiSTer/` | upstream sources, unchanged |
| `lynx-rtl/` | the core as built: upstream VHDL with the Chromatic changes below, plus `lynx_chromatic.vhd` (std_logic ports for the Verilog board code) |
| `lynx_board_core.sv` | 16 MHz clock enable, PSRAM loader (boot ROM, cartridge header, BS93 homebrew), cartridge reads through the ROM cache |
| `board-rtl/lynx_video_bridge.sv` | Lynx rows to the panel: 64-row pen ring with per-row palettes, letterboxed 160x102 in 160x144, adaptive frame start |
| `lynx_state_engine.sv` | save states over the Game Gear state commands; MiSTer's state image staged in PSRAM |
| `lynx_dpram.v` | the core's dual-port RAMs (replaces Intel `altsyncram`) |
| `emu_system_top.v`, `src/` | board shell (from the Game Gear core); panel ring 2 banks and hold-after-frame, version 50.3 |
| `tests/sim/` | Verilator + GHDL simulation of the real `emu_system_top` (`run.sh`) |
| `tests/testprog/` | original 6502 test program (BS93, needs no boot ROM) |

## Changes to the MiSTer core

- **Clock**: runs on the board's 67.108864 MHz `xclk`. The 16 MHz Lynx clock enable is an exact 15625/65536 divider (ticks 4 or 5 clocks apart); MiSTer used 64 MHz / 4. Fast forward and turbo are removed.
- **60 Hz frame lock** (`timer.vhd`, `timer_module.vhd`): Lynx games choose their own LCD rate (about 50 to 79 Hz). The panel refreshes at 60 Hz, so a 75 Hz game drops a frame in five and fine scrolling judders. When a game's frame is shorter than 1/60 s, timer 0 (the line timer that clocks timer 2, the frame counter) is clocked by a fractional divider so that exactly `(TIM0BKUP+1)*(TIM2BKUP+1)` ticks fill 1/60 s. Only the frame rate changes: the CPU, Suzy and the audio timers (chain 1-3-5-7-audio) keep their real speed, so sound neither pauses nor changes pitch. Game logic that runs once per frame runs at 60 per second (a 75 Hz game plays at 80% speed, smoothly). MiSTer's own "sync to 60 Hz" pauses the whole system every frame instead, which stutters the audio. Games at 60 Hz or below are untouched.
- **Video** (`display_dma.vhd`): exports each pixel's 4-bit pen and the 16-entry palette, so the bridge buffers pens instead of 12-bit colour (4 block RAMs instead of 11).
- **RAM** in Verilog (`lynx_dpram.v`); Gowin mapped the VHDL version to an unsupported read-before-write mode.

## Boot ROM and game files

The MCU stages a 16-byte info block, the boot ROM and the game file in PSRAM (see `lynx_board_core.sv`). `.lnx` games need the Lynx boot ROM (`lynxboot.img`, 512 bytes, not included) in `CHROMAGIC/BACKUPS/LYNX/` or `CHROMAGIC/BIOS/`. BS93 homebrew (`.o`) runs without it.

## Controls

D-pad, A, B as on the Lynx; Start = Pause; Select = Option 1; Menu + B = Option 2.

## Save states

Same commands as the Game Gear core (10 freeze, 14 block, 11 load, 16 release). FREEZE makes the core save itself into PSRAM at ROM-window 0x200000 (MiSTer's 66,560-byte image: header, registers, 64 KiB RAM) and then pauses; the MCU reads it in 65 blocks of 1 KiB. LOAD reads a staged image back. Blocks 64-108 read the panel framebuffer instead (diagnostics, `lynxshot` console command).

## Flash slot and switching

Slot `0x200000`, next boot `0x000000` (GB/GBC). Core switching uses the cycle GB -> GG -> Lynx -> GB, so the Game Gear image must boot `0x200000` next; the MCU reloads again when it lands on the core in between (`core_switch.c`).

## Building

`gw_sh build.tcl` from this directory (Gowin EDA V1.9.12.04). Simulation: `tests/sim/run.sh` (GHDL from OSS CAD Suite, Verilator 5).
