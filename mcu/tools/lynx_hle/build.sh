#!/bin/bash
# Rebuild hle_boot.bin with cc65 (ca65/ld65) and check the patch offsets in
# main/lynx_hle_boot.h (LOADER 0x4d, SKIPLO 0x14d, SKIPHI 0x14e).
set -e
cd "$(dirname "$0")"
CC65=${CC65:-ca65}; LD65=${LD65:-ld65}
$CC65 -l hle_boot.lst hle_boot.s -o hle_boot.o
$LD65 -C hle_boot.cfg hle_boot.o -o hle_boot.bin -m hle_boot.map
grep -E "LOADER:|SKIPLO:|SKIPHI:" hle_boot.lst
