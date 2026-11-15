#!/bin/bash
# Build and run the Lynx core simulation (GHDL netlist of lynx-rtl + Verilator).
set -euo pipefail
cd "$(dirname "$0")"
L=../..
G=../../../../../../toolchains/oss-cad-suite/bin/ghdl
mkdir -p build out
if [ ! -f build/lynx_chromatic.v ] || [ -n "$(find $L/lynx-rtl -newer build/lynx_chromatic.v -name '*.vhd')" ]; then
  (cd build && rm -f work-obj08.cf && F="--std=08 -frelaxed -fsynopsys" &&
   $G -i $F ../$L/lynx-rtl/*.vhd > ghdl.log 2>&1 && $G -m $F lynx_chromatic >> ghdl.log 2>&1 &&
   $G --synth $F --out=verilog lynx_chromatic > lynx_chromatic.v 2>> ghdl.log)
fi
verilator --cc --exe --build -j 8 -O3 --x-assign fast --x-initial fast -Wno-fatal -Wno-lint -Wno-style \
  --top-module lynx_sim_top -Mdir build/obj \
  lynx_sim_top.sv $L/emu_system_top.v $L/lynx_board_core.sv $L/lynx_state_engine.sv $L/lynx_dpram.v \
  $L/board-rtl/lynx_video_bridge.sv $L/board-rtl/gg_async_fifo.sv $L/board-rtl/gg_audio_bridge.sv \
  $L/gg_rom_bridge.sv $L/gg_audio_lpf.sv build/lynx_chromatic.v lynx_test.cpp > build/verilator.log 2>&1 \
  || { tail -30 build/verilator.log; exit 1; }
./build/obj/Vlynx_sim_top "${1:-../testprog/lynxtest_bs93.o}" out "${2:-12}" "${3:-1}"
