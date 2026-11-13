#!/bin/bash
# Game Gear save-state engine test (Verilator). Saves through the 1 KiB block
# buffer, restores from a payload staged in modelled PSRAM, and requires the
# restored game to replay the saved game's CPU bus, video and audio exactly.
#   run.sh ROM.gg [warm-up cycles] [compared cycles]
# Sonic The Hedgehog (World) (Rev 1): 1100000000 reaches Green Hill Zone.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$(cd "$here/../.." && pwd)"
out="${GG_STATE_SIM_DIR:-${TMPDIR:-/tmp}/gg-state-sim}"
mkdir -p "$out"
verilator --cc --exe --build -CFLAGS "-std=c++17 -O2" -MAKEFLAGS "OPT_FAST=-O2" -j 8 \
 --top-module gg_ss_sim_top -Wno-fatal -Wno-lint -Wno-style --Mdir "$out/obj" \
 "$here/gg_ss_sim_top.sv" "$core/gg_board_core.sv" "$core/board-rtl/gg_state_clock.sv" \
 "$core/gg_state_engine.sv" "$core/gg_rom_bridge.sv" "$core/gg_system.v" "$core/generated/video.v" \
 "$core/gg_cheatcodes.sv" "$core"/vendor/SMS_MiSTer/rtl/jt89/*.v "$core/vendor/SMS_MiSTer/rtl/MC8123.v" \
 "$core/vendor/SMS_MiSTer/rtl/SEGASYS1_PRGDEC.v" "$core/vendor/SMS_MiSTer/rtl/parts.v" \
 "$here/ss_test.cpp" > "$out/build.log" 2>&1
cd "$out"   # screenshots (captured/saved-run/restored-run .ppm) land here
"$out/obj/Vgg_ss_sim_top" "$@"
