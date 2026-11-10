#!/usr/bin/env python3
"""Lower only the capture engine for Gowin; retain VHDL as source of truth."""
from pathlib import Path
import subprocess,re
b=Path(__file__).resolve().parent;tools=b.parents[1]/'toolchains/oss-cad-suite/bin';d=b/'build/capture-lowered';d.mkdir(exist_ok=True)
c=b/'platform/src/rtl/EMU/CORE';u=b/'platform/src/Gameboy_MiSTer/rtl'
flags=['--std=08','-fsynopsys','-frelaxed']
with (b/'logs/capture-lowering-ghdl.log').open('w') as log:
 subprocess.run([tools/'ghdl','-a',*flags,c/'bus_savestates.vhd',u/'gb_savestates.vhd'],cwd=d,stdout=log,stderr=log,check=True)
 s=subprocess.run([tools/'ghdl','--synth',*flags,'--out=verilog','gb_savestates'],cwd=d,stdout=subprocess.PIPE,stderr=log,text=True,check=True).stdout
v=(u/'gb_savestates.vhd').read_text().split('end entity;')[0]
for p in re.findall(r'^\s*(\w+)\s*:\s*(?:in|out|buffer)\b',v,re.M):s=re.sub(r'\b'+p.lower()+r'\b',p,s)
(d/'engine.v').write_text(s)
with (b/'logs/capture-lowering.log').open('w') as log:
 subprocess.run([tools/'yosys','-p','read_verilog engine.v; hierarchy -top gb_savestates; proc; opt; fsm; opt; clean; stat; write_verilog -noattr engine_opt.v'],cwd=d,stdout=log,stderr=log,check=True)
p=b/'platform/src/rtl/EMU/capture_engine_lowered.v';p.write_bytes((d/'engine_opt.v').read_bytes())
p=b/'platform/build.tcl';s=p.read_text().replace('add_file -type vhdl "src/Gameboy_MiSTer/rtl/gb_savestates.vhd"','add_file -type verilog "src/rtl/EMU/capture_engine_lowered.v"');p.write_text(s)
