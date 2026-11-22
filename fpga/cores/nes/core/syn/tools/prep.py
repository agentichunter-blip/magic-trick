"""Generate patched synthesis sources for the Gowin NES probe from the pristine clone.

Patches (copies only; the clone stays untouched):
  * every SV file using SSREG_* constants gets `import regs_savestates::*;` (Quartus leaks the
    import from ppu.sv into the compilation unit; Gowin does not).
  * `always begin` (implicit sensitivity, Quartus accepts) -> `always @* begin`.
  * cart.sv is reduced to the selected mapper modules (cart_full.sv / cart_small.sv); expansion
    audio (SS5B, VRC6/7, N163, MMC5, FDS), FDS, NSF, mapper 16 EEPROM and mapper 413 removed.
"""
import re, sys
from pathlib import Path

work = Path(sys.argv[1]).resolve()            # nes-work
rtl = work / 'NES_MiSTer' / 'rtl'
out = work / 'syn' / 'src'
out.mkdir(parents=True, exist_ok=True)

def patch(text, need_import):
    text = re.sub(r'\balways\s+begin\b', 'always @* begin', text)
    if need_import and 'import regs_savestates::*;' not in text:
        text = 'import regs_savestates::*;\n' + text
    return text

copies = {
    'nes.v': 'nes.sv', 'ppu.sv': 'ppu.sv', 'apu.sv': 'apu.sv', 'cheatcodes.sv': 'cheatcodes.sv',
    'composite_board.sv': 'composite_board.sv', 'regs_savestates.sv': 'regs_savestates.sv',
    'mappers/generic.sv': 'generic.sv', 'mappers/MMC1.sv': 'MMC1.sv', 'mappers/MMC2.sv': 'MMC2.sv',
    'mappers/MMC3.sv': 'MMC3.sv', 'mappers/VRC.sv': 'VRC.sv', 'mappers/Sunsoft.sv': 'Sunsoft.sv',
}
for src, dst in copies.items():
    t = (rtl / src).read_text(errors='replace')
    (out / dst).write_text(patch(t, 'SSREG_' in t and src != 'regs_savestates.sv'))
for f in ['T65_Pack.vhd', 'T65_ALU.vhd', 'T65_MCode.vhd', 'T65.vhd']:
    (out / f).write_text((rtl / 't65' / f).read_text(errors='replace'))
for f in ['bus_savestates.vhd', 'savestates.vhd', 'statemanager.vhd']:
    (out / f).write_text((rtl / f).read_text(errors='replace'))

# ---- reduced cart_top ----
lines = (rtl / 'cart.sv').read_text(errors='replace').split('\n')
banner = re.compile(r'^//\*{20,}//$')
# header = up to (not including) the MMC0 comment / instance
hdr_end = next(i for i, l in enumerate(lines) if l.startswith('// This mapper used to be default'))
tail_start = next(i for i, l in enumerate(lines) if l.startswith('wire [15:0] ss5b_audio'))
# blocks: start at a banner line that is followed (within 7 lines) by another banner; end before next block
starts = []
i = hdr_end
while i < tail_start:
    if banner.match(lines[i]):
        j = next(k for k in range(i + 1, i + 12) if banner.match(lines[k]))
        starts.append(i)
        i = j + 1
    else:
        i += 1
starts.append(tail_start)
blocks = []
for a, b in zip(starts, starts[1:]):
    body = lines[a:b]
    m = next((re.match(r'^(\w+)\s+(\w+)\s*\(?\s*$', l) for l in body if re.match(r'^(\w+)\s+(\w+)\s*\(?\s*$', l) and not l.startswith(('wire', 'tri', 'reg'))), None)
    blocks.append((m.group(1) if m else None, body))

