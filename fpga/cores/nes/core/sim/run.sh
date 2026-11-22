#!/bin/bash
# Build the Verilator model (once) and run one ROM: sim/run.sh ROM.nes OUTDIR [frames]
set -euo pipefail
S="$(cd "$(dirname "$0")" && pwd)"; W="$(cd "$S/.." && pwd)"
V=${VERILATOR:-$(cd "$W/.." && pwd)/toolchains/oss-cad-suite/bin/verilator}
B="${NES_BUILD:-$S/build}"
mkdir -p "$B"
SRCS=("$S/nes_sim_top.sv" "$W/rtl/nes_board_core.sv" "$W/rtl/nes_rom_cache.sv" "$W/rtl/nes_state_engine.sv"
  "$S/nes_clock_gate_model.sv" "$W/rtl/nes_savestates.sv" "$W/rtl/nes_ss_support.sv" "$W/rtl/t65_wrap.sv" "$W/rtl/gowin_dpb_snapshot_model.sv"
  "$W/rtl/gen/regs_savestates.sv" "$W/rtl/gen/nes.sv" "$W/rtl/gen/${NES_PPU:-ppu}.sv" "$W/rtl/gen/apu.sv" "$W/rtl/gen/cart.sv"
  "$W/rtl/gen/mappers.sv" "$W/rtl/gen/cheatcodes.sv" "$W/rtl/gen/nes_palette_rom.sv" "$W/rtl/gen/t65_nes_netlist.v")
NEW=0
[ -x "$B/obj/Vnes_sim_top" ] || NEW=1
for f in "${SRCS[@]}" "$S/nes_test.cpp"; do [ "$f" -nt "$B/obj/Vnes_sim_top" ] && NEW=1; done
if [ $NEW = 1 ]; then
  "$V" --cc --exe --build -j 8 -O3 --x-assign fast --x-initial fast -Wno-fatal -Wno-lint -Wno-style \
    -Wno-MULTIDRIVEN -Wno-UNOPTFLAT -Wno-COMBDLY -Wno-LATCH -Wno-TIMESCALEMOD \
    -CFLAGS "-O2 -std=c++17" --top-module nes_sim_top -Mdir "$B/obj" "${SRCS[@]}" "$S/nes_test.cpp" \
    > "$B/verilator.log" 2>&1 || { grep -E "%Error" "$B/verilator.log" | head -40; exit 1; }
fi
[ $# -ge 2 ] || exit 0
mkdir -p "$2"
exec "$B/obj/Vnes_sim_top" "$@"
