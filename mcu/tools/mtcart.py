#!/usr/bin/env python3
"""Magic Trick carts: Game Gear and Lynx games on a reflashable Game Boy cart.

  mtcart.py build OUT.gb GAME [GAME...]   dedicated cart image: a small Game Boy
                                          program (explains the cart), the
                                          directory at 0x4000, then the games
  mtcart.py append BASE.gb OUT.gb GAME... an existing Game Boy ROM with games
                                          added after it (whole-cart writers)
  mtcart.py list IMAGE                    show and check an image's games

Write the image with a cart flasher (FlashGBX, ChroMagician). On the console,
`cartadd` adds games to an inserted cart's free flash without erasing it.
The format matches mcu/main/cart_store.h.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

BLOCK = 1024
DIR_MAGIC = b'MAGICTRICKCART01'
ENTRY_MAGIC = b'MTE1'
SLOTS, SLOT_BASE, SLOT_SIZE, NAME_SIZE = 10, 64, 96, 72
DEDICATED_DIR = 0x4000
EXTENSIONS = {'.gg': 1, '.lnx': 2, '.lyx': 2, '.o': 2, '.gb': 0, '.gbc': 0}
LOGO = bytes.fromhex('ceed6666cc0d000b03730083000c000d0008111f8889000edccc6ee6dddd'
                     'd999bbbb67636e0eecccdddc999fbbb9333e')

# 5x7 font for the stub's message (tile 0 is the space).
FONT = {
    ' ': ['     '] * 7,
    'A': [' ### ', '#   #', '#   #', '#####', '#   #', '#   #', '#   #'],
    'B': ['#### ', '#   #', '#   #', '#### ', '#   #', '#   #', '#### '],
    'C': [' ### ', '#   #', '#    ', '#    ', '#    ', '#   #', ' ### '],
    'D': ['#### ', '#   #', '#   #', '#   #', '#   #', '#   #', '#### '],
    'E': ['#####', '#    ', '#    ', '#### ', '#    ', '#    ', '#####'],
    'F': ['#####', '#    ', '#    ', '#### ', '#    ', '#    ', '#    '],
    'G': [' ### ', '#   #', '#    ', '# ###', '#   #', '#   #', ' ####'],
    'H': ['#   #', '#   #', '#   #', '#####', '#   #', '#   #', '#   #'],
    'I': [' ### ', '  #  ', '  #  ', '  #  ', '  #  ', '  #  ', ' ### '],
    'J': ['  ###', '   # ', '   # ', '   # ', '   # ', '#  # ', ' ##  '],
    'K': ['#   #', '#  # ', '# #  ', '##   ', '# #  ', '#  # ', '#   #'],
    'L': ['#    ', '#    ', '#    ', '#    ', '#    ', '#    ', '#####'],
    'M': ['#   #', '## ##', '# # #', '# # #', '#   #', '#   #', '#   #'],
    'N': ['#   #', '##  #', '# # #', '#  ##', '#   #', '#   #', '#   #'],
    'O': [' ### ', '#   #', '#   #', '#   #', '#   #', '#   #', ' ### '],
    'P': ['#### ', '#   #', '#   #', '#### ', '#    ', '#    ', '#    '],
    'Q': [' ### ', '#   #', '#   #', '#   #', '# # #', '#  # ', ' ## #'],
    'R': ['#### ', '#   #', '#   #', '#### ', '# #  ', '#  # ', '#   #'],
    'S': [' ####', '#    ', '#    ', ' ### ', '    #', '    #', '#### '],
    'T': ['#####', '  #  ', '  #  ', '  #  ', '  #  ', '  #  ', '  #  '],
    'U': ['#   #', '#   #', '#   #', '#   #', '#   #', '#   #', ' ### '],
    'V': ['#   #', '#   #', '#   #', '#   #', '#   #', ' # # ', '  #  '],
    'W': ['#   #', '#   #', '#   #', '# # #', '# # #', '# # #', ' # # '],
    'X': ['#   #', '#   #', ' # # ', '  #  ', ' # # ', '#   #', '#   #'],
    'Y': ['#   #', '#   #', ' # # ', '  #  ', '  #  ', '  #  ', '  #  '],
    'Z': ['#####', '    #', '   # ', '  #  ', ' #   ', '#    ', '#####'],
    '0': [' ### ', '#   #', '#  ##', '# # #', '##  #', '#   #', ' ### '],
    '1': ['  #  ', ' ##  ', '  #  ', '  #  ', '  #  ', '  #  ', ' ### '],
    '2': [' ### ', '#   #', '    #', '   # ', '  #  ', ' #   ', '#####'],
    '3': ['#####', '   # ', '  #  ', '   # ', '    #', '#   #', ' ### '],
    '4': ['   # ', '  ## ', ' # # ', '#  # ', '#####', '   # ', '   # '],
    '5': ['#####', '#    ', '#### ', '    #', '    #', '#   #', ' ### '],
    '6': ['  ## ', ' #   ', '#    ', '#### ', '#   #', '#   #', ' ### '],
    '7': ['#####', '    #', '   # ', '  #  ', ' #   ', ' #   ', ' #   '],
    '8': [' ### ', '#   #', '#   #', ' ### ', '#   #', '#   #', ' ### '],
    '9': [' ### ', '#   #', '#   #', ' ####', '    #', '   # ', ' ##  '],
    '.': ['     ', '     ', '     ', '     ', '     ', ' ##  ', ' ##  '],
    ',': ['     ', '     ', '     ', '     ', ' ##  ', '  #  ', ' #   '],
    ':': ['     ', ' ##  ', ' ##  ', '     ', ' ##  ', ' ##  ', '     '],
    '-': ['     ', '     ', '     ', '#####', '     ', '     ', '     '],
    "'": ['  #  ', '  #  ', ' #   ', '     ', '     ', '     ', '     '],
    '!': ['  #  ', '  #  ', '  #  ', '  #  ', '  #  ', '     ', '  #  '],
    '/': ['     ', '    #', '   # ', '  #  ', ' #   ', '#    ', '     '],
    '(': ['   # ', '  #  ', ' #   ', ' #   ', ' #   ', '  #  ', '   # '],
    ')': [' #   ', '  #  ', '   # ', '   # ', '   # ', '  #  ', ' #   '],
    '&': [' ##  ', '#  # ', '# #  ', ' #   ', '# # #', '#  # ', ' ## #'],
    '+': ['     ', '  #  ', '  #  ', '#####', '  #  ', '  #  ', '     '],
}
CHARS = list(FONT)
assert all(len(g) == 7 and all(len(r) == 5 for r in g) for g in FONT.values())


def font_tiles():
    rows = bytearray()
    for ch in CHARS:
        for r in FONT[ch] + ['     ']:
            rows.append(sum(0x40 >> i for i, c in enumerate(r) if c == '#'))
    return bytes(rows)


def text_map(lines):
    """18 rows x 20 columns of tile numbers."""
    out = bytearray()
    for row in range(18):
        line = lines[row] if row < len(lines) else ''
        line = line.upper()[:20].ljust(20)
        out += bytes(CHARS.index(c) if c in FONT else 0 for c in line)
    return bytes(out)


def stub_lines(names):
    lines = ['', '  MAGIC TRICK CART', '', 'GAME GEAR AND LYNX', 'GAMES FOR CHROMATIC.', '',
             'ON A CHROMATIC WITH', 'MAGIC TRICK, OPEN', 'THE MENU AND PICK', 'CARTRIDGE.', '']
    for name in names[:6]:
        lines.append(' ' + Path(name).stem[:19])
    return lines


def assemble(font, tmap, pal):
    """SM83 code at 0x150; data follows. Returns bank 0 (16 KiB)."""
    code = bytearray()
    fix = {}

    def emit(*b):
        code.extend(b)

    def ref(label):          # 16-bit address patched later
        fix[len(code)] = label
        code.extend(b'\0\0')

    emit(0xf3)                              # di
    emit(0x31, 0xfe, 0xff)                  # ld sp,$fffe
    emit(0xf0, 0x44, 0xfe, 0x90, 0x38, 0xfa)  # .w: ldh a,[$44]; cp 144; jr c,.w
    emit(0xaf, 0xe0, 0x40)                  # xor a; ldh [$40],a  (LCD off)
    emit(0x21, 0x00, 0x80)                  # ld hl,$8000
    emit(0x11); ref('font')                 # ld de,font
    emit(0x01, *struct.pack('<H', len(font)))  # ld bc,len
    emit(0x1a, 0x13, 0x22, 0x22, 0x0b, 0x78, 0xb1, 0x20, 0xf7)
    # .f: ld a,[de]; inc de; ld [hl+],a; ld [hl+],a; dec bc; ld a,b; or c; jr nz,.f
    emit(0x21, 0x00, 0x98, 0x01, 0x00, 0x04)  # ld hl,$9800; ld bc,$400
    emit(0xaf, 0x22, 0x0b, 0x78, 0xb1, 0x20, 0xf9)  # .c: xor a; ld [hl+],a; dec bc; ld a,b; or c; jr nz,.c
    emit(0x21, 0x00, 0x98)                  # ld hl,$9800
    emit(0x11); ref('map')                  # ld de,map
    emit(0x0e, 18)                          # ld c,18
    emit(0x06, 20)                          # .r: ld b,20
    emit(0x1a, 0x13, 0x22, 0x05, 0x20, 0xfa)  # .k: ld a,[de]; inc de; ld [hl+],a; dec b; jr nz,.k
    emit(0xd5, 0x11, 12, 0x00, 0x19, 0xd1)  # push de; ld de,12; add hl,de; pop de
    emit(0x0d, 0x20, 0xef)                  # dec c; jr nz,.r
    emit(0x3e, 0xe4, 0xe0, 0x47)            # ld a,$e4; ldh [$47],a  (DMG palette)
    emit(0x3e, 0x80, 0xe0, 0x68)            # ld a,$80; ldh [$68],a  (CGB BG palette 0)
    emit(0x21); ref('pal')                  # ld hl,pal
    emit(0x06, 8)                           # ld b,8
    emit(0x2a, 0xe0, 0x69, 0x05, 0x20, 0xfa)  # .p: ld a,[hl+]; ldh [$69],a; dec b; jr nz,.p
    emit(0xaf, 0xe0, 0x42, 0xe0, 0x43)      # xor a; ldh [$42],a; ldh [$43],a
    emit(0x3e, 0x91, 0xe0, 0x40)            # ld a,$91; ldh [$40],a  (LCD on)
    emit(0x18, 0xfe)                        # .l: jr .l

    labels = {}
    data = bytearray()
    for name, blob in (('font', font), ('map', tmap), ('pal', pal)):
        labels[name] = 0x150 + len(code) + len(data)
        data += blob
    for at, label in fix.items():
        code[at:at + 2] = struct.pack('<H', labels[label])
    bank = bytearray(b'\xff' * 0x4000)
    bank[0:0x100] = b'\0' * 0x100
    bank[0x100:0x104] = bytes([0x00, 0xc3, 0x50, 0x01])   # nop; jp $150
    bank[0x104:0x134] = LOGO
    body = bytes(code) + bytes(data)
    assert 0x150 + len(body) <= 0x4000
    bank[0x150:0x150 + len(body)] = body
    return bank


def finish_header(image, title=b'MAGIC TRICK'):
    image[0x134:0x143] = title.ljust(15, b'\0')[:15]
    image[0x143] = 0x80          # Game Boy Color compatible
    image[0x144:0x146] = b'00'
    image[0x146] = 0
    image[0x147] = 0x19          # MBC5
    size = len(image)
    code = (size // 0x8000).bit_length() - 1
    assert 0x8000 << code == size
    image[0x148] = code
    image[0x149] = 0
    image[0x14a] = 1
    image[0x14b] = 0x33
    image[0x14c] = 0
    image[0x14d] = (-sum(image[0x134:0x14d]) - (0x14d - 0x134)) & 0xff
    total = (sum(image) - image[0x14e] - image[0x14f]) & 0xffff
    image[0x14e:0x150] = struct.pack('>H', total)


def entry(name, offset, data):
    raw = name.encode('utf-8')
    if not 0 < len(raw) < NAME_SIZE or '/' in name or name.startswith('.'):
        sys.exit(f'bad game name: {name!r}')
    ext = Path(name).suffix.lower()
    if ext not in EXTENSIONS:
        sys.exit(f'unsupported game type: {name}')
    slot = bytearray(SLOT_SIZE)
    slot[0:4] = ENTRY_MAGIC
    slot[4] = EXTENSIONS[ext]
    slot[5:8] = b'\xff\xff\xff'
    slot[8:20] = struct.pack('<III', offset, len(data), zlib.crc32(data))
    slot[24:24 + len(raw)] = raw
    slot[20:24] = struct.pack('<I', zlib.crc32(bytes(slot)))   # CRC field zero while summed
    return bytes(slot)


def place_games(image, dir_offset, games):
    if len(games) > SLOTS:
        sys.exit(f'at most {SLOTS} games per cart')
    directory = bytearray(b'\xff' * BLOCK)
    directory[0:16] = DIR_MAGIC
    offset = dir_offset + BLOCK
    for i, path in enumerate(games):
        data = Path(path).read_bytes()
        directory[SLOT_BASE + i * SLOT_SIZE:SLOT_BASE + (i + 1) * SLOT_SIZE] = entry(Path(path).name, offset, data)
        end = offset + len(data)
        if len(image) < end:
            image.extend(b'\xff' * (end - len(image)))
        image[offset:end] = data
        offset = (end + BLOCK - 1) // BLOCK * BLOCK
    if len(image) < dir_offset + BLOCK:
        image.extend(b'\xff' * (dir_offset + BLOCK - len(image)))
    image[dir_offset:dir_offset + BLOCK] = directory


def pad_pow2(image):
    size = 0x8000
    while size < len(image):
        size *= 2
    image.extend(b'\xff' * (size - len(image)))


def parse(image):
    candidates = [DEDICATED_DIR]
    if image[0x148] <= 8:
        candidates.append(0x8000 << image[0x148])
    for d in candidates:
        block = image[d:d + BLOCK]
        if block[:16] != DIR_MAGIC:
            continue
        games = []
        for i in range(SLOTS):
            slot = bytearray(block[SLOT_BASE + i * SLOT_SIZE:SLOT_BASE + (i + 1) * SLOT_SIZE])
            if slot == b'\xff' * SLOT_SIZE:
                continue
            crc = struct.unpack('<I', slot[20:24])[0]
            slot[20:24] = b'\0\0\0\0'
            if slot[:4] != ENTRY_MAGIC or zlib.crc32(bytes(slot)) != crc:
                games.append((i, None))
                continue
            offset, size, data_crc = struct.unpack('<III', slot[8:20])
            name = bytes(slot[24:]).split(b'\0')[0].decode('utf-8', 'replace')
            ok = zlib.crc32(image[offset:offset + size]) == data_crc
            games.append((i, (name, offset, size, data_crc, ok)))
        return d, games
    return None, []


def check_size(image, capacity):
    if len(image) > capacity:
        sys.exit(f'image is {len(image)} bytes; the cart holds {capacity}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('out')
    b.add_argument('games', nargs='+')
    b.add_argument('--capacity', type=lambda v: int(v, 0), default=4 << 20)
    a = sub.add_parser('append')
    a.add_argument('base')
    a.add_argument('out')
    a.add_argument('games', nargs='+')
    a.add_argument('--capacity', type=lambda v: int(v, 0), default=4 << 20)
    l = sub.add_parser('list')
    l.add_argument('image')
    args = p.parse_args()

    if args.cmd == 'build':
        font = font_tiles()
        tmap = text_map(stub_lines(args.games))
        pal = struct.pack('<4H', 0x7fff, 0x56b5, 0x294a, 0x0000)
        image = bytearray(assemble(font, tmap, pal))
        place_games(image, DEDICATED_DIR, args.games)
        pad_pow2(image)
        finish_header(image)
        check_size(image, args.capacity)
        Path(args.out).write_bytes(image)
        print(f'{args.out}: {len(image)} bytes, {len(args.games)} games')
    elif args.cmd == 'append':
        image = bytearray(Path(args.base).read_bytes())
        if image[0x148] > 8 or len(image) != 0x8000 << image[0x148]:
            sys.exit('base ROM size does not match its header')
        base = len(image)
        place_games(image, base, args.games)
        check_size(image, args.capacity)
        Path(args.out).write_bytes(image)
        print(f'{args.out}: {len(image)} bytes; directory at 0x{base:x}')
    else:
        image = Path(args.image).read_bytes()
        d, games = parse(image)
        if d is None:
            sys.exit('no Magic Trick directory')
        print(f'directory at 0x{d:x}')
        bad = False
        for i, g in games:
            if g is None:
                print(f'  slot {i}: damaged')
                bad = True
                continue
            name, offset, size, crc, ok = g
            print(f'  slot {i}: {name}  offset=0x{offset:x} size={size} crc={crc:08x} {"ok" if ok else "BAD DATA"}')
            bad |= not ok
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
