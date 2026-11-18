#!/usr/bin/env python3
"""Magic Trick carts: Game Gear and Lynx games on a reflashable Game Boy cart.

  mtcart.py build OUT.gb GAME [GAME...]   dedicated cart image: a small Game Boy
                                          program (a title card), the
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



# The menu's 8x8 banner face (components/osd/system/banner_font.c), so the
# cart's title card matches the Magic Trick menu.
BANNER_C = Path(__file__).resolve().parent.parent / 'components/osd/system/banner_font.c'
LOGO_PPM = Path(__file__).resolve().parent / 'mtcart_logo.ppm'   # the Magic Trick wordmark
W, H = 160, 144
NAVY, WHITE, BLUE, SKY = (6, 10, 25), (244, 247, 249), (47, 103, 192), (91, 196, 255)
# The colors each 8x8 tile may use (the logo's own plus its anti-aliasing).
# Two in-between shades smooth the edges and still leave the screen within
# 8 palettes (with more, the red/yellow accent bar loses its palette).
CANDIDATES = [NAVY, WHITE, BLUE, SKY, (236, 50, 78), (255, 185, 92),
              (146, 175, 220), (26, 56, 108)]
HIDE_FRAMES = 180   # black for ~3 s at power-on, so the card only shows after the intro
DMG_BGP = 0x1b


def read_ppm(path):
    data = Path(path).read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[pos:pos + w * h * 3]
    return w, h, [[tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]


def banner_font():
    import re
    text = BANNER_C.read_text()
    body = text[text.index('Bits[]={'):text.index('};', text.index('Bits[]={'))]
    values = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body)]
    return {chr(32 + i): values[i * 8:i * 8 + 8] for i in range(95)}


def text_width(font, text, scale):
    # 10 px advance (5 for a space) with the menu's -1 letter spacing.
    if not text:
        return 0
    adv = sum((4 if c == ' ' else 9) * scale for c in text)
    return adv - (4 if text[-1] == ' ' else 1) * scale


def draw_text(pixels, font, text, x, y, scale, color):
    for c in text:
        rows = font.get(c, font['?'])
        for r, bits in enumerate(rows):
            for col in range(8):
                if bits & (0x80 >> col):
                    for dy in range(scale):
                        for dx in range(scale):
                            px, py = x + col * scale + dx, y + r * scale + dy
                            if 0 <= px < W and 0 <= py < H:
                                pixels[py][px] = color
        x += (4 if c == ' ' else 9) * scale


def centered(pixels, font, text, y, scale, color):
    draw_text(pixels, font, text, (W - text_width(font, text, scale)) // 2, y, scale, color)


def game_title(names):
    if len(names) != 1:
        return f'{len(names)} GAMES'
    # No-Intro names: drop region tags and subtitles,
    # "Ninja Gaiden III - The Ancient Ship of Doom (USA, Europe)" -> "NINJA GAIDEN III".
    stem = Path(names[0]).stem
    return (stem.split(' (')[0].split(' - ')[0].strip() or stem).upper()


def title_card(names):
    """The dedicated cart's screen: the Magic Trick logo, the game and
    LOADING..., shown after the intro while a Magic Trick Chromatic loads it."""
    font = banner_font()
    pixels = [[NAVY] * W for _ in range(H)]
    lw, lh, logo = read_ppm(LOGO_PPM)
    top, left = 12, (W - lw) // 2
    for y in range(lh):
        pixels[top + y][left:left + lw] = logo[y]
    title = game_title(names)
    while text_width(font, title, 1) > 152:
        title = title[:-1].rstrip()
    centered(pixels, font, title, 114, 1, WHITE)
    centered(pixels, font, 'LOADING...', 128, 1, SKY)
    return pixels


def _dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def _nearest(color, choices):
    return min(choices, key=lambda c: _dist(color, c))


def cgb_screen(pixels):
    """Game Boy Color background: each 8x8 tile picks one of 8 palettes of 4
    colors. Returns (tiles bank 0, tiles bank 1, map, attributes, palettes)."""
    blocks = []
    for ty in range(18):
        for tx in range(20):
            block = [_nearest(pixels[ty * 8 + r][tx * 8 + c], CANDIDATES) for r in range(8) for c in range(8)]
            counts = {}
            for c in block:
                counts[c] = counts.get(c, 0) + 1
            keep = sorted(counts, key=lambda c: -counts[c])[:4]
            blocks.append([_nearest(c, keep) for c in block])
    # Pack the tiles' color sets into at most 8 palettes.
    sets = sorted({frozenset(b) for b in blocks}, key=len, reverse=True)
    palettes = []
    for colors in sets:
        if any(colors <= p for p in palettes):
            continue
        fits = [p for p in palettes if len(p | colors) <= 4]
        if fits:
            best = min(fits, key=lambda p: len(p | colors))
            palettes[palettes.index(best)] = best | colors
        elif len(palettes) < 8:
            palettes.append(set(colors))
        else:
            sys.exit('title card needs more than 8 palettes')
    palettes = [sorted(p, key=lambda c: (c != NAVY, c)) for p in palettes]
    pal_bytes = bytearray()
    for p in palettes:
        p = p + [p[0]] * (4 - len(p))
        for r, g, b in p:
            pal_bytes += struct.pack('<H', (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10)
    pal_bytes += bytes(64 - len(pal_bytes))
    tiles, index, tmap, attrs = [], {}, bytearray(), bytearray()
    for block in blocks:
        # The palette that renders this tile with the least error.
        best = min(range(len(palettes)),
                   key=lambda i: sum(_dist(c, _nearest(c, palettes[i])) for c in block))
        pal = palettes[best]
        ids = [pal.index(_nearest(c, pal)) for c in block]
        tile = bytearray()
        for r in range(8):
            row = ids[r * 8:r * 8 + 8]
            tile.append(sum(0x80 >> i for i, v in enumerate(row) if v & 1))
            tile.append(sum(0x80 >> i for i, v in enumerate(row) if v & 2))
        tile = bytes(tile)
        if tile not in index:
            index[tile] = len(tiles)
            tiles.append(tile)
        n = index[tile]
        tmap.append(n & 0xff)
        attrs.append(best | (0x08 if n >= 256 else 0))
    if len(tiles) > 512:
        sys.exit(f'title card needs {len(tiles)} tiles (max 512)')
    return b''.join(tiles[:256]), b''.join(tiles[256:]), bytes(tmap), bytes(attrs), bytes(pal_bytes), palettes, blocks


def write_ppm(pixels, path, zoom=4):
    out = bytearray(f'P6\n{W * zoom} {H * zoom}\n255\n'.encode())
    for row in pixels:
        line = b''.join(bytes(c) * zoom for c in row)
        out += line * zoom
    Path(path).write_bytes(out)


def preview_pixels(blocks, palettes, attrs_unused=None):
    """What the Game Boy Color shows (tile colors after the palette fit)."""
    px = [[NAVY] * W for _ in range(H)]
    for i, block in enumerate(blocks):
        ty, tx = divmod(i, 20)
        best = min(palettes, key=lambda p: sum(_dist(c, _nearest(c, p)) for c in block))
        for j, c in enumerate(block):
            px[ty * 8 + j // 8][tx * 8 + j % 8] = _nearest(c, best)
    return px


def assemble(tiles0, tiles1, tmap, attrs, pal):
    """SM83 code at 0x150; data follows. Returns bank 0 (16 KiB).
    Loads the Game Boy Color background (both tile banks, attributes, map)
    with the LCD off and every palette black, keeps it black for
    HIDE_FRAMES (the Magic Trick intro plays over it), then sets the
    palettes and idles."""
    code = bytearray()
    fix = {}

    def emit(*b):
        code.extend(b)

    def ref(label):          # 16-bit address patched later
        fix[len(code)] = label
        code.extend(b'\0\0')

    def copy(dst, label, length):
        emit(0x21, *struct.pack('<H', dst))      # ld hl,dst
        emit(0x11); ref(label)                   # ld de,label
        emit(0x01, *struct.pack('<H', length))   # ld bc,length
        emit(0xcd); ref('copy')                  # call copy

    def copy_map(label):
        emit(0x21, 0x00, 0x98)                   # ld hl,$9800
        emit(0x11); ref(label)                   # ld de,label
        emit(0xcd); ref('copymap')               # call copymap

    wait_vblank = (0xf0, 0x44, 0xfe, 0x90, 0x20, 0xfa)   # .w: ldh a,[$44]; cp 144; jr nz,.w
    emit(0xf3)                                   # di
    emit(0x31, 0xfe, 0xff)                       # ld sp,$fffe
    emit(0xf0, 0x44, 0xfe, 0x90, 0x38, 0xfa)     # .w: ldh a,[$44]; cp 144; jr c,.w
    emit(0xaf, 0xe0, 0x40)                       # xor a; ldh [$40],a  (LCD off)
    emit(0x3e, 0x80, 0xe0, 0x68)                 # ld a,$80; ldh [$68],a  (BG palettes, auto-increment)
    emit(0x06, 64, 0xaf)                         # ld b,64; xor a
    emit(0xe0, 0x69, 0x05, 0x20, 0xfb)           # .z: ldh [$69],a; dec b; jr nz,.z  (all black)
    emit(0x3e, 0x01, 0xe0, 0x4f)                 # ld a,1; ldh [$4f],a  (VRAM bank 1)
    copy(0x8000, 'tiles1', len(tiles1))
    copy_map('attrs')
    emit(0xaf, 0xe0, 0x4f)                       # xor a; ldh [$4f],a  (VRAM bank 0)
    copy(0x8000, 'tiles0', len(tiles0))
    copy_map('map')
    emit(0x3e, DMG_BGP, 0xe0, 0x47)              # ld a,DMG_BGP; ldh [$47],a
    emit(0xaf, 0xe0, 0x42, 0xe0, 0x43)           # xor a; ldh [$42],a; ldh [$43],a
    emit(0x3e, 0x91, 0xe0, 0x40)                 # ld a,$91; ldh [$40],a  (LCD on)
    emit(0x0e, HIDE_FRAMES)                      # ld c,HIDE_FRAMES
    emit(*wait_vblank)                           # .d: wait for line 144
    emit(0xf0, 0x44, 0xfe, 0x90, 0x28, 0xfa)     #     then for it to pass
    emit(0x0d, 0x20, 0xf1)                       #     dec c; jr nz,.d
    emit(*wait_vblank)
    emit(0x3e, 0x80, 0xe0, 0x68)                 # ld a,$80; ldh [$68],a
    emit(0x21); ref('pal')                       # ld hl,pal
    emit(0x06, 64)                               # ld b,64
    emit(0x2a, 0xe0, 0x69, 0x05, 0x20, 0xfa)     # .p: ld a,[hl+]; ldh [$69],a; dec b; jr nz,.p
    emit(0x18, 0xfe)                             # .l: jr .l
    labels = {'copy': 0x150 + len(code)}
    emit(0x78, 0xb1, 0xc8)                       # copy: ld a,b; or c; ret z
    emit(0x1a, 0x13, 0x22, 0x0b, 0x78, 0xb1, 0x20, 0xf8, 0xc9)
    # .c: ld a,[de]; inc de; ld [hl+],a; dec bc; ld a,b; or c; jr nz,.c; ret
    labels['copymap'] = 0x150 + len(code)
    emit(0x0e, 18)                               # copymap: ld c,18
    emit(0x06, 20)                               # .r: ld b,20
    emit(0x1a, 0x13, 0x22, 0x05, 0x20, 0xfa)     # .k: ld a,[de]; inc de; ld [hl+],a; dec b; jr nz,.k
    emit(0xd5, 0x11, 12, 0x00, 0x19, 0xd1)       # push de; ld de,12; add hl,de; pop de
    emit(0x0d, 0x20, 0xef, 0xc9)                 # dec c; jr nz,.r; ret

    data = bytearray()
    for name, blob in (('tiles0', tiles0), ('tiles1', tiles1), ('map', tmap), ('attrs', attrs), ('pal', pal)):
        labels[name] = 0x150 + len(code) + len(data)
        data += blob
    for at, label in fix.items():
        code[at:at + 2] = struct.pack('<H', labels[label])
    bank = bytearray(b'\xff' * 0x4000)
    bank[0:0x100] = b'\0' * 0x100
    bank[0x100:0x104] = bytes([0x00, 0xc3, 0x50, 0x01])   # nop; jp $150
    bank[0x104:0x134] = LOGO
    body = bytes(code) + bytes(data)
    assert 0x150 + len(body) <= 0x4000, len(body)
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
    b.add_argument('games', nargs='*')
    b.add_argument('--capacity', type=lambda v: int(v, 0), default=4 << 20)
    b.add_argument('--preview', help='also write the title card as a PPM image')
    a = sub.add_parser('append')
    a.add_argument('base')
    a.add_argument('out')
    a.add_argument('games', nargs='+')
    a.add_argument('--capacity', type=lambda v: int(v, 0), default=4 << 20)
    l = sub.add_parser('list')
    l.add_argument('image')
    args = p.parse_args()

    if args.cmd == 'build':
        card = title_card(args.games)
        tiles0, tiles1, tmap, attrs, pal, palettes, blocks = cgb_screen(card)
        if args.preview:
            write_ppm(preview_pixels(blocks, palettes), args.preview)
        image = bytearray(assemble(tiles0, tiles1, tmap, attrs, pal))
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
