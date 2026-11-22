"""Run the test-ROM suite in parallel through sim/run.sh and print a pass/fail table.
usage: python3 -I sim/run_tests.py [filter-substring ...]   (results also in sim/out/results.txt)
Test ROMs are data only (testroms/, untrusted downloads); nothing from them is executed on the host.
"""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

S = Path(__file__).resolve().parent
R = S.parent / 'testroms' / 'nes-test-roms'
OUT = S / 'out'
BIN = OUT / 'Vnes_sim_top_suite'
# (name, rom, mode, frames, extra env)   mode: b = $6000 protocol, f8 = result at $F8, v = visual only
T = []
for n in sorted((R / 'instr_test-v5/rom_singles').glob('*.nes')):
    T.append(('instr_test-v5/' + n.stem, n, 'b', 1500, {}))
T += [('instr_test-v5/all_instrs (MMC1)', R / 'instr_test-v5/all_instrs.nes', 'b', 3000, {})]
for n in sorted((R / 'instr_misc/rom_singles').glob('*.nes')):
    T.append(('instr_misc/' + n.stem, n, 'b', 900, {}))
for n in sorted((R / 'instr_timing/rom_singles').glob('*.nes')):
    T.append(('instr_timing/' + n.stem, n, 'b', 2000, {}))
for n in sorted((R / 'cpu_interrupts_v2/rom_singles').glob('*.nes')):
    T.append(('cpu_interrupts_v2/' + n.stem, n, 'b', 900, {}))
T += [('cpu_dummy_reads (CNROM)', R / 'cpu_dummy_reads/cpu_dummy_reads.nes', 'b', 900, {}),
      ('cpu_dummy_writes_oam', R / 'cpu_dummy_writes/cpu_dummy_writes_oam.nes', 'b', 900, {}),
      ('cpu_dummy_writes_ppumem', R / 'cpu_dummy_writes/cpu_dummy_writes_ppumem.nes', 'b', 900, {}),
      ('cpu_exec_space_ppuio', R / 'cpu_exec_space/test_cpu_exec_space_ppuio.nes', 'b', 900, {}),
      ('cpu_exec_space_apu', R / 'cpu_exec_space/test_cpu_exec_space_apu.nes', 'b', 900, {}),
      ('cpu_reset/registers', R / 'cpu_reset/registers.nes', 'b', 900, {}),
      ('cpu_reset/ram_after_reset', R / 'cpu_reset/ram_after_reset.nes', 'b', 900, {})]
for n in sorted((R / 'branch_timing_tests').glob('*.nes')):
    T.append(('branch_timing/' + n.stem, n, 'f8', 1200, {}))
for n in sorted((R / 'ppu_vbl_nmi/rom_singles').glob('*.nes')):
    T.append(('ppu_vbl_nmi/' + n.stem, n, 'b', 1500, {}))
for n in sorted((R / 'vbl_nmi_timing').glob('*.nes')):
    T.append(('vbl_nmi_timing/' + n.stem, n, 'f8', 1500, {}))
for n in sorted((R / 'blargg_ppu_tests_2005.09.15b').glob('*.nes')):
    T.append(('blargg_ppu_tests/' + n.stem, n, 'f8', 900, {'NES_RESADDR': 'F0'}))
for n in sorted((R / 'sprite_hit_tests_2005.10.05').glob('*.nes')):
    T.append(('sprite_hit/' + n.stem, n, 'f8', 1200, {}))
for n in sorted((R / 'sprite_overflow_tests').glob('*.nes')):
    T.append(('sprite_overflow/' + n.stem, n, 'f8', 1200, {}))
T += [('oam_read', R / 'oam_read/oam_read.nes', 'b', 900, {}),
      ('oam_stress', R / 'oam_stress/oam_stress.nes', 'b', 3000, {}),
      ('ppu_open_bus', R / 'ppu_open_bus/ppu_open_bus.nes', 'b', 900, {}),
      ('ppu_read_buffer (CNROM)', R / 'ppu_read_buffer/test_ppu_read_buffer.nes', 'b', 3000, {}),
      ('sprdma_and_dmc_dma', R / 'sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes', 'b', 900, {}),
      ('sprdma_and_dmc_dma_512', R / 'sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes', 'b', 900, {})]
for n in sorted((R / 'apu_test/rom_singles').glob('*.nes')):
    T.append(('apu_test/' + n.stem, n, 'b', 900, {}))
for n in sorted((R / 'mmc3_test_2/rom_singles').glob('*.nes')):
    T.append(('mmc3_test_2/' + n.stem, n, 'b', 900, {}))
for n in sorted((R / 'mmc3_test').glob('*.nes')):
    T.append(('mmc3_test/' + n.stem, n, 'b', 900, {}))
