#!/usr/bin/env python3
"""Write gg_test.nes (NROM, 16 KiB PRG): copies $9000-$9002 (11 22 33) to RAM $10-$12, then loops.
Game Genie check (run_tests.py gg / README):
  NES_GG=9000:55,9001:66:23,9002:77:33  ->  ram 10=55 (no compare), 11=22 (compare 23 misses), 12=77 (compare hits)
usage: gg_test_rom.py OUT.nes"""
import sys
prg = bytearray(16384)
code = bytes([0x78, 0xD8, 0xA2, 0xFF, 0x9A,
              0xAD, 0x00, 0x90, 0x85, 0x10,
              0xAD, 0x01, 0x90, 0x85, 0x11,
              0xAD, 0x02, 0x90, 0x85, 0x12,
              0x4C, 0x14, 0xC0,      # $C014: JMP $C014
              0x40])                 # $C017: RTI
prg[0:len(code)] = code
prg[0x1000:0x1003] = bytes([0x11, 0x22, 0x33])          # $9000 ($D000 mirror)
prg[0x3FFA:0x4000] = bytes([0x17, 0xC0, 0x00, 0xC0, 0x17, 0xC0])
open(sys.argv[1], 'wb').write(b'NES\x1a' + bytes([1, 1, 0, 0]) + bytes(8) + bytes(prg) + bytes(8192))
