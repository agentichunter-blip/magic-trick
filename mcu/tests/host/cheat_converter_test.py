import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
spec=importlib.util.spec_from_file_location('prepare_cheats',Path(__file__).parents[2]/'tools/prepare_cheats.py')
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class Cheats(unittest.TestCase):
    def rom(self,mapper=1,banks=4):
        r=bytearray(16384*banks); r[0x147]=mapper; r[0x104:0x134]=bytes(range(48)); return r
    def test_known_decode(self):
        self.assertEqual(m.decode('00A-17B-C49'),(0x4a17,0,0xc8))
        self.assertEqual(m.decode('99D-49C-E62'),(0x3d49,0x99,2))
        self.assertEqual(m.decode('1CF-35E'),(0x1f35,0x1c,None))
        self.assertEqual(m.decode('99D+49C+E62'),m.decode('99D-49C-E62'))
        self.assertEqual(len(m.codes('00A-17B-C49+99D-49C-E62')),2)
        for code in ['010238CD','00A-17B-C49 GARBAGE','xyz','001-000','123456']:
            with self.assertRaises(ValueError): m.codes(code)
    def test_banks_and_compare(self):
        r=self.rom(); r[0x4a17]=r[0xca17]=0xc8
        self.assertEqual(m.patches(r,'00A-17B-C49'),[(0x4a17,0xc8,0),(0xca17,0xc8,0)])
        r[0x4a17]=r[0xca17]=0
        with self.assertRaisesRegex(ValueError,'compare'): m.patches(r,'00A-17B-C49')
    def test_aliases_header_and_conflicts(self):
        r=self.rom(0x19); r[0xa17]=0xc8
        with self.assertRaisesRegex(ValueError,'aliases'): m.patches(r,'00A-17B-C49')
        r=self.rom(); r[0x3d49]=2
        with self.assertRaisesRegex(ValueError,'conflicting'): m.patches(r,'99D-49C-E62+FFD-49C-E62')
        # Encoded ROM address 0104 (Nintendo logo) must never be modified.
        with self.assertRaisesRegex(ValueError,'header'): m.patches(r,'011-04F')
        with self.assertRaisesRegex(ValueError,'large'): m.patches(self.rom(1,64),'00A-17B-C49')
    def test_matching_and_pack(self):
        self.assertNotEqual(m.identity('Game (USA)'),m.identity('Game (Europe)'))
        self.assertNotEqual(m.identity('Game (World) (Rev 1)'),m.identity('Game (World)'))
        self.assertNotEqual(m.identity('Game (USA) (Sample)'),m.identity('Game (USA)'))
        self.assertEqual(m.identity('Game (USA) (SGB Enhanced)'),m.identity('Game (USA)'))
        self.assertEqual(m.identity('Game (USA, Europe) (Game Genie)'),m.identity('Game (Europe, USA)'))
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); roms=root/'roms'; db=root/'db'; out=root/'out'
            (roms/'GB/0-F').mkdir(parents=True); (db/'cht/Nintendo - Game Boy').mkdir(parents=True)
            r=self.rom(); r[0x3d49]=2
            (roms/'GB/0-F/Game (World).gb').write_bytes(r)
            (roms/'GB/0-F/Game (USA).gb').write_bytes(r)
            (db/'cht/Nintendo - Game Boy/Game (World) (Game Genie).cht').write_text('cheat0_desc = "99 lives"\ncheat0_code = "99D-49C-E62"\ncheat1_desc = "Infinite time - switch off at end"\ncheat1_code = "FFD-49C-E62"\n')
            report=m.prepare(roms,db,out)
            self.assertEqual(report['pack_count'],1); self.assertEqual(report['cheat_count'],1)
            self.assertEqual(len(report['unmatched_games']),1); self.assertEqual(len(report['rejected']),1)
            blob=(out/'GB/0-F/Game (World).gb.cmc').read_bytes()
            self.assertEqual(m.HEADER.unpack(blob[:32]),(b'CMCH',1,len(r),zlib.crc32(r),1,1,zlib.crc32(blob[32:]),0))
            self.assertEqual(m.PATCH.unpack(blob[-8:]),(0x3d49,2,0x99,0))
            self.assertEqual((roms/'GB/0-F/Game (World).gb').read_bytes(),r)

if __name__=='__main__': unittest.main()
