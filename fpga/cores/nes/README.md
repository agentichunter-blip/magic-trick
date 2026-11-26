# NES core (FPGA 50.4, flash slot 0x300000)

The MiSTer NES core (NES_MiSTer, GPLv3, upstream commit cf7fe6c) on the Game Gear
board shell. Core details, test results and the port table are in
[core/README.md](core/README.md); the fit study is [core/FEASIBILITY.md](core/FEASIBILITY.md).

## Layout
- `core/rtl/` — `nes_board_core.sv` (clocking, on-chip RAMs, ROM caches, loader,
  video/audio taps), `nes_state_engine.sv` (save states and battery RAM),
  `nes_rom_cache.sv`, `nes_clock_gate.sv` (DFFR enable + GW5A DCE).
  `core/rtl/gen/` is generated from the upstream clone (`core/tools/regen.sh`
  with the clone at `core/NES_MiSTer`, not committed).
- `core/board/nes_video_bridge.sv` — 256x240 to the 160x144 panel: lines 8-231
  box-filtered 5/8 both ways (160x140, two black rows above and below).
- `emu_system_top.v`, `src/` — board shell (from the Game Gear core). Changes for
  the NES: status tag 'N', version 50.4, live battery blocks 64-71 in
  `src/top.v`, video hold on the state engine's frozen flag, battery
  dirty-clear on block 64, cartridge-slot engine stubbed (as on the Lynx core:
  no logic to spare; FPGA USB is kept).
- `core/sim/` — Verilator harness (`run_tests.py` runs the test-ROM suite; test
  ROMs not committed).

## Timing
The NES runs on xclk gated by the DCE. There is deliberately no generated clock
on the DCE output (it would lose the PLL-to-DCE delay); STA traces xclk through
it. Every path from a NES register is a two-cycle path (`nes_board.sdc`; Gowin's
`*` does not cross hierarchy levels, so each level is listed, and a multicycle
needs `-to`). The enable logic in `nes_board_core.sv` guarantees it: NES edges at
least 3 xclk apart (a ROM byte from the cache included), and NES outputs
sampled from the second xclk edge after a NES edge. Games run at 60.000 Hz in
simulation, and the audio leaves through a steady-rate queue (`nes_audio_smooth.sv`),
so clock stalls are not heard.

Place and route takes longer than 30 minutes at 89% logic.

## Boot ring
GB (0x000000) -> GG (0x100000) -> NES (0x300000) -> Lynx (0x200000) -> GB.
