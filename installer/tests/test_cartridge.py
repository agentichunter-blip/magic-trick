import struct, time, unittest
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
    def test_nes_and_sms_games(self):
        nrom = b'NES\x1a\x02\x01\x01\x00' + bytes(8) + bytes(32768 + 8192)
        self.assertIsNone(c.nes_problem(nrom))
        self.assertEqual(c.Game('Nes15.nes', nrom).system, 3)
        self.assertEqual(c.Game('Chase.sms', bytes(0x20000)).system, 1)
        self.assertEqual(c.Game('Chase.SMS', bytes(0x20000)).system_name, 'Master System')
        self.assertEqual(c.Game('Sonic.gg', bytes(0x20000)).system_name, 'Game Gear')
        self.assertEqual(c.Game('Nes15.nes', nrom).system_name, 'NES')
        self.assertIn('mapper 5', c.nes_problem(b'NES\x1a\x10\x10\x50\x00' + bytes(8) + bytes(0x60000)))
        self.assertIn('shorter', c.nes_problem(nrom[:-1]))
        self.assertIn('no iNES', c.nes_problem(bytes(64)))
        # 8 KiB battery save + three 21 KiB states, in 4 KiB clusters
        self.assertEqual(c.save_bytes(c.Game('Nes15.nes', nrom)), 8192 + 3 * 24576)

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


class OwnArtTest(unittest.TestCase):
    def test_own_game_art_round_trip(self):
        art = b'CMAR' + bytes(range(256)) * 40
        games = [game('A.gg', 0x8000, 1), c.Game('B.lnx', b'\2' * 0x8000, b'CMAR' + bytes(100))]
        image = c.build(CAP, games, own_image=cart_with(gb_rom()), own_art=art)
        cart = c.parse(image)
        self.assertEqual((cart.own_title, cart.own_art), ('MR.TETRIS', art))
        self.assertEqual([g.data for g in cart.games], [g.data for g in games])
        self.assertEqual(c.parse(c.build(CAP, games, own_image=image)).own_art, None)   # dropped when not passed
        parts, used = c.usage(CAP, games, own=0x40000, own_art=art)
        self.assertEqual(used, len(image.rstrip(b'\xff')) + (-len(image.rstrip(b'\xff'))) % c.BLOCK)

    def test_own_art_needs_its_check(self):
        art = b'CMAR' + bytes(64)
        image = bytearray(c.build(CAP, [game('A.gg', 0x8000, 1)], own_image=cart_with(gb_rom()), own_art=art))
        d = 0x40000
        image[d + c.OWN_ART_AT + 4] ^= 1        # size changed, check no longer matches
        self.assertIsNone(c.parse(bytes(image)).own_art)
        self.assertEqual(len(c.parse(bytes(image)).games), 1)   # the games are unaffected
        ded = c.build(CAP, [game('A.gg', 0x8000, 1)], dedicated=True, own_art=art)
        self.assertIsNone(c.parse(ded).own_art)  # a Magic Trick cart has no own game


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


SCALE = 0.1   # test time per second of real time


class LossyChromatic:
    """The Chromatic's USB console for cart reads, over a link that drops a byte now and then
    (seen on a Windows PC: about 1 byte in 4 KiB)."""

    def __init__(self, image, save, drop, seed, lose_ok=()):
        import random
        self.image, self.save, self.drop, self.rng = image, save, drop, random.Random(seed)
        self.lose_ok = set(lose_ok)   # requests whose OK line goes missing
        self.inbuf, self.out, self.seq, self.waiting = b'', bytearray(), 0, None

    def emit(self, text):
        self.out += bytes(b for b in text.encode() if self.rng.random() >= self.drop)

    def write(self, data):
        import base64
        self.inbuf += data
        while len(self.inbuf) >= 16:
            size = int.from_bytes(self.inbuf[8:12], 'little')
            if len(self.inbuf) < 16 + size:
                return
            seq, payload = int.from_bytes(self.inbuf[4:8], 'little'), self.inbuf[16:16 + size]
            self.inbuf = self.inbuf[16 + size:]
            if self.waiting and time.time() - self.waiting > 5 * SCALE:
                self.emit('PCSD FAIL error=ESP_ERR_TIMEOUT\n')   # it waits 5 s for the next request
                return
            if seq != self.seq:
                self.emit('PCSD FAIL error=ESP_ERR_INVALID_ARG\n')
                return
            if payload[0] == 0x20:
                self.emit('PCSD CART capacity=%d save=%d\n' % (len(self.image), len(self.save)))
            elif payload[0] in (0x21, 0x26):
                offset, n = struct.unpack('<II', payload[1:9])
                src = self.image if payload[0] == 0x21 else self.save
                for a in range(offset, offset + n * c.BLOCK, c.BLOCK):
                    self.emit('PCSD CDATA %x %s\n' % (a, base64.b64encode(src[a:a + c.BLOCK]).decode()))
            elif payload[0] == 0x25:
                self.emit('PCSD CDIR games=2\n')
            if self.seq not in self.lose_ok:
                self.emit('PCSD OK seq=%d\n' % self.seq)
            self.seq += 1
            self.waiting = time.time()

    def flush(self):
        pass

    def read(self, n):
        data = bytes(self.out[:min(n, 4096)])
        del self.out[:len(data)]
        return data


class CartUsbReadTest(unittest.TestCase):
    def read(self, drop, seed, lose_ok=()):
        import random, time, library
        rng = random.Random(seed)
        image, save = rng.randbytes(256 << 10), rng.randbytes(8 << 10)
        usb = object.__new__(library.UsbTarget)
        library.Target.__init__(usb)
        usb.port, usb.seq, usb._buf, usb.deadline = LossyChromatic(image, save, drop, seed, lose_ok), 0, b'', 0
        usb._timeout = lambda seconds: setattr(usb, 'deadline', time.time() + seconds * SCALE)
        cart = c.CartUsb(usb)
        got = cart.read(), cart.read_save(), cart.close()
        return got, (image, save, 2)

    def test_clean_link(self):
        got, want = self.read(0, 1)
        self.assertEqual(got, want)

    def test_lost_ok_moves_on_in_time(self):
        # The Chromatic waits only 5 s for the next request after its OK.
        got, want = self.read(0, 1, lose_ok=(0, 2, 5))
        self.assertEqual(got, want)

    def test_damaged_lines_are_read_again(self):
        for seed in (2, 3, 4):
            got, want = self.read(1 / 4000, seed)
            self.assertEqual(got, want)


if __name__ == '__main__':
    unittest.main()
