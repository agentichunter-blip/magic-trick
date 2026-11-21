"""Magic Trick carts: Game Gear, Lynx and Game Boy games on a reflashable Game
Boy cart (ModRetro carts), for the installer's Cartridge tab and mcu/tools/mtcart.py.

A cart either keeps its own Game Boy game (games go in the flash after it) or
is a dedicated Magic Trick cart (a small Game Boy program showing a title
card, then the games). The directory format matches mcu/main/cart_store.h:

  version 2 (written here): 64-byte header "MAGICTRICKCART02", entry count,
  CRC-32 of the entries, directory bytes, CRC-32 of the header's first 28
  bytes; then 128-byte entries "MTE2", system, flags, offset, size, CRC, art
  offset, art size, art CRC, 72-byte name. No fixed game limit.
  version 1 (read only): 10 slots of 96 bytes, written by the console's cartadd.

Writing: the cart is changed 64 KiB block by block (every supported chip's
erase sectors fit inside one), and a block that already holds the wanted
bytes is never touched, so a kept game stays exactly as it was.
"""
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

BLOCK = 1024
ERASE_BLOCK = 64 * 1024
DEDICATED_DIR = 0x4000
NAME_SIZE = 72
DIR1_MAGIC, ENTRY1_MAGIC = b'MAGICTRICKCART01', b'MTE1'
DIR2_MAGIC, ENTRY2_MAGIC = b'MAGICTRICKCART02', b'MTE2'
SLOTS1, SLOT1_BASE, SLOT1_SIZE = 10, 64, 96
DIR2_HEADER, ENTRY2_SIZE = 64, 128
SYSTEMS = {'.gg': 1, '.lnx': 2, '.lyx': 2, '.o': 2, '.gb': 0, '.gbc': 0}
SYSTEM_NAMES = {0: 'Game Boy', 1: 'Game Gear', 2: 'Atari Lynx'}
ART_SIZE = 16 + 128 * 92 * 2         # the menu's .cma box art (library.COVER_BYTES)
ASSETS = Path(__file__).resolve().parent / 'assets'

# Saves and states live on the microSD card, or with no card in the
# Chromatic's internal store (mcu/main/sd_card.c: FAT on MCU flash
# 0x110000-0x25F000, less wear levelling and FAT overhead).
STORE_BYTES = 1_300_000
STATE_SLOTS = 3
STATE_HEADER = 64
GB_STATE_CORE = 8 + 128 * 8 + 32768 + 16384 + 160 + 128
GG_STATE, LYNX_STATE = 57 * 1024, 65 * 1024
GG_SAVE_MAX, GG_MIRROR = 32 * 1024, 57 * 1024
CLUSTER = 4096


class CartError(Exception):
    pass


