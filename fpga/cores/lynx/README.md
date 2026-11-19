# Atari Lynx core

An Atari Lynx for the Chromatic, built from [AtariLynx_MiSTer](https://github.com/MiSTer-devel/AtariLynx_MiSTer) (GPL-2.0, revision in `vendor/AtariLynx_MiSTer/REVISION`) on the Game Gear core's board shell. FPGA version `50.3`, status identity `'L'`.

## Layout

| Path | What |
|---|---|
| `vendor/AtariLynx_MiSTer/` | upstream sources, unchanged |
| `lynx-rtl/` | the core as built: upstream VHDL with the Chromatic changes below, plus `lynx_chromatic.vhd` (std_logic ports for the Verilog board code) |
| `lynx_board_core.sv` | 16 MHz clock enable, PSRAM loader (boot ROM, cartridge header, BS93 homebrew), cartridge reads through the ROM cache |
| `board-rtl/lynx_video_bridge.sv` | Lynx rows to the panel: 48-row pen ring with per-row palettes, letterboxed 160x102 in 160x144 or stretched to fill it, adaptive frame start |
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

## Screen: Original or Stretch

Display > LYNX SCREEN in the menu (MCU setting `lynx-stretch`, sent as system control bit 15; the other cores ignore it). Original letterboxes the 160x102 picture with 21 black rows above and below. Stretch fills all 144 panel rows: each panel row covers 17/24 of a Lynx row, so a row inside one Lynx row shows it unchanged and a row straddling two shows both mixed by coverage in quarters (pure colours stay exact). Everything looks 41% taller. The bridge starts stretched frames later so that Lynx row 101 is ready at panel row 142. The change applies from the next frame; while the menu is open the panel repeats the old picture.

## Sideways games

Some games are played with the Lynx held on its side. The MCU picks the rotation when it loads a game (`LynxRotation()` in `mcu/main/virtual_cart.c`): header byte 58 if it is 1 (left) or 2 (right), otherwise a table of CRC32s of the first 4 KB after the header (`LynxRom_KnownRotation()` in `mcu/main/lynx_rom.h`; most No-Intro dumps carry no rotation byte). Known games: Gauntlet - The Third Encounter and Klax (left); Centipede, Lexis, NFL Football and Raiden (right). It passes the rotation in info block byte 8, bits 3:2.

For a rotated game the bridge keeps two whole 160x102 frames (pens in a 32768x4 array whose first buffer shares the normal ring, palette per frame in palette slots 48/49) and shows the finished one turned to 102x160, scaled by 9/10 to 92x144 with a 2x2 coverage mix, centred with 34 black columns each side. Display lags one Lynx frame. LYNX SCREEN has no effect on rotated games. `lynx_board_core.sv` turns the D-pad so Up is up on the Chromatic's screen. Sim: `LYNX_ROT=1|2` in `tests/sim`.

## Boot ROM and game files

The MCU stages a 16-byte info block, the boot ROM and the game file in PSRAM (see `lynx_board_core.sv`). `.lnx` games don't need the Lynx boot ROM: without it the MCU decrypts the cartridge loader itself (`mcu/main/lynx_hle.c`) and starts it with a small replacement stub (`mcu/tools/lynx_hle/`), including two-stage loaders. If `lynxboot.img` (512 bytes, not included) is next to the game, in `CHROMAGIC/BACKUPS/LYNX/` or in `CHROMAGIC/BIOS/`, it is used instead. BS93 homebrew (`.o`) needs neither.

## Controls

D-pad, A, B as on the Lynx; Start = Pause; Select = Option 1; Menu + B = Option 2.

## Save states

Same commands as the Game Gear core (10 freeze, 14 block, 11 load, 16 release). FREEZE makes the core save itself into PSRAM at ROM-window 0x200000 (MiSTer's 66,560-byte image: header, registers, 64 KiB RAM) and then pauses; the MCU reads it in 65 blocks of 1 KiB. LOAD reads a staged image back. Blocks 80-124 read the panel framebuffer instead (diagnostics, `lynxshot` console command).

## Flash slot and switching

Slot `0x200000`, next boot `0x000000` (GB/GBC). Core switching uses the cycle GB -> GG -> Lynx -> GB, so the Game Gear image must boot `0x200000` next; the MCU reloads again when it lands on the core in between (`core_switch.c`).

## Building

`gw_sh build.tcl` from this directory (Gowin EDA V1.9.12.04). Simulation: `tests/sim/run.sh` (GHDL from OSS CAD Suite, Verilator 5).