def make(name, keep):
    kept = ['\n'.join(b) for t, b in blocks if t in keep]
    missing = keep - {t for t, _ in blocks}
    assert not missing, missing
    text = '\n'.join(kept)
    text = text.replace('.audio_in   (ss5b_audio)', '.audio_in   (audio_in)')
    idx = sorted({int(x) for x in re.findall(r'SaveStateBus_wired_or\[(\d+)\]', text)})
    assert 'SaveStateRAM_wired_or' not in text
    tail = f'''
reg [1023:0] me;

always @* begin
	me = 1024'd0;
	me[{{flags[18:17],flags[7:0]}}] = 1'b1;

	{{prg_aout[21:0], prg_allow,   chr_aout,   vram_a10,   vram_ce,   chr_allow,   prg_dout,   chr_dout,   irq,   audio}} =
	{{prg_addr_b,     prg_allow_b, chr_addr_b, vram_a10_b, vram_ce_b, chr_allow_b, prg_dout_b, chr_dout_b, irq_b, audio_out_b}};

	{{mapper_addr, mapper_data_out, mapper_prg_write, mapper_ovr}} = 28'd0;
	prg_aout[24:22] = 3'd0;
	diskside = 2'd0;

	has_chr_dout    = flags_out_b[0];
	prg_bus_write   = flags_out_b[1];
	prg_conflict    = flags_out_b[2];
	has_savestate   = flags_out_b[3];
	prg_conflict_d0 = flags_out_b[4];
	has_flashsaves  = flags_out_b[5];

	if ((prg_aout[21] == 1'b0) && (prg_aout[24] == 1'b0))
		prg_aout[20:0] = {{(prg_aout[20:11] & prg_mask[9:0]), prg_aout[10:0]}};

	if (chr_aout[21:20] == 2'b10)
		chr_aout[19:0] = {{(chr_aout[19:11] & chr_mask[8:0]), chr_aout[10:0]}};

	chr_aout = vram_ce ? {{11'b11_1010_0000_0, vram_a10, chr_ain[9:0]}} : chr_aout;
	prg_aout = (prg_ain < 'h2000) ? {{11'b11_1000_0000_0, prg_ain[10:0]}} : prg_aout;
	prg_allow = prg_allow || (prg_ain < 'h2000);

	if (vs_prg_ram) begin
		prg_aout = {{3'b000, vs_prg_ram_addr}};
		prg_allow = 1'b1;
		prg_bus_write = 1'b0;
		prg_conflict = 1'b0;
		prg_conflict_d0 = 1'b0;
	end

	if (vs_nametable_ram) begin
		chr_aout = vs_nametable_ram_addr;
		chr_allow = 1'b1;
		vram_ce = 1'b0;
		has_chr_dout = 1'b0;
	end
end

localparam SAVESTATE_MODULES = 41;
wire [63:0] SaveStateBus_wired_or[0:SAVESTATE_MODULES-1];
assign SaveStateBus_Dout = {' | '.join(f'SaveStateBus_wired_or[{n}]' for n in idx)};
assign Savestate_MAPRAMReadData = 8'd0;

endmodule
'''
    hdr = '\n'.join(lines[:hdr_end])
    hdr = 'import regs_savestates::*;\n// GENERATED by syn/tools/prep.py from NES_MiSTer rtl/cart.sv: mappers ' + ', '.join(sorted(keep)) + '\n' + hdr
    hdr = hdr.replace('wire [2:0] prg_aoute_m413;', '')
    text = patch(text, False)
    (out / name).write_text(hdr + '\n' + text + '\n' + tail)
    print(name, sorted(keep), 'ss idx', idx)

make('cart_full.sv', {'MMC1', 'Mapper28', 'MMC2', 'MMC3', 'MMC4', 'Mapper66', 'Mapper69', 'Mapper71', 'VRC24'})
make('cart_small.sv', {'MMC1', 'Mapper28', 'MMC3'})

# ---- declaration fixes: Quartus accepts procedural assignment to wires / plain outputs ----
# decl_fixes.txt lines: <file> <line> <ident>   (collected from Gowin EX3900 errors by collect_fixes.py)
fixfile = Path(__file__).with_name('decl_fixes.txt')
if fixfile.exists():
    todo = {}
    for l in fixfile.read_text().split():
        pass
    for row in fixfile.read_text().splitlines():
        if not row.strip():
            continue
        f, ln, ident = row.split()
        todo.setdefault(f, []).append((int(ln), ident))
    for f, items in todo.items():
        p = out / f
        L = p.read_text().split('\n')
        for ln, ident in items:
            i = ln - 1
            ms = next(k for k in range(i, -1, -1) if re.match(r'^\s*module\b', L[k]))
            done = False
            for k in range(ms, len(L)):
                s = L[k]
                if re.search(r'\b' + re.escape(ident) + r'\b', s) and re.match(r'^\s*(output|wire)\b', s):
                    if s.lstrip().startswith('wire') and '=' in s:
                        continue
                    s2 = re.sub(r'^(\s*)output\s+wire\b', r'\1output logic', s)
                    if s2 == s:
                        s2 = re.sub(r'^(\s*)output\b(?!\s+(reg|logic)\b)', r'\1output logic', s)
                    if s2 == s:
                        s2 = re.sub(r'^(\s*)wire\b', r'\1logic', s)
                    L[k] = s2
                    done = True
                    break
            if not done:
                print('WARN no decl for', f, ln, ident)
        p.write_text('\n'.join(L))
    print('applied', sum(len(v) for v in todo.values()), 'decl fixes')