T += [('mmc1_a12 (visual)', R / 'MMC1_A12/mmc1_a12.nes', 'v', 120, {}),
      ('nes15 (homebrew, NROM)', R / 'nes15-1.0.0/nes15-NTSC.nes', 'v', 240, {'NES_PRESS': '120:08,126:00'}),
      ('240pee (homebrew, UxROM+CHR RAM)', R / '240pee/240pee.nes', 'v', 240, {}),
      ('high-hopes (demo, MMC3)', R / 'other/high-hopes.nes', 'v', 600, {}),
      ('BladeBuster (homebrew, MMC3)', R / 'other/BladeBuster.nes', 'v', 360, {}),
      ('quantum_disco_brothers (demo, CNROM)', R / 'other/quantum_disco_brothers_by_wAMMA.nes', 'v', 600, {}),
      ('oam3 (AxROM)', R / 'other/oam3.nes', 'v', 120, {}),
      ('fceuxd (Color Dreams, 11)', R / 'other/fceuxd.nes', 'v', 120, {}),
      ('m22chrbankingtest (VRC2, 22)', R / 'm22chrbankingtest/0-127.nes', 'v', 120, {}),
      ('pulsar (demo, MMC1)', R / 'other/pulsar.nes', 'v', 300, {}),
      ('smwstomp (MMC3)', R / 'stomper/smwstomp.nes', 'v', 300, {}),
      ('full_palette', R / 'full_palette/full_palette.nes', 'v', 60, {})]
# Holy Mapperel (pinobatch, zlib licence): mapper detection + bank/RAM tests, result on screen
HM = S.parent / 'testroms' / 'holy-mapperel' / 'testroms'
for n in ['M0_P32K_C8K_V', 'M1_P128K_C32K_S8K', 'M1_P512K_S8K', 'M2_P128K_V', 'M3_P32K_C32K_H', 'M4_P256K_C256K',
          'M7_P128K', 'M9_P128K_C64K', 'M10_P128K_C64K_S8K', 'M11_P64K_C64K_V', 'M66_P64K_C16K_V', 'M69_P128K_C64K_S8K']:
    T.append(('holy-mapperel ' + n, HM / (n + '.nes'), 'v', 2400 if n.startswith('M69') else 600, {}))
# save-state round trips: save at frame A, run K frames, load, run K frames: frames and audio must match
for nm, rom, a in [('quantum_disco_brothers (CNROM)', 'other/quantum_disco_brothers_by_wAMMA.nes', 240),
                   ('high-hopes (MMC3)', 'other/high-hopes.nes', 400), ('BladeBuster (MMC3)', 'other/BladeBuster.nes', 300),
                   ('smwstomp (MMC3)', 'stomper/smwstomp.nes', 200), ('pulsar (MMC1, CHR RAM)', 'other/pulsar.nes', 200),
                   ('240pee (UxROM, CHR RAM)', '240pee/240pee.nes', 150), ('nes15 (NROM)', 'nes15-1.0.0/nes15-NTSC.nes', 200)]:
    T.append(('savestate ' + nm, R / rom, 's', 0, {'NES_SS': f'{a}:60', 'NES_LAT': '40'}))

def run(item):
    name, rom, mode, frames, env = item
    d = OUT / re.sub(r'[^A-Za-z0-9_.-]+', '_', name)
    e = dict(os.environ, **env)
    if mode == 'b': e['NES_BLARGG'] = '1'
    if mode == 'f8': e['NES_F8'] = '1'
    d.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(BIN), str(rom), str(d), str(frames)], env=e,
                       capture_output=True, text=True, timeout=7200)
    log = p.stdout + p.stderr
    (d / 'log.txt').write_text(log)
    try:
        subprocess.run([sys.executable, '-I', str(S.parent / 'tools/ppm2png.py'), str(d / 'last.ppm')], check=False)
    except Exception:
        pass
    res = 'ERROR'
    m = re.search(r'blargg result=(\d+) text="([^"]*)"', log)
    if mode == 'b':
        res = ('PASS' if m.group(1) == '0' else 'FAIL #' + m.group(1)) + ' ' + m.group(2).replace('|', ' ').strip()[:90] if m else 'NO RESULT'
    elif mode == 'f8':
        m = re.search(r'f8 result=(\d+) \((.*?)\)', log)
        res = ('PASS' if m.group(1) == '1' else 'FAIL #' + m.group(1)) + ('' if 'final' in m.group(2) and 'NOT' not in m.group(2) else ' (not final)') if m else 'NO RESULT'
    elif mode == 's':
        m = re.search(r'save-state: (.*)', log)
        res = ('PASS' if 'PASS save-state' in log else 'FAIL') + ' ' + (m.group(1) if m else log.strip().splitlines()[-1][:100])
    else:
        res = 'see ' + str((d / 'last.png').relative_to(S.parent))
    if 'FAIL' in log and mode == 'v':
        res = 'FAIL ' + [l for l in log.splitlines() if 'FAIL' in l][0]
    hz = re.search(r'avg ([\d.]+) Hz.*stall_cycles=(\d+) bursts=(\d+)', log)
    return name, res, (hz.group(1) + ' Hz, stall ' + hz.group(2) + ', bursts ' + hz.group(3)) if hz else ''

if __name__ == '__main__':
    subprocess.run([str(S / 'run.sh')], check=True)    # build once, then run a private copy
    OUT.mkdir(exist_ok=True)
    import shutil
    shutil.copy2(S / 'build/obj/Vnes_sim_top', BIN)
    flt = sys.argv[1:]
    items = [t for t in T if not flt or any(f in t[0] for f in flt)]
    with ThreadPoolExecutor(int(os.environ.get('JOBS', '9'))) as ex:
        results = list(ex.map(run, items))
    lines = [f'{n:45s} {r}   [{x}]' for n, r, x in results]
    print('\n'.join(lines))
    (OUT / ('results.txt' if not flt else 'results_partial.txt')).write_text('\n'.join(lines) + '\n')
