#!/usr/bin/env python3
"""Compile conservative Game Genie packs for exact, user-supplied GB/GBC/GG ROMs.

No ROM bytes are distributed: packs contain descriptions and sparse byte patches.
Input: libretro-database/cht, output: CHROMAGIC/CHEATS mirroring BACKUPS.
Database attribution/license must accompany the output. Never uses fuzzy titles.
"""
import argparse
import hashlib
import json
import re
import struct
import unicodedata
import zlib
from pathlib import Path

HEADER = struct.Struct('<4s7I')
ENTRY = struct.Struct('<96sIHH')
PATCH = struct.Struct('<IBBH')
MAX_ENTRIES = 256
MAX_PATCHES = 512
GAME_GENIE = re.compile(r'[0-9A-Fa-f]{3}[-+][0-9A-Fa-f]{3}(?:[-+][0-9A-Fa-f]{3})?')
LIVE_TOGGLE = re.compile(r'(?:turn|switch)(?:\s+\w+){0,3}\s+off|disable|(?:press|hold)\s+(?:select|start|button)', re.I)

def decode(code, gamegear=False):
    """GB Game Genie: value AB, address (FCDE xor F000), compare ror(GI,2)^BA."""
    if not GAME_GENIE.fullmatch(code):
        raise ValueError('unsupported code format')
    raw = re.sub('[-+]', '', code).upper()
    value = int(raw[:2], 16)
    address = int(raw[5] + raw[2:5], 16) ^ 0xF000
    if address >= (0xC000 if gamegear else 0x8000):
        raise ValueError('not a ROM code')
    compare = None
    if len(raw) == 9:
        encoded = int(raw[6] + raw[8], 16)
        compare = ((encoded >> 2) | (encoded << 6)) & 255
        compare ^= 0xBA
    return address, value, compare

def codes(text, gamegear=False):
    result = []
    end = 0
    for match in GAME_GENIE.finditer(text):
        if text[end:match.start()].strip(' +,;\t\r\n'):
            raise ValueError('mixed or unsupported code format')
        result.append(decode(match.group(), gamegear))
        end = match.end()
    if not result or text[end:].strip(' +,;\t\r\n'):
        raise ValueError('mixed or unsupported code format')
    if len(result) > 32:
        raise ValueError('too many codes in one option')
    return result

def patches(rom, text):
    """Resolve bank aliases conservatively. Reject any patch with a second CPU
    window alias, since load-time patching cannot reproduce a read interceptor.
    MBC1 multicarts/large MBC1 are deferred (different low-window mappings).
    """
    mapper = rom[0x147]
    banks = len(rom) // 16384
    if mapper not in (0, 8, 9, 1, 2, 3, 5, 6, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E):
        raise ValueError('unsupported mapper')
    if mapper in (1, 2, 3) and banks > 32:
        raise ValueError('large/multicart MBC1 mapping needs runtime support')
    # Repeated valid Nintendo logos identify possible MBC1 multicart images.
    if mapper in (1, 2, 3) and any(rom[b*16384+0x104:b*16384+0x134] == rom[0x104:0x134] for b in range(16, banks, 16)):
        raise ValueError('possible MBC1 multicart')
    result = {}
    for address, value, compare in codes(text):
        if mapper in (0, 8, 9):
            candidates = [address]
        elif address < 0x4000:
            candidates = [address]
        else:
            candidates = [bank*16384 + address-0x4000 for bank in range(1, banks)]
            if mapper >= 0x19:
                candidates.insert(0, address-0x4000)
        matched = False
        for offset in candidates:
            if offset >= len(rom):
                continue
            original = rom[offset]
            if compare is not None and original != compare:
                continue
            matched = True
            if original == value:
                continue
            if 0x100 <= offset <= 0x14F:
                raise ValueError('would modify cartridge boot/header bytes')
            if mapper >= 0x19 and offset < 0x4000:
                raise ValueError('MBC5 bank zero has two CPU window aliases')
            new = (original, value)
            if offset in result and result[offset] != new:
                raise ValueError('conflicting codes within one option')
            result[offset] = new
        if not matched:
            raise ValueError('compare byte does not match this ROM')
    if not result:
        raise ValueError('no changes for this ROM')
    if len(result) > MAX_PATCHES:
        raise ValueError('too many patches for one option')
    return [(offset, *values) for offset, values in sorted(result.items())]

def gg_codes(rom, text):
    """Runtime CPU-address/compare codes: bank mappings remain untouched.
    Layout matches the existing SMS_MiSTer CODES engine. Decode agrees with
    Genesis Plus GX's 8-bit Game Genie format; no source code copied.
    """
    size=len(rom)
    if size<8192 or size>4*1024*1024 or size&(size-1):
        raise ValueError('unsupported GG extent')
    header=0x7ff0 if size>=32768 else 0x3ff0 if size>=16384 else 0x1ff0
    if rom[header:header+8]!=b'TMR SEGA' or rom[header+15]>>4 not in (5,6,7):
        raise ValueError('not a Game Gear ROM')
    result={}
    for address,value,compare in codes(text,gamegear=True):
        if compare is None:
            raise ValueError('GG requires nine-digit Game Genie codes')
        offsets=[address%size] if address<0x400 or size<16384 else range(address%16384,size,16384)
        if not any(rom[offset]==compare for offset in offsets):
            raise ValueError('compare byte does not match this ROM')
        item=(compare,value,1)
        if address in result and result[address]!=item:
            raise ValueError('conflicting codes within one option')
        result[address]=item
    if not result or len(result)>32:
        raise ValueError('GG code limit')
    return [(address,*item) for address,item in sorted(result.items())]