# ---- hoist forward-referenced module-level declarations ----
# Quartus accepts a net used before its `wire` declaration; Gowin turns the early use into an
# undriven implicit net (seen: color0, apu_cs, SaveStateBus_*, ppu_dbus ...), which silently
# constant-folds large parts of the PPU/APU. Move such declarations (column-0 only) to the top
# of the module body; `wire x = expr;` becomes `wire x;` + `assign x = expr;`.
decl_re = re.compile(r'^(wire|reg|logic|tri0|tri1)\b(\s+signed)?\s*(\[[^\]]*\])?\s*([^=;]+?)\s*(=\s*(.*?))?;\s*(//.*)?$')
lp_re = re.compile(r'^(localparam|parameter)\b[^;]*?\b(\w+)\s*=.*;')
def strip_comment(s):
    return s.split('//')[0]
def hoist_file(p):
    L = p.read_text().split('\n')
    i, nh = 0, 0
    res = []
    while i < len(L):
        if re.match(r'^module\b', L[i]):
            end = next(k for k in range(i, len(L)) if re.match(r'^endmodule\b', L[k]))
            hdr = next(k for k in range(i, end) if re.match(r'^\s*\)\s*;', L[k]) or (k == i and L[k].rstrip().endswith(';')))
            body = L[hdr + 1:end]
            lps = {}
            for k, s in enumerate(body):
                m = lp_re.match(s)
                if m: lps[m.group(2)] = k
            hoisted, hoist_lp = [], set()
            # join multi-line column-0 declarations into one statement (kept in body[k], others emptied)
            span = {}
            for k in range(len(body)):
                if re.match(r'^(wire|reg|logic)\b', body[k]) and ';' not in strip_comment(body[k]):
                    e = k
                    while ';' not in strip_comment(body[e]):
                        e += 1
                    span[k] = e
            for k, s in enumerate(body):
                if k in span:
                    s = ' '.join(strip_comment(x).strip() for x in body[k:span[k] + 1])
                m = decl_re.match(s)
                if not m or m.group(1) in ('reg', 'logic') and m.group(5):
                    continue
                if m.group(5) and '[' in m.group(4):
                    continue
                names = [re.sub(r'\[.*', '', n).strip() for n in m.group(4).split(',')]
                if not all(re.fullmatch(r'\w+', n) for n in names):
                    continue
                before = '\n'.join(strip_comment(x) for x in body[:k])
                if not any(re.search(r'\b' + n + r'\b', before) for n in names):
                    continue
                typ = m.group(1) + (m.group(2) or '') + (' ' + m.group(3) if m.group(3) else '')
                for ident, kk in lps.items():
                    if kk > 0 and re.search(r'\b' + ident + r'\b', m.group(4) + (m.group(3) or '')):
                        hoist_lp.add(kk)
                if m.group(5):
                    assert len(names) == 1, s
                    hoisted.append(f'{typ} {m.group(4)};')
                    body[k] = f'assign {names[0]} = {m.group(6)};' + (' ' + m.group(7) if m.group(7) else '')
                    for e in range(k + 1, span.get(k, k) + 1):
                        body[e] = ''
                else:
                    hoisted.append(s)
                    body[k] = '// (hoisted) ' + s
                    for e in range(k + 1, span.get(k, k) + 1):
                        body[e] = ''
            lp_lines = [body[k] for k in sorted(hoist_lp)]
            for k in hoist_lp:
                body[k] = '// (hoisted) ' + body[k]
            nh += len(hoisted)
            res += L[i:hdr + 1] + (['// ---- declarations hoisted by syn/tools/prep.py ----'] + lp_lines + hoisted if hoisted else []) + body + [L[end]]
            i = end + 1
        else:
            res.append(L[i]); i += 1
    p.write_text('\n'.join(res))
    return nh
