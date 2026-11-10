#!/usr/bin/env python3
"""Compile the upstream SMS/Game Gear logic for host simulation; never program hardware."""
from pathlib import Path
import re, subprocess, os
root=Path(__file__).resolve().parents[1]
rtl=root/'vendor/SMS_MiSTer/rtl'
build=root/'build'
adapt=root/'rtl'
def write_changed(path, text):
    if not path.exists() or path.read_text()!=text: path.write_text(text)
ghdl=root.parent/'toolchains/oss-cad-suite/bin/ghdl'
# Replace Intel-only RAM primitives with inferred RAM for the prototype.
for name in ('spram','dpram','sprom'):
    source=(rtl/(name+'.vhd')).read_text()
    header=source[:re.search(r'ARCHITECTURE',source,re.I).start()]
    header=re.sub(r'LIBRARY altera_mf;\s*USE altera_mf.all;', 'use ieee.numeric_std.all;',header,flags=re.I)
    decl='type mem_t is array (0 to 2**widthad_a-1) of std_logic_vector(width_a-1 downto 0);\n'
    if name=='sprom':
        data={}
        for addr,vals in re.findall(r'^([0-9A-F]+): ([0-9A-F ]+);', (rtl/'mboot.mif').read_text(),re.M):
            for offset,val in enumerate(vals.split()): data[int(addr,16)+offset]=int(val,16)
        decl+='constant mem : mem_t := (\n'+',\n'.join(f'{a} => std_logic_vector(to_unsigned({v},width_a))' for a,v in data.items() if v)+",\nothers => (others=>'0'));\n"
    else:
        decl+="signal mem : mem_t := (others => (others => '0'));\n"
    if name=='dpram':
        # Both ports in the SMS VDP are driven by clk_sys. Do not use this
        # prototype implementation for independent-clock RAM.
        body='''process(clock_a) begin
 if rising_edge(clock_a) then
  q_a <= mem(to_integer(unsigned(address_a)));
  q_b <= mem(to_integer(unsigned(address_b)));
  if wren_a='1' then mem(to_integer(unsigned(address_a))) <= data_a; q_a <= data_a; end if;
  if wren_b='1' then mem(to_integer(unsigned(address_b))) <= data_b; q_b <= data_b; end if;
 end if;
end process;
'''
    else:
        body='process(clock) begin\n if rising_edge(clock) then\n  q <= mem(to_integer(unsigned(address)));\n'
        if name=='spram': body+="  if wren='1' then mem(to_integer(unsigned(address))) <= data; q <= data; end if;\n"
        body+=' end if;\nend process;\n'
    write_changed(adapt/(name+'.vhd'),header+'architecture prototype of '+name+' is\n'+decl+'begin\n'+body+'end;\n')
files=[p for p in rtl.rglob('*.vhd') if p.name not in ('spram.vhd','dpram.vhd','sprom.vhd') and '/pll/' not in str(p) and '/VM2413/' not in str(p)]
converted=[]
for p in files:
    dest=build/'vhdl'/p.relative_to(rtl)
    dest.parent.mkdir(parents=True,exist_ok=True)
    src=p.read_text(encoding='latin-1')
    src=re.sub(r'\bRelease\b','ReleasePhase',src,flags=re.I)
    src=re.sub(r':\s*work\.', ': entity work.', src)
    if p.name=='system.vhd':
        # Game Gear has no YM2413 FM synthesizer; omit SMS-only FM block.
        src=re.sub(r'fm: entity work.opll.*?\);', "FM_out <= (others => '0');", src, flags=re.S)
        # This prototype boots cartridge ROM directly; BIOS inputs are tied low
        # in gg_sim_top. Omit unused BIOS stores before emitting the netlist.
        for instance,output in [('boot_rom_inst','boot_rom_D_out'),('ext_bios_inst','ext_bios_D_out'),('ext_gg_bios_inst','ext_gg_bios_D_out')]:
            src=re.sub(instance+r'\s*:\s*entity work\.\w+.*?\);',output+" <= (others => '0');",src,flags=re.S)

    if p.name=='vdp_sprites.vhd':
        states=dict(re.findall(r'constant\s+(\w+)\s*:\s*std_logic_vector\s*:=\s*"([01]+)"',src,re.I))
        src=re.sub(r"'([01])'\s*&\s*(\w+)",lambda m:'"'+m[1]+states[m[2]]+'"' if m[2] in states else m[0],src)
    if p.name=='mapper_ctrl.vhd':
        # Upstream covers binary values only. Explicitly retain state for X/Z.
        src=re.sub(r'(case A\(1 downto 0\) is)(.*?)(end case;)',
            lambda m:m[1]+m[2]+('when others => null;\n' if 'when others' not in m[2] else '')+m[3],src,flags=re.S)
    if p.name=='T80.vhd': src=src.replace('ioq and x"7"', 'ioq and "000000111"')
    write_changed(dest,src)
    converted.append(dest)
files=converted+list(adapt.glob('*.vhd'))
flags=['--std=08','-fsynopsys','-frelaxed']
with (root/'logs/ghdl.log').open('w') as log:
    subprocess.run([str(ghdl),'-i',*flags,*map(str,files)],cwd=build,stdout=log,stderr=log,check=True)
    for top in ('system','video'):
        subprocess.run([str(ghdl),'-m',*flags,top],cwd=build,stdout=log,stderr=log,check=True)
        with (build/(top+'.v')).open('w') as out:
            subprocess.run([str(ghdl),'--synth',*flags,'--out=verilog',top],cwd=build,stdout=out,stderr=log,check=True)
    netlist=build/'system.v'
    netlist.write_text(re.sub(r'\bpriority\b','bg_priority_flag',netlist.read_text()))
print('VHDL translated to Verilog. See logs/ghdl.log for diagnostics.')