def parse_cht(path):
    entries = {}
    for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        m = re.match(r'\s*cheat(\d+)_(desc|code)\s*=\s*"(.*)"\s*$', line)
        if m:
            entries.setdefault(int(m[1]), {})[m[2]] = m[3].replace('\\"', '"')
    return [e for _, e in sorted(entries.items()) if 'desc' in e and 'code' in e]

def identity(name):
    """Punctuation-insensitive exact title + exact region/revision tags only.
    Device labels are removed; revision/beta/sample distinctions are preserved.
    """
    name = re.sub(r'\s*\(Game Genie\)$', '', name, flags=re.I)
    groups = [g for g in re.findall(r'\(([^)]*)\)', name) if g.casefold() not in ('sgb enhanced', 'gb compatible', 'rumble version')]
    title = re.sub(r'\([^)]*\)', '', name).strip()
    title = ''.join(c for c in unicodedata.normalize('NFKD', title).casefold() if c.isalnum())
    tags = tuple(sorted(''.join(sorted(x.strip().casefold() for x in g.split(','))) for g in groups))
    return title, tags

def encode(rom, entries, gamegear=False):
    names, body = bytearray(), bytearray()
    for name, values in entries:
        ascii_name = unicodedata.normalize('NFKD', name).encode('ascii', 'ignore').decode()
        ascii_name = re.sub(r'\s+', ' ', ascii_name).strip()
        ascii_name = ''.join(c for c in ascii_name if 32 <= ord(c) < 127)[:95]
        if not ascii_name:
            raise ValueError('empty description')
        names += ENTRY.pack(ascii_name.encode(), len(body)//PATCH.size, len(values), 0)
        for item in values:
            offset, original, value = item[:3]
            body += PATCH.pack(offset, original, value, item[3] if gamegear else 0)
    payload = names + body
    return HEADER.pack(b'CMCH', 2 if gamegear else 1, len(rom), zlib.crc32(rom), len(entries), len(body)//PATCH.size, zlib.crc32(payload), 0) + payload

def prepare(rom_root, database, output):
    index = {}
    for system, directory in [('GB', 'Nintendo - Game Boy'), ('GBC', 'Nintendo - Game Boy Color'), ('GG', 'Sega - Game Gear')]:
        for path in sorted((database/'cht'/directory).glob('* (Game Genie).cht')):
            index.setdefault((system, identity(path.stem)), []).append(path)
    report = {'matched_games': [], 'unmatched_games': [], 'rejected': [], 'pack_count': 0, 'cheat_count': 0}
    for path in sorted(rom_root.rglob('*')):
        if not path.is_file() or path.suffix.lower() not in ('.gb', '.gbc', '.gg') or path.name.startswith('.'):
            continue
        relative = path.relative_to(rom_root)
        if relative.parts[0] not in ('GB', 'GBC', 'GG') or path.name.startswith('[BIOS]'):
            continue
        sources = index.get((relative.parts[0], identity(path.stem)), [])
        if not sources:
            report['unmatched_games'].append(str(relative)); continue
        rom = path.read_bytes()
        gamegear=relative.parts[0]=='GG'
        if len(rom) < (8192 if gamegear else 32768) or len(rom) % (8192 if gamegear else 16384) or len(rom) > 4*1024*1024:
            report['rejected'].append({'game':str(relative), 'reason':'unsupported ROM extent'}); continue
        options, seen = [], set()
        for source in sources:
            for e in parse_cht(source):
                try:
                    if LIVE_TOGGLE.search(e['desc']):
                        raise ValueError('requires live toggling/input; restart-only unsupported')
                    values = gg_codes(rom,e['code']) if gamegear else patches(rom,e['code'])
                    key = tuple(values)
                    if key in seen:
                        continue
                    if len(options) == MAX_ENTRIES:
                        raise ValueError('entry limit reached')
                    seen.add(key); options.append((e['desc'], values))
                except ValueError as exc:
                    report['rejected'].append({'game':str(relative),'description':e['desc'],'reason':str(exc)})
        if options:
            target = output/relative.parent/(relative.name+'.cmc')
            target.parent.mkdir(parents=True, exist_ok=True)
            data = encode(rom, options, gamegear=gamegear); target.write_bytes(data)
            report['pack_count'] += 1; report['cheat_count'] += len(options)
            report['matched_games'].append({'game':str(relative), 'rom_sha256':hashlib.sha256(rom).hexdigest(), 'pack_sha256':hashlib.sha256(data).hexdigest(), 'options':len(options), 'sources':[str(p.relative_to(database)) for p in sources]})
    return report

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rom-root', type=Path, required=True)
    p.add_argument('--database', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    report = prepare(a.rom_root, a.database, a.output)
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, indent=2)+'\n')
    (a.output/'LICENSE.txt').write_bytes((a.database/'LICENSE').read_bytes())
    (a.output/'ATTRIBUTION.txt').write_text('Cheat descriptions and codes adapted from libretro/libretro-database.\nhttps://github.com/libretro/libretro-database\nLicense: Creative Commons Attribution-ShareAlike 4.0 International.\nChanges: exact-title/region matching, code decoding, unsupported-code filtering,\nROM-specific physical patch compilation, and binary CMCH format conversion.\nAll options default off. Changes require relaunching the game.\nCommunity-sourced codes are not all hardware-tested.\n')
    print(json.dumps({k:report[k] for k in ['pack_count','cheat_count']}))

if __name__ == '__main__':
    main()