for p in sorted(out.glob('*.sv')):
    n = hoist_file(p)
    if n: print('hoisted', n, 'decls in', p.name)

# ---- experimental variant: OAM in block RAM (ppu_oam.sv) ----
# Estimate only (not cycle-verified): single-port OAM with registered read, no extra-sprite
# evaluation, no OAM row-corruption emulation, 32-byte secondary OAM without array reset.
t = (out / 'ppu.sv').read_text()
def rep(old, new, regex=False):
    global t
    if regex:
        t2, n = re.subn(old, new, t, count=1, flags=re.S)
    else:
        n = t.count(old); t2 = t.replace(old, new, 1)
    assert n >= 1, old[:80]
    t = t2
rep("reg [7:0] oam_temp[64];", "reg [7:0] oam_temp[32];")
rep("if (Savestate_OAMRdEn) Savestate_OAMReadData  <= oam[Savestate_OAMAddr];\n", "")
rep("if (Savestate_OAMWrEn) oam[Savestate_OAMAddr] <= Savestate_OAMWriteData;\n", "")
rep("\toam_temp <= '{64{8'hFF}};\n", "")
rep(r"\tif \(\(\(~old_rendering && rendering\) \|\| corrupting_write\) && ~PAL\) begin.*?oam_temp\[oam_row_cur\] <= oam_temp\[oam_row_last\];\s*end\s*end\n", "", True)
rep(r"\t+// During init, we hunt for the 8th sprite.*?spr_counter <= spr_counter \+ 1'b1;\s*end\n", "", True)
rep(r"\t+// Clear extra sprite space too\s*oam_temp\[\{1'b1, oam_secondary_addr\}\] <= 8'hFF;\n", "", True)
rep(r"\t+if \(&spr_counter && ~ex_ovr\) begin.*?oam\[\{oam_addr_ex, 2'b11\}\];\s*end\s*end\s*end\n", "", True)
rep("(oam[oam_read_addr] & (is_attr_byte ? 8'hE3 : 8'hFF))", "(oam_q & (is_attr_byte ? 8'hE3 : 8'hFF))")
rep(r"\t+last_y <= oam\[\{oam_addr\[7:2\], 2'b00\}\];.*?repeat_count <= repeat_count \+ 3'd1;\s*end\n", "", True)
rep(r"\t+// Check if the 9th sprite is a repeat.*?repeat_count <= repeat_count \+ 3'd1;\n", "", True)
rep(r"\t+if \(cycle\[2:0\] == 3'd0\) begin\s*oam_bus_ex <= \{.*?\};\s*end\n", "", True)
rep("oam_data <= oam[oam_read_addr]; // Keep it available", "oam_data <= oam_q; // Keep it available")
rep("\t\t\toam[oam_read_addr] <= (is_attr_byte) ? (oam_din & 8'hE3) : oam_din; // attr has no bits 2-4\n", "")
rep("always @(posedge clk) begin :oam_eval", """// OAM as one read/write port RAM (probe variant)
reg [7:0] oam_q;
wire oam_ss_sel = Savestate_OAMRdEn | Savestate_OAMWrEn;
wire [7:0] oam_ra = oam_ss_sel ? Savestate_OAMAddr : oam_read_addr;
wire oam_we = Savestate_OAMWrEn | (ce & oam_data_write & ~rendering);
wire [7:0] oam_wd = Savestate_OAMWrEn ? Savestate_OAMWriteData : (is_attr_byte ? (oam_din & 8'hE3) : oam_din);
always @(posedge clk) begin
	// write-through (GW5A BSRAM has no read-before-write mode); oam_q settles within a dot
	if (oam_we) begin oam[oam_ra] <= oam_wd; oam_q <= oam_wd; end
	else oam_q <= oam[oam_ra];
end
assign Savestate_OAMReadData = oam_q;
always @(posedge clk) begin :oam_eval""")
assert 'oam[' not in re.sub(r'oam\[(oam_ra|256)\]', '', t[t.find('module OAMEval'):t.find('module SpriteAddressGen')]), 'stray oam[] access'
(out / 'ppu_oam.sv').write_text(t)
print('wrote ppu_oam.sv')