class CartFull(CartError):
    def __init__(self, needed, capacity):
        super().__init__('These games need %d KB; the cart holds %d KB.' % (needed // 1024, capacity // 1024))
        self.needed, self.capacity = needed, capacity


@dataclass
class Game:
    name: str
    data: bytes
    art: bytes = None          # a .cma file (header + 72x80 RGB565), or None

    @property
    def system(self):
        return SYSTEMS[Path(self.name).suffix.lower()]


@dataclass
class Cart:
    """What a cart image holds."""
    capacity: int
    own_title: str = None      # the cart's own Game Boy game, if kept
    own_size: int = 0          # bytes it spans from offset 0
    dedicated: bool = False
    version: int = 0           # directory version, 0 for none
    games: list = field(default_factory=list)
    damaged: int = 0           # entries whose data doesn't match their CRC


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def check_name(name):
    raw = name.encode('utf-8')
    if not 0 < len(raw) < NAME_SIZE or '/' in name or '\\' in name or name.startswith('.'):
        raise CartError('Game names must be 1 to %d characters: %r' % (NAME_SIZE - 1, name))
    if Path(name).suffix.lower() not in SYSTEMS:
        raise CartError('Not a Game Boy, Game Gear or Lynx game: %s' % name)
    return raw


def short_name(name):
    """Fit a file name in the directory's 72 bytes, keeping its extension."""
    stem, ext = Path(name).stem, Path(name).suffix
    while len((stem + ext).encode('utf-8')) >= NAME_SIZE:
        stem = stem[:-1].rstrip()
    return stem + ext


def header_ok(image):
    if len(image) < 0x150:
        return False
    total = 0
    for b in image[0x134:0x14d]:
        total = (total - b - 1) & 0xff
    return total == image[0x14d]


def header_title(image):
    color = image[0x143] & 0x80
    raw = bytes(image[0x134:0x143 if color else 0x144]).split(b'\0')[0]
    return ''.join(chr(c) if 32 <= c < 127 else ' ' for c in raw).strip() or 'GAME BOY'


def own_size(image):
    """Bytes of the cart's own game (the header's ROM size), or 0."""
    code = image[0x148] if len(image) > 0x148 else 0xff
    return 0x8000 << code if header_ok(image) and code <= 8 else 0


# ----------------------------------------------------------------- parse

def _parse_v1(image, d):
    games, damaged = [], 0
    block = image[d:d + BLOCK]
    for i in range(SLOTS1):
        slot = bytearray(block[SLOT1_BASE + i * SLOT1_SIZE:SLOT1_BASE + (i + 1) * SLOT1_SIZE])
        if slot == b'\xff' * SLOT1_SIZE:
            continue
        crc = struct.unpack('<I', slot[20:24])[0]
        slot[20:24] = b'\0\0\0\0'
        if slot[:4] != ENTRY1_MAGIC or crc32(bytes(slot)) != crc:
            damaged += 1
            continue
        offset, size, data_crc = struct.unpack('<III', slot[8:20])
        name = bytes(slot[24:]).split(b'\0')[0].decode('utf-8', 'replace')
        data = bytes(image[offset:offset + size])
        if crc32(data) != data_crc:
            damaged += 1
            continue
        games.append(Game(name, data))
    return games, damaged


def _parse_v2(image, d):
    head = image[d:d + DIR2_HEADER]
    if struct.unpack('<I', head[28:32])[0] != crc32(bytes(head[:28])):
        return None
    count, entries_crc, _ = struct.unpack('<III', head[16:28])
    entries = image[d + DIR2_HEADER:d + DIR2_HEADER + count * ENTRY2_SIZE]
    if len(entries) != count * ENTRY2_SIZE or crc32(bytes(entries)) != entries_crc:
        return None
    games, damaged = [], 0
    for i in range(count):
        e = entries[i * ENTRY2_SIZE:(i + 1) * ENTRY2_SIZE]
        if e[:4] != ENTRY2_MAGIC:
            damaged += 1
            continue
        offset, size, data_crc, art_offset, art_size, art_crc = struct.unpack('<IIIIII', e[8:32])
        name = bytes(e[32:32 + NAME_SIZE]).split(b'\0')[0].decode('utf-8', 'replace')
        data = bytes(image[offset:offset + size])
        if crc32(data) != data_crc:
            damaged += 1
            continue
        art = bytes(image[art_offset:art_offset + art_size]) if art_size else None
        if art is not None and crc32(art) != art_crc:
            art = None
        games.append(Game(name, data, art))
    return games, damaged


def parse(image):
    """Read a whole-cart image (a backup)."""
    cart = Cart(capacity=len(image))
    size = own_size(image)
    candidates = [DEDICATED_DIR] + ([size] if size >= 0x8000 and size < len(image) else [])
    for d in candidates:
        magic = bytes(image[d:d + 16])
        parsed = None
        if magic == DIR2_MAGIC:
            parsed, cart.version = _parse_v2(image, d), 2
        elif magic == DIR1_MAGIC:
            parsed, cart.version = _parse_v1(image, d), 1
        if parsed is None:
            cart.version = 0
            continue
        cart.games, cart.damaged = parsed
        cart.dedicated = d == DEDICATED_DIR
        break
    if not cart.dedicated and size and header_ok(image) and not bytes(image[0x134:0x13f]).startswith(b'MAGIC TRICK'):
        cart.own_title, cart.own_size = header_title(image), size
    elif bytes(image[0x134:0x13f]).startswith(b'MAGIC TRICK'):
        cart.dedicated = True
    return cart


# ------------------------------------------------ the cart's own game

MAPPERS = {0x00: 'ROM only', 0x01: 'MBC1', 0x02: 'MBC1', 0x03: 'MBC1', 0x05: 'MBC2', 0x06: 'MBC2',
           0x0f: 'MBC3', 0x10: 'MBC3', 0x11: 'MBC3', 0x12: 'MBC3', 0x13: 'MBC3',
           0x19: 'MBC5', 0x1a: 'MBC5', 0x1b: 'MBC5', 0x1c: 'MBC5', 0x1d: 'MBC5', 0x1e: 'MBC5'}
RAM_SIZES = {0: 0, 1: 2048, 2: 8192, 3: 32768, 4: 131072, 5: 65536}


def own_game(rom, save_size=32768):
    """Check a Game Boy / Game Boy Color ROM for use as the cart's own game
    (what an original Game Boy plays). Returns (padded image, title, notes):
    notes are warnings about how well the cart's own controller (which acts
    like an MBC5, as on ModRetro carts) can run it."""
    if not header_ok(rom):
        raise CartError('That isn\'t a Game Boy or Game Boy Color game (its header doesn\'t check out).')
    size = 0x8000 << rom[0x148] if rom[0x148] <= 8 else 0
    if not size or len(rom) > size:
        raise CartError('That game\'s size doesn\'t match its header.')
    padded = bytes(rom) + b'\xff' * (size - len(rom))
    notes = []
    mapper = MAPPERS.get(rom[0x147])
    if mapper is None:
        notes.append('It uses a special cartridge chip (type 0x%02X) that this cart can\'t act like, so it '
                     'probably won\'t run on an original Game Boy.' % rom[0x147])
    elif mapper == 'MBC1':
        notes.append('It was made for an MBC1 cartridge. This cart acts like an MBC5, so on an original Game Boy '
                     'it may not run or may crash later on.')
    elif mapper == 'MBC2':
        notes.append('It was made for an MBC2 cartridge (with save memory built into the chip), which this cart '
                     'can\'t act like.')
    elif mapper == 'MBC3':
        notes.append('It was made for an MBC3 cartridge. It should mostly run, but %s' %
                     ('its clock won\'t work.' if rom[0x147] in (0x0f, 0x10) else 'test it on a Game Boy.'))
    ram = 512 if mapper == 'MBC2' else RAM_SIZES.get(rom[0x149], 0)
    if ram > save_size:
        notes.append('It saves %d KB, but this cart\'s save memory holds %d KB.' % (ram // 1024, save_size // 1024))
    return padded, header_title(rom), notes


# ---------------------------------------------------------------- layout

def _directory(games, placed):
    entries = bytearray()
    for g, (offset, art_offset) in zip(games, placed):
        e = bytearray(ENTRY2_SIZE)
        e[0:4] = ENTRY2_MAGIC
        e[4] = g.system
        e[5] = 1 if g.art else 0
        e[6:8] = b'\xff\xff'
        art = g.art or b''
        e[8:32] = struct.pack('<IIIIII', offset, len(g.data), crc32(g.data),
                              art_offset if art else 0, len(art), crc32(art) if art else 0)
        raw = check_name(g.name)
        e[32:32 + len(raw)] = raw
        e[104:128] = b'\xff' * 24
        entries += e
    size = (DIR2_HEADER + len(entries) + BLOCK - 1) // BLOCK * BLOCK
    head = bytearray(b'\xff' * DIR2_HEADER)
    head[0:16] = DIR2_MAGIC
    head[16:28] = struct.pack('<III', len(games), crc32(bytes(entries)), size)
    head[28:32] = struct.pack('<I', crc32(bytes(head[:28])))
    return bytes(head + entries)


def _align(n, a=BLOCK):
    return (n + a - 1) // a * a


def directory_bytes(count):
    return _align(DIR2_HEADER + count * ENTRY2_SIZE)


def usage(capacity, games, own=0, dedicated=False):
    """Where the space goes: [(label, kind, bytes)], kind in own/stub/directory/game/art/free."""
    parts = []
    base = own if not dedicated else 0x4000
    parts.append(('Cart program' if dedicated else 'Cart\'s own game', 'stub' if dedicated else 'own', base))
    parts.append(('Game list', 'directory', directory_bytes(len(games))))
    for g in games:
        parts.append((g.name, 'game', _align(len(g.data))))
    art = sum(_align(len(g.art)) for g in games if g.art)
    if art:
        parts.append(('Box art', 'art', art))
    used = sum(p[2] for p in parts)
    parts.append(('Free', 'free', max(0, capacity - used)))
    return parts, used


def build(capacity, games, own_image=None, dedicated=False):
    """A whole-cart image (0xFF where unused). own_image: the current cart
    contents, whose own game is kept when not dedicated."""
    seen = set()
    for g in games:
        check_name(g.name)
        if g.name.lower() in seen:
            raise CartError('Two games are named %s.' % g.name)
        seen.add(g.name.lower())
    if dedicated:
        image = bytearray(b'\xff' * capacity)
        image[0:0x4000] = stub_bank(games)
        d = DEDICATED_DIR
    else:
        size = own_size(own_image) if own_image else 0
        if not size:
            raise CartError('This cart has no Game Boy game of its own to keep.')
        image = bytearray(b'\xff' * capacity)
        image[0:size] = own_image[0:size]
        d = size
    offset = d + directory_bytes(len(games))
    placed = []
    for g in games:
        placed.append([offset, 0])
        offset = _align(offset + len(g.data))
    for p, g in zip(placed, games):
        if g.art:
            p[1] = offset
            offset = _align(offset + len(g.art))
    if offset > capacity:
        raise CartFull(offset, capacity)
    for (game_offset, art_offset), g in zip(placed, games):
        image[game_offset:game_offset + len(g.data)] = g.data
        if g.art:
            image[art_offset:art_offset + len(g.art)] = g.art
    directory = _directory(games, placed)
    image[d:d + len(directory)] = directory
    if dedicated:
        finish_header(image)
    return bytes(image)


def plan(current, target):
    """Per 64 KiB block: ('keep', o), ('program', o, [1 KiB offsets]) or
    ('erase', o, [1 KiB offsets to program after the erase])."""
    steps = []
    for o in range(0, len(target), ERASE_BLOCK):
        cur, want = current[o:o + ERASE_BLOCK], target[o:o + ERASE_BLOCK]
        if cur == want:
            steps.append(('keep', o))
            continue
        clears_only = all((w & ~c) & 0xff == 0 for c, w in zip(cur, want))
        if clears_only:
            blocks = [o + b for b in range(0, ERASE_BLOCK, BLOCK) if cur[b:b + BLOCK] != want[b:b + BLOCK]]
            steps.append(('program', o, blocks))
        else:
            blocks = [o + b for b in range(0, ERASE_BLOCK, BLOCK) if want[b:b + BLOCK] != b'\xff' * BLOCK]
            steps.append(('erase', o, blocks))
    return steps


def plan_bytes(steps):
    return sum(len(s[2]) * BLOCK for s in steps if s[0] != 'keep')


# ---------------------------------------------------- saves and states

def _cluster(n):
    return _align(n, CLUSTER) if n else 0


def save_bytes(game):
    """Worst case the game needs for its save and three save states."""
    if game.system == 1:
        return _cluster(GG_SAVE_MAX) + STATE_SLOTS * _cluster(STATE_HEADER + GG_STATE)
    if game.system == 2:
        return _cluster(512) + STATE_SLOTS * _cluster(STATE_HEADER + LYNX_STATE)
    ram = 0
    if len(game.data) > 0x149:
        ram = {0: 0, 1: 2048, 2: 8192, 3: 32768, 4: 131072, 5: 65536}.get(game.data[0x149], 0)
        if game.data[0x147] in (0x05, 0x06):   # MBC2: 512 nibbles
            ram = 512
    rtc = 48 if len(game.data) > 0x147 and game.data[0x147] in (0x0f, 0x10) else 0
    return _cluster(ram) + _cluster(rtc) + STATE_SLOTS * _cluster(STATE_HEADER + GB_STATE_CORE + ram)


def store_usage(games):
    """[(name, bytes)] for the internal store, plus the Game Gear working copy."""
    parts = [(g.name, save_bytes(g)) for g in games]
    if any(g.system == 1 for g in games):
        parts.append(('Game Gear working copy', _cluster(GG_MIRROR + 20)))
    return parts, sum(p[1] for p in parts)


# ------------------------------------------------- dedicated cart program
# A small Game Boy Color program: the Magic Trick logo, the game and
# LOADING..., shown after the intro while a Magic Trick Chromatic loads it.

W, H = 160, 144
NAVY, WHITE, BLUE, SKY = (6, 10, 25), (244, 247, 249), (47, 103, 192), (91, 196, 255)
CANDIDATES = [NAVY, WHITE, BLUE, SKY, (236, 50, 78), (255, 185, 92), (146, 175, 220), (26, 56, 108)]
HIDE_FRAMES = 180   # black for ~3 s at power-on, so the card only shows after the intro
DMG_BGP = 0x1b
LOGO = bytes.fromhex('ceed6666cc0d000b03730083000c000d0008111f8889000edccc6ee6dddd'
                     'd999bbbb67636e0eecccdddc999fbbb9333e')


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
    """The menu's 8x8 banner face (components/osd/system/banner_font.c)."""
    values = (ASSETS / 'cart_font.bin').read_bytes()
    return {chr(32 + i): list(values[i * 8:i * 8 + 8]) for i in range(95)}


def text_width(text, scale=1):
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
    draw_text(pixels, font, text, (W - text_width(text, scale)) // 2, y, scale, color)


def game_title(names):
    if len(names) != 1:
        return '%d GAMES' % len(names)
    # "Ninja Gaiden III - The Ancient Ship of Doom (USA, Europe)" -> "NINJA GAIDEN III".
    stem = Path(names[0]).stem
    return (stem.split(' (')[0].split(' - ')[0].strip() or stem).upper()


def title_card(names):
    font = banner_font()
    pixels = [[NAVY] * W for _ in range(H)]
    lw, lh, logo = read_ppm(ASSETS / 'cart_logo.ppm')
    top, left = 12, (W - lw) // 2
    for y in range(lh):
        pixels[top + y][left:left + lw] = logo[y]
    title = game_title(names)
    while text_width(title) > 152:
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
            raise CartError('title card needs more than 8 palettes')
    palettes = [sorted(p, key=lambda c: (c != NAVY, c)) for p in palettes]
    pal_bytes = bytearray()
    for p in palettes:
        p = p + [p[0]] * (4 - len(p))
        for r, g, b in p:
            pal_bytes += struct.pack('<H', (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10)
    pal_bytes += bytes(64 - len(pal_bytes))
    tiles, index, tmap, attrs = [], {}, bytearray(), bytearray()
    for block in blocks:
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
        raise CartError('title card needs %d tiles (max 512)' % len(tiles))
    return b''.join(tiles[:256]), b''.join(tiles[256:]), bytes(tmap), bytes(attrs), bytes(pal_bytes), palettes, blocks


def preview_pixels(blocks, palettes):
    """What the Game Boy Color shows (tile colors after the palette fit)."""
    px = [[NAVY] * W for _ in range(H)]
    for i, block in enumerate(blocks):
        ty, tx = divmod(i, 20)
        best = min(palettes, key=lambda p: sum(_dist(c, _nearest(c, p)) for c in block))
        for j, c in enumerate(block):
            px[ty * 8 + j // 8][tx * 8 + j % 8] = _nearest(c, best)
    return px


def write_ppm(pixels, path, zoom=4):
    out = bytearray(('P6\n%d %d\n255\n' % (W * zoom, H * zoom)).encode())
    for row in pixels:
        line = b''.join(bytes(c) * zoom for c in row)
        out += line * zoom
    Path(path).write_bytes(out)


def assemble(tiles0, tiles1, tmap, attrs, pal):
    """SM83 code at 0x150; data follows. Returns bank 0 (16 KiB). Loads the
    background with the LCD off and every palette black, keeps it black for
    HIDE_FRAMES (the Magic Trick intro plays over it), then sets the palettes
    and idles."""
    code = bytearray()
    fix = {}

    def emit(*b):
        code.extend(b)

    def ref(label):
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

    wait_vblank = (0xf0, 0x44, 0xfe, 0x90, 0x20, 0xfa)
    emit(0xf3)                                   # di
    emit(0x31, 0xfe, 0xff)                       # ld sp,$fffe
    emit(0xf0, 0x44, 0xfe, 0x90, 0x38, 0xfa)     # wait for vblank
    emit(0xaf, 0xe0, 0x40)                       # LCD off
    emit(0x3e, 0x80, 0xe0, 0x68)                 # BG palettes, auto-increment
    emit(0x06, 64, 0xaf)
    emit(0xe0, 0x69, 0x05, 0x20, 0xfb)           # all black
    emit(0x3e, 0x01, 0xe0, 0x4f)                 # VRAM bank 1
    copy(0x8000, 'tiles1', len(tiles1))
    copy_map('attrs')
    emit(0xaf, 0xe0, 0x4f)                       # VRAM bank 0
    copy(0x8000, 'tiles0', len(tiles0))
    copy_map('map')
    emit(0x3e, DMG_BGP, 0xe0, 0x47)
    emit(0xaf, 0xe0, 0x42, 0xe0, 0x43)
    emit(0x3e, 0x91, 0xe0, 0x40)                 # LCD on
    emit(0x0e, HIDE_FRAMES)
    emit(*wait_vblank)
    emit(0xf0, 0x44, 0xfe, 0x90, 0x28, 0xfa)
    emit(0x0d, 0x20, 0xf1)
    emit(*wait_vblank)
    emit(0x3e, 0x80, 0xe0, 0x68)
    emit(0x21); ref('pal')
    emit(0x06, 64)
    emit(0x2a, 0xe0, 0x69, 0x05, 0x20, 0xfa)
    emit(0x18, 0xfe)                             # idle
    labels = {'copy': 0x150 + len(code)}
    emit(0x78, 0xb1, 0xc8)
    emit(0x1a, 0x13, 0x22, 0x0b, 0x78, 0xb1, 0x20, 0xf8, 0xc9)
    labels['copymap'] = 0x150 + len(code)
    emit(0x0e, 18)
    emit(0x06, 20)
    emit(0x1a, 0x13, 0x22, 0x05, 0x20, 0xfa)
    emit(0xd5, 0x11, 12, 0x00, 0x19, 0xd1)
    emit(0x0d, 0x20, 0xef, 0xc9)

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


def stub_bank(games):
    tiles0, tiles1, tmap, attrs, pal, _, _ = cgb_screen(title_card([g.name for g in games]))
    return assemble(tiles0, tiles1, tmap, attrs, pal)


def finish_header(image, title=b'MAGIC TRICK'):
    image[0x134:0x143] = title.ljust(15, b'\0')[:15]
    image[0x143] = 0x80          # Game Boy Color compatible
    image[0x144:0x146] = b'00'
    image[0x146] = 0
    image[0x147] = 0x19          # MBC5
    code = (len(image) // 0x8000).bit_length() - 1
    assert 0x8000 << code == len(image)
    image[0x148] = code
    image[0x149] = 0
    image[0x14a] = 1
    image[0x14b] = 0x33
    image[0x14c] = 0
    image[0x14d] = (-sum(image[0x134:0x14d]) - (0x14d - 0x134)) & 0xff
    total = (sum(image) - image[0x14e] - image[0x14f]) & 0xffff
    image[0x14e:0x150] = struct.pack('>H', total)


# --------------------------------------------------------- USB and Wi-Fi

class CartAccess:
    """The cartridge in a Chromatic's Game Boy core: read, erase 64 KiB blocks,
    program, CRC. Subclasses move the bytes (USB console or Wi-Fi Files)."""
    capacity = 0
    save_size = 0                # cartridge save memory (battery RAM / FRAM)
    run_limit = BLOCK            # bytes per program request

    def matches(self, image, offsets):
        """Whether the cart holds image's bytes in these 64 KiB blocks."""
        return all(self.crc(o, ERASE_BLOCK) == crc32(image[o:o + ERASE_BLOCK]) for o in offsets)

    def program_blocks(self, target, offsets, progress=None, done=0, total=None):
        """Program these 1 KiB blocks of target, contiguous ones together."""
        offsets = sorted(offsets)
        total = total or len(offsets) * BLOCK
        i = 0
        while i < len(offsets):
            j = i + 1
            while j < len(offsets) and offsets[j] == offsets[j - 1] + BLOCK and (j - i + 1) * BLOCK <= self.run_limit:
                j += 1
            start, end = offsets[i], offsets[j - 1] + BLOCK
            self.program(start, target[start:end])
            done += end - start
            if progress:
                progress(done, total)
            i = j
        return done

    def write(self, current, target, progress=None, log=print):
        """Change the cart from current to target (both whole-cart images).
        Only the blocks that change (and block 0, the cart's identity) are
        checked before and after: every programmed 1 KiB is read back by the
        firmware as it goes. Returns the plan's steps."""
        steps = plan(current, target)
        touched = [s[1] for s in steps if s[0] != 'keep']
        if not self.matches(current, sorted(set([0] + touched))):
            raise CartError('The cart isn\'t the one that was read (or it changed since). Nothing was written.')
        total, done = max(1, plan_bytes(steps)), 0
        for step in steps:
            if step[0] == 'keep':
                continue
            if step[0] == 'erase':
                self.erase(step[1])
            done = self.program_blocks(target, step[2], progress, done, total)
        if not self.matches(target, touched):
            raise CartError('The cart doesn\'t match what was written. Nothing was lost: write it again '
                            'or restore the backup.')
        return steps


class CartUsb(CartAccess):
    """Over USB (USB TRANSFERS on), through library.UsbTarget's session."""

    def __init__(self, usb):
        self.usb = usb
        usb._timeout(30)
        usb._send(b'\x20')
        rec = usb._record()
        if not rec.startswith('CART capacity='):
            raise CartError('The Chromatic did not open the cart (%r).' % rec)
        fields = dict(f.split('=', 1) for f in rec.split()[1:])
        self.capacity, self.save_size = int(fields['capacity']), int(fields.get('save', 0))
        usb._ok()

    def read(self, progress=None):
        import base64
        out = bytearray(self.capacity)
        step = 64 * BLOCK
        for o in range(0, self.capacity, step):
            n = min(step, self.capacity - o) // BLOCK
            self.usb._timeout(60)
            self.usb._send(b'\x21' + struct.pack('<II', o, n))
            got = 0
            while got < n:
                rec = self.usb._record()
                if not rec.startswith('CDATA '):
                    raise CartError('unexpected cart reply %r' % rec[:40])
                at, data = rec[6:].split(' ', 1)
                block = base64.b64decode(data)
                at = int(at, 16)
                if len(block) != BLOCK or not o <= at < o + n * BLOCK:
                    raise CartError('bad cart block at 0x%x' % at)
                out[at:at + BLOCK] = block
                got += 1
            self.usb._ok()
            if progress:
                progress(o + n * BLOCK, self.capacity)
        return bytes(out)

    def crc(self, offset, length):
        self.usb._timeout(120)
        self.usb._send(b'\x24' + struct.pack('<II', offset, length))
        rec = self.usb._record()
        if not rec.startswith('CCRC crc='):
            raise CartError('unexpected cart reply %r' % rec)
        self.usb._ok()
        return int(rec[9:], 16)

    def erase(self, offset):
        self.usb._exchange(b'\x22' + struct.pack('<I', offset), seconds=180)

    def program(self, offset, data):
        for at in range(0, len(data), BLOCK):
            self.usb._exchange(b'\x23' + struct.pack('<I', offset + at) + data[at:at + BLOCK], seconds=30)

    def read_save(self):
        import base64
        out = bytearray(self.save_size)
        for o in range(0, self.save_size, 32 * BLOCK):
            n = min(32 * BLOCK, self.save_size - o) // BLOCK
            self.usb._timeout(60)
            self.usb._send(b'\x26' + struct.pack('<II', o, n))
            for _ in range(n):
                rec = self.usb._record()
                if not rec.startswith('CDATA '):
                    raise CartError('unexpected cart reply %r' % rec[:40])
                at, data = rec[6:].split(' ', 1)
                out[int(at, 16):int(at, 16) + BLOCK] = base64.b64decode(data)
            self.usb._ok()
        return bytes(out)

    def write_save(self, data):
        for at in range(0, len(data), BLOCK):
            self.usb._exchange(b'\x27' + struct.pack('<I', at) + data[at:at + BLOCK], seconds=30)

    def close(self):
        self.usb._timeout(60)
        self.usb._send(b'\x25')
        rec = self.usb._record()
        self.usb._ok()
        return int(rec.split('=', 1)[1]) if rec.startswith('CDIR games=') else None


class CartWifi(CartAccess):
    """Over Wi-Fi Files (MCU with /api/cart), through library.WifiTarget's session."""
    run_limit = 32 * BLOCK

    def __init__(self, wifi):
        import json
        self.wifi, self.json = wifi, json
        try:
            reply = json.loads(self._call('POST', 'cart/open', timeout=60))
        except CartError as exc:
            if '(404)' in str(exc) or '(405)' in str(exc):
                raise CartError('This Chromatic\'s firmware can\'t reach the cart over Wi-Fi yet. Update Magic '
                                'Trick, or use USB.') from None
            raise
        self.capacity, self.save_size = int(reply['capacity']), int(reply.get('save', 0))

    def _call(self, method, route, body=None, timeout=120):
        try:
            return self.wifi._request(method, route, body=body, timeout=timeout,
                                      headers={'Content-Type': 'application/octet-stream'} if body else None)
        except Exception as exc:   # library.WifiError / ConnectionError: keep the device's words
            if getattr(exc, 'status', None) == 409:
                raise CartError('Start a Game Boy game on the Chromatic first: only it can reach the cart.') from None
            raise CartError(str(exc)) from None

    def read(self, progress=None):
        out = bytearray(self.capacity)
        step = 64 * BLOCK
        for o in range(0, self.capacity, step):
            n = min(step, self.capacity - o)
            data = self._call('GET', 'cart/read?offset=%d&length=%d' % (o, n))
            if len(data) != n:
                raise CartError('The Chromatic sent %d of %d bytes at 0x%x.' % (len(data), n, o))
            out[o:o + n] = data
            if progress:
                progress(o + n, self.capacity)
        return bytes(out)

    def crc(self, offset, length):
        return int(self.json.loads(self._call('GET', 'cart/crc?offset=%d&length=%d' % (offset, length)))['crc32'], 16)

    def erase(self, offset):
        self._call('POST', 'cart/erase?offset=%d' % offset, timeout=180)

    def program(self, offset, data):
        self._call('PUT', 'cart/write?offset=%d' % offset, body=bytes(data))

    def read_save(self):
        data = self._call('GET', 'cart/save?offset=0&length=%d' % self.save_size) if self.save_size else b''
        if len(data) != self.save_size:
            raise CartError('The Chromatic sent %d of %d save bytes.' % (len(data), self.save_size))
        return data

    def write_save(self, data):
        self._call('PUT', 'cart/save?offset=0', body=bytes(data))

    def close(self):
        return int(self.json.loads(self._call('POST', 'cart/close', timeout=120)).get('games', 0))
