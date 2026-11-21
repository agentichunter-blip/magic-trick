import struct, unittest
import cartridge as c

CAP = 2 << 20


def gb_rom(size=0x40000, title=b'MR.TETRIS', color=0xc0):
    rom = bytearray(b'\x5a' * size)
    rom[0x134:0x134 + len(title)] = title
    rom[0x134 + len(title):0x143] = b'\0' * (0x143 - 0x134 - len(title))
    rom[0x143] = color
    rom[0x147] = 0x1b
    rom[0x148] = (size // 0x8000).bit_length() - 1
    rom[0x149] = 3
    rom[0x14d] = (-sum(rom[0x134:0x14d]) - (0x14d - 0x134)) & 0xff
    return bytes(rom)


def game(name, size, fill):
    return c.Game(name, bytes([fill]) * size)


def cart_with(rom, cap=CAP):
    return rom + b'\xff' * (cap - len(rom))


def v1_image(rom, games, cap=CAP):
    """A version 1 cart as the console's cartadd leaves it."""
    image = bytearray(cart_with(rom, cap))
    d = len(rom)
    block = bytearray(b'\xff' * c.BLOCK)
    block[:16] = c.DIR1_MAGIC
    offset = d + c.BLOCK
    for i, g in enumerate(games):
        slot = bytearray(c.SLOT1_SIZE)
        slot[0:4] = c.ENTRY1_MAGIC
        slot[4] = g.system
        slot[5:8] = b'\xff\xff\xff'
        slot[8:20] = struct.pack('<III', offset, len(g.data), c.crc32(g.data))
        raw = g.name.encode()
        slot[24:24 + len(raw)] = raw
        slot[20:24] = struct.pack('<I', c.crc32(bytes(slot)))
        block[c.SLOT1_BASE + i * c.SLOT1_SIZE:c.SLOT1_BASE + (i + 1) * c.SLOT1_SIZE] = slot
        image[offset:offset + len(g.data)] = g.data
        offset = (offset + len(g.data) + c.BLOCK - 1) // c.BLOCK * c.BLOCK
    image[d:d + c.BLOCK] = block
    return bytes(image)


class CartridgeTest(unittest.TestCase):
    def test_reads_version_1_and_own_game(self):
        rom = gb_rom()
        games = [game('Shinobi.gg', 0x40000, 1), game('Golf.lnx', 0x20000, 2)]
        cart = c.parse(v1_image(rom, games))
        self.assertEqual((cart.version, cart.own_title, cart.own_size, cart.dedicated), (1, 'MR.TETRIS', 0x40000, False))
        self.assertEqual([g.name for g in cart.games], ['Shinobi.gg', 'Golf.lnx'])
        self.assertEqual(cart.games[0].data, games[0].data)

    def test_conversion_keeps_own_game_and_rewrites_one_block(self):
        rom = gb_rom()
        games = [game('Shinobi.gg', 0x40000, 1), game('Golf.lnx', 0x20000, 2)]
        current = v1_image(rom, games)
        target = c.build(CAP, c.parse(current).games, own_image=current)
        self.assertEqual(target[:len(rom)], rom)
        steps = c.plan(current, target)
        self.assertTrue(all(s[0] == 'keep' for s in steps if s[1] < len(rom)))
        self.assertEqual([s[1] for s in steps if s[0] != 'keep'], [0x40000])
        back = c.parse(target)
        self.assertEqual((back.version, back.own_title), (2, 'MR.TETRIS'))
        self.assertEqual([g.data for g in back.games], [g.data for g in games])

    def test_more_than_ten_games_and_box_art(self):
        art = b'CMAR' + bytes(c.ART_SIZE - 4)
        games = [c.Game('Game %02d.gg' % i, bytes([i]) * 0x8000, art if i % 2 else None) for i in range(30)]
        image = c.build(CAP, games, dedicated=True)
        cart = c.parse(image)
        self.assertTrue(cart.dedicated)
        self.assertEqual(len(cart.games), 30)
        self.assertEqual(cart.games[3].art, art)
        self.assertIsNone(cart.games[4].art)
        self.assertTrue(c.header_ok(image))
        self.assertEqual(image[0x134:0x13f], b'MAGIC TRICK')

    def test_full_cart_and_names(self):
        with self.assertRaises(c.CartFull):
            c.build(CAP, [game('Big.gg', 0x200000, 1)], dedicated=True)
        with self.assertRaises(c.CartError):
            c.build(CAP, [game('A.gg', 64, 1), game('a.GG', 64, 2)], dedicated=True)
        with self.assertRaises(c.CartError):
            c.build(CAP, [game('notes.txt', 64, 1)], dedicated=True)
        long = 'X' * 90 + ' (USA).gg'
        self.assertTrue(c.short_name(long).endswith('.gg'))
        self.assertLess(len(c.short_name(long).encode()), c.NAME_SIZE)

    def test_plan_programs_without_erase_when_only_adding(self):
        rom = gb_rom()
        first = c.build(CAP, [game('A.gg', 0x8000, 1)], own_image=cart_with(rom))
        # Same layout with free space programmed: only clears bits, so no erase.
        target = bytearray(first)
        target[0x1f0000:0x1f0400] = b'\x00' * 0x400
        steps = c.plan(first, bytes(target))
        self.assertEqual([(s[0], s[1]) for s in steps if s[0] != 'keep'], [('program', 0x1f0000)])
        self.assertEqual(c.plan_bytes(steps), 0x400)

    def test_usage_and_store_budget(self):
        games = [game('A.gg', 0x40000, 1), game('B.lnx', 0x20000, 2), c.Game('C.gb', gb_rom(0x8000, b'C'))]
        parts, used = c.usage(CAP, games, own=0x40000)
        self.assertEqual(used + parts[-1][2], CAP)
        _, store = c.store_usage(games)
        gg = 32768 + 3 * c._cluster(64 + c.GG_STATE)
        self.assertEqual(c.save_bytes(games[0]), gg)
        self.assertGreater(store, gg + c.save_bytes(games[1]))


class OwnGameTest(unittest.TestCase):
    def test_replace_own_game(self):
        tetris, other = gb_rom(), gb_rom(0x20000, b'POCKET', 0x80)
        current = c.build(CAP, [game('A.gg', 0x8000, 1)], own_image=cart_with(tetris))
        image, title, notes = c.own_game(other)
        self.assertEqual((title, len(image), notes), ('POCKET', 0x20000, []))   # MBC5 (0x1b), 32 KiB save: fine
        target = c.build(CAP, [game('A.gg', 0x8000, 1)], own_image=image)
        cart = c.parse(target)
        self.assertEqual((cart.own_title, cart.own_size, [g.name for g in cart.games]), ('POCKET', 0x20000, ['A.gg']))
        self.assertEqual(target[:0x20000], other)
        self.assertNotEqual(c.plan(current, target)[0][0], 'keep')   # block 0 now changes

    def test_own_game_warnings(self):
        rom = bytearray(gb_rom(0x8000, b'OLD'))
        rom[0x147], rom[0x149] = 0x03, 4          # MBC1, 128 KiB save
        rom[0x14d] = (-sum(rom[0x134:0x14d]) - (0x14d - 0x134)) & 0xff
        _, _, notes = c.own_game(bytes(rom))
        self.assertEqual(len(notes), 2)
        self.assertIn('MBC1', notes[0]); self.assertIn('128 KB', notes[1])
        with self.assertRaises(c.CartError):
            c.own_game(b'\0' * 0x8000)           # no valid header
        short = gb_rom(0x40000)[:0x30000]         # shorter than its header says: padded
        self.assertEqual(len(c.own_game(short)[0]), 0x40000)


class FakeWifi:
    """Wi-Fi Files' /api/cart routes over a bytearray cart."""
    def __init__(self, image):
        self.cart, self.calls = bytearray(image), []
        self.save = bytearray(b'\x5a' * 32768)

    def _request(self, method, route, body=None, headers=None, timeout=None):
        import json, urllib.parse
        path, _, query = route.partition('?')
        q = {k: int(v[0]) for k, v in urllib.parse.parse_qs(query).items()}
        self.calls.append((method, path, q.get('offset'), len(body or b'')))
        if path == 'cart/open':
            return json.dumps({'capacity': len(self.cart), 'save': len(self.save)}).encode()
        if path == 'cart/save' and method == 'GET':
            return bytes(self.save[q['offset']:q['offset'] + q['length']])
        if path == 'cart/save' and method == 'PUT':
            self.save[q['offset']:q['offset'] + len(body)] = body
            return b'{"ok":true}'
        if path == 'cart/read':
            return bytes(self.cart[q['offset']:q['offset'] + q['length']])
        if path == 'cart/crc':
            return json.dumps({'crc32': '%08x' % c.crc32(bytes(self.cart[q['offset']:q['offset'] + q['length']]))}).encode()
        if path == 'cart/erase':
            self.cart[q['offset']:q['offset'] + c.ERASE_BLOCK] = b'\xff' * c.ERASE_BLOCK
            return b'{"ok":true}'
        if path == 'cart/write':
            o = q['offset']
            assert all(old & new == new for old, new in zip(self.cart[o:o + len(body)], body)), 'programs only clear bits'
            self.cart[o:o + len(body)] = body
            return b'{"ok":true}'
        if path == 'cart/close':
            return json.dumps({'games': len(c.parse(bytes(self.cart)).games)}).encode()
        raise AssertionError(route)


class WifiCartTest(unittest.TestCase):
    def test_read_and_write_over_wifi(self):
        rom = gb_rom()
        current = c.build(CAP, [game('A.gg', 0x8000, 1)], own_image=cart_with(rom))
        fake = FakeWifi(current)
        cart = c.CartWifi(fake)
        self.assertEqual((cart.capacity, cart.read()), (CAP, current))
        target = c.build(CAP, [game('A.gg', 0x8000, 1), game('B.lnx', 0x30000, 2)], own_image=current)
        cart.write(current, target)
        self.assertEqual(bytes(fake.cart), target)
        writes = [x for x in fake.calls if x[1] == 'cart/write']
        self.assertTrue(writes and all(n <= cart.run_limit and n % c.BLOCK == 0 for *_, n in writes))
        self.assertLess(len(writes), c.plan_bytes(c.plan(current, target)) // c.BLOCK)   # batched
        self.assertEqual((cart.save_size, cart.read_save()), (32768, b'\x5a' * 32768))
        cart.write_save(b'\xff' * 32768)
        self.assertEqual(bytes(fake.save), b'\xff' * 32768)
        self.assertEqual(cart.close(), 2)

    def test_changed_cart_is_refused(self):
        rom = gb_rom()
        current = c.build(CAP, [game('A.gg', 0x8000, 1)], own_image=cart_with(rom))
        fake = FakeWifi(current)
        fake.cart[0x150] ^= 0xff
        with self.assertRaises(c.CartError):
            c.CartWifi(fake).write(current, c.build(CAP, [], own_image=current))
        self.assertFalse([x for x in fake.calls if x[1] in ('cart/write', 'cart/erase')])


if __name__ == '__main__':
    unittest.main()
