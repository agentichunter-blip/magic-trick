#!/bin/bash
# A/B check of the OAM rewrite: run the sprite/OAM tests with the rewritten PPU (rtl/gen/ppu.sv)
# and with upstream's OAMEval (rtl/gen/ppu_stock.sv). usage: sim/ab_sprites.sh
set -uo pipefail
S="$(cd "$(dirname "$0")" && pwd)"; R="$S/../testroms/nes-test-roms"
NES_PPU=ppu_stock NES_BUILD="$S/build_stock" "$S/run.sh" || exit 1
NES_BUILD="$S/build_dev" "$S/run.sh" || exit 1
run() { # variant build name rom mode
  local d="$S/out/ab/$1/$3"; mkdir -p "$d"
  if [ "$4" = b ]; then E="NES_BLARGG=1"; else E="NES_F8=1"; fi
  env $E "$S/$2/obj/Vnes_sim_top" "$R/$5" "$d" 1500 > "$d/log.txt" 2>&1
  echo "$1 $3: $(grep -o 'blargg result=[0-9]*\|f8 result=[0-9]* ([^)]*)' "$d/log.txt" | head -1)"
}
T=("hit01 f sprite_hit_tests_2005.10.05/01.basics.nes" "hit02 f sprite_hit_tests_2005.10.05/02.alignment.nes"
   "hit03 f sprite_hit_tests_2005.10.05/03.corners.nes" "hit04 f sprite_hit_tests_2005.10.05/04.flip.nes"
   "hit05 f sprite_hit_tests_2005.10.05/05.left_clip.nes" "hit06 f sprite_hit_tests_2005.10.05/06.right_edge.nes"
   "hit07 f sprite_hit_tests_2005.10.05/07.screen_bottom.nes" "hit08 f sprite_hit_tests_2005.10.05/08.double_height.nes"
   "hit09 f sprite_hit_tests_2005.10.05/09.timing_basics.nes" "hit10 f sprite_hit_tests_2005.10.05/10.timing_order.nes"
   "hit11 f sprite_hit_tests_2005.10.05/11.edge_timing.nes"
   "ovf1 f sprite_overflow_tests/1.Basics.nes" "ovf2 f sprite_overflow_tests/2.Details.nes" "ovf3 f sprite_overflow_tests/3.Timing.nes"
   "ovf4 f sprite_overflow_tests/4.Obscure.nes" "ovf5 f sprite_overflow_tests/5.Emulator.nes"
   "oam_read b oam_read/oam_read.nes" "oam_stress b oam_stress/oam_stress.nes"
   "dummy_writes_oam b cpu_dummy_writes/cpu_dummy_writes_oam.nes" "sprdma b sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes"
   "sprite_ram f blargg_ppu_tests_2005.09.15b/sprite_ram.nes")
for t in "${T[@]}"; do set -- $t
  if [ "$1" = sprite_ram ]; then export NES_RESADDR=F0; else unset NES_RESADDR; fi
  run stock build_stock "$1" "$2" "$3" &
  run oamram build_dev "$1" "$2" "$3" &
  while [ "$(jobs -r | wc -l)" -ge 4 ]; do sleep 1; done   # (macOS bash 3.2: no wait -n)
done
wait
