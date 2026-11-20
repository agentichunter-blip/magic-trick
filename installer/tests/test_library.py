import io, json, shutil, sys, tempfile, threading, unittest, unittest.mock, zipfile, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote
import library

TOOLS = Path(__file__).resolve().parents[2] / 'mcu' / 'tools'


def jpeg(colour=(30, 90, 200), size=(300, 400)):
    from PIL import Image
    out = io.BytesIO(); Image.new('RGB', size, colour).save(out, 'JPEG'); return out.getvalue()


def rom(size=0x8000, cgb=0x00, seed=1):
    data = bytearray((seed * 7 + i) & 0xFF for i in range(size))
    data[0x143] = cgb
    return bytes(data)


INDEX = '\n'.join([
    '#name\tcrc\tsize\tkey',
    'Tetris (World) (Rev 1)\t%08x\t32768\tTetris (World) (Rev 1)' % zlib.crc32(rom(seed=1)),
    'Tetris (Japan)\t11111111\t32768\tTetris (World) (Rev 1)',
    'Golf (World)\t22222222\t65536\tGolf (World)',
    'Pinball (USA)\t33333333\t65536\tPinball (USA)',
    'Pinball (Japan)\t44444444\t65536\tPinball (Japan)',
])


class Matching(unittest.TestCase):
    def setUp(self):
        self.index = library.PackIndex('nintendo-handhelds', 'GAMEBOY', INDEX)

    def test_exact_key_and_catalogued_name(self):
        self.assertEqual(library.resolve('Golf (World)', [self.index])[1:], ('Golf (World)', 'name'))
        self.assertEqual(library.resolve('tetris (japan)', [self.index])[1:], ('Tetris (World) (Rev 1)', 'name'))

    def test_checksum_catches_a_renamed_file(self):
        crcs = library.rom_crcs('x.gb', rom(seed=1))
        self.assertEqual(library.resolve('my tetris', [self.index], crcs)[1:], ('Tetris (World) (Rev 1)', 'checksum'))

    def test_bare_title_only_when_unique(self):
        self.assertEqual(library.resolve('Golf [!]', [self.index])[1], 'Golf (World)')
        self.assertIsNone(library.resolve('Pinball (Europe)', [self.index]))   # two different boxes: no guess

    def test_sibling_catalogue(self):
        gbc = library.PackIndex('nintendo-handhelds', 'GBC', '#name\tcrc\tsize\tkey\nPokemon Pinball (USA)\t\t\tPokemon Pinball (USA)')
        found = library.resolve('Pokemon Pinball (USA)', [self.index, gbc])
        self.assertEqual((found[0].system, found[1]), ('GBC', 'Pokemon Pinball (USA)'))

    def test_url_encodes_key(self):
        self.assertTrue(self.index.url('Tetris (World) (Rev 1)').endswith(
            '/artworkdb-nintendo-handhelds/media-box2d/docs/GAMEBOY/Artwork/Tetris%20%28World%29%20%28Rev%201%29.jpg'))

    def test_lynx_header_is_ignored_for_checksums(self):
        body = bytes(range(256)) * 512
        crcs = library.rom_crcs('a.lnx', b'LYNX' + bytes(60) + body)
        self.assertIn((zlib.crc32(body), len(body)), crcs)


class Card(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.card = self.tmp / 'card'; self.card.mkdir()

    def put(self, rel, data):
        path = self.card / rel; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)

    def test_add_games_sorts_files_zips_and_colour_only_games(self):
        src = self.tmp / 'in'; src.mkdir()
        (src / 'Tetris (World).gb').write_bytes(rom(seed=1))
        (src / 'Colour Only.gb').write_bytes(rom(cgb=0xC0, seed=2))
        (src / 'readme.txt').write_text('x')
        with zipfile.ZipFile(src / 'pack.zip', 'w') as z:
            z.writestr('roms/Sonic.gg', b'GG' * 100)
            z.writestr('roms/notes.nfo', b'x')
        self.put('CHROMAGIC/BACKUPS/GB/Puzzles/Golf (World).gb', rom(seed=3))
        (src / 'Golf (World).gb').write_bytes(rom(seed=3))
        result = library.add_games(library.FolderTarget(self.card), [src], log=lambda *a: None)
        games = self.card / 'CHROMAGIC/BACKUPS'
        self.assertTrue((games / 'GB/Tetris (World).gb').is_file())
        self.assertTrue((games / 'GBC/Colour Only.gb').is_file())
        self.assertEqual((games / 'GG/Sonic.gg').read_bytes(), b'GG' * 100)
        self.assertFalse((games / 'GB/Golf (World).gb').exists())   # already on the card, in a subfolder
        self.assertIn('Added 3 games', result)
        self.assertIn('1 was already on the card, so it was skipped', result)

    def test_remove_games_with_art_and_clips_keeps_saves(self):
        for rel in ('CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).gb', 'CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).sav',
                    'CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).state', 'CHROMAGIC/ART/GB/S-Z/Tetris (World).gb.cma',
                    'CHROMAGIC/VIDEO/GB/S-Z/Tetris (World).gb.cmv', 'CHROMAGIC/BACKUPS/GG/Sonic.gg'):
            self.put(rel, b'x')
        target = library.FolderTarget(self.card)
        self.assertEqual(sorted(p for p, _, _ in library.card_games(target)),
                         ['CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).gb', 'CHROMAGIC/BACKUPS/GG/Sonic.gg'])
        # A different spelling still finds the file (FAT ignores case); a vanished game is reported.
        result = library.remove_games(target, ['CHROMAGIC/BACKUPS/GB/S-Z/tetris (world).gb',
                                               'CHROMAGIC/BACKUPS/GG/Gone.gg'], log=lambda *a: None)
        self.assertFalse((self.card / 'CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).gb').exists())
        self.assertFalse((self.card / 'CHROMAGIC/ART/GB/S-Z/Tetris (World).gb.cma').exists())
        self.assertFalse((self.card / 'CHROMAGIC/VIDEO/GB/S-Z/Tetris (World).gb.cmv').exists())
        self.assertTrue((self.card / 'CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).sav').exists())
        self.assertTrue((self.card / 'CHROMAGIC/BACKUPS/GB/S-Z/Tetris (World).state').exists())
        self.assertTrue((self.card / 'CHROMAGIC/BACKUPS/GG/Sonic.gg').exists())
        self.assertEqual(result, 'Removed 1 game and 2 box art and clips. 1 was already gone. '
                                 'Their saves and save states are still on the card.')

    def test_remove_refuses_files_that_are_not_games(self):
        self.put('CHROMAGIC/BACKUPS/GB/Tetris (World).sav', b'x')
        for bad in ('CHROMAGIC/BACKUPS/GB/Tetris (World).sav', 'CHROMAGIC/ART/GB/x.gb', '../x.gb'):
            with self.assertRaises(ValueError):
                library.remove_games(library.FolderTarget(self.card), [bad], log=lambda *a: None)
        self.assertTrue((self.card / 'CHROMAGIC/BACKUPS/GB/Tetris (World).sav').exists())

    def test_games_go_in_existing_letter_folders(self):
        for sub in ('0-F', 'G-L', 'M-R', 'S-Z'):
            (self.card / 'CHROMAGIC/BACKUPS/GB' / sub).mkdir(parents=True)
        src = self.tmp / 'in'; src.mkdir()
        names = {'Tetris (World).gb': 'S-Z', 'Alleyway (World).gb': '0-F', '3D Pocket Pool (Europe).gb': '0-F',
                 "'Rare' Demo (PD).gb": '0-F', 'Mole Mania (USA).gb': 'M-R', 'Kirby (USA).gb': 'G-L',
                 'Zelda (USA).gb': 'S-Z'}
        for i, name in enumerate(names):
            (src / name).write_bytes(rom(seed=10 + i))
        (src / 'Wario (USA).gg').write_bytes(b'GG' * 64)   # GG has no letter folders here
        library.add_games(library.FolderTarget(self.card), [src], log=lambda *a: None)
        for name, sub in names.items():
            self.assertTrue((self.card / 'CHROMAGIC/BACKUPS/GB' / sub / name).is_file(), name)
        self.assertTrue((self.card / 'CHROMAGIC/BACKUPS/GG/Wario (USA).gg').is_file())

    def test_letter_folder_edge_cases(self):
        target = library.FolderTarget(self.card)
        (self.card / 'CHROMAGIC/BACKUPS/GBC/g-l').mkdir(parents=True)
        (self.card / 'CHROMAGIC/BACKUPS/GBC/S-Z').mkdir()
        (self.card / 'CHROMAGIC/BACKUPS/GBC/Favourites').mkdir()
        pick = lambda name: library.letter_folder(target, 'CHROMAGIC/BACKUPS/GBC', name)
        self.assertEqual(pick('kirby.gbc'), 'g-l')
        self.assertEqual(pick('Alleyway.gbc'), 'g-l')     # before the first range: the first range
        self.assertEqual(pick('Pokemon.gbc'), 'S-Z')      # between ranges with no folder: the later one
        self.assertIsNone(library.letter_folder(target, 'CHROMAGIC/BACKUPS/GB', 'x.gb'))

    def test_existing_folder_spelling_is_reused(self):
        self.put('chromagic/backups/gb/a.gb', rom())
        target = library.FolderTarget(self.card)
        target.write('CHROMAGIC/ART/GB/a.gb.cma', b'x')
        self.assertTrue((self.card / 'chromagic/ART/GB/a.gb.cma').is_file())

    def test_covers_written_next_to_games(self):
        self.put('CHROMAGIC/BACKUPS/GB/Favourites/Golf (World).gb', rom(seed=3))
        self.put('CHROMAGIC/BACKUPS/GB/renamed.gb', rom(seed=1))
        self.put('CHROMAGIC/BACKUPS/GB/Unknown Homebrew.gb', rom(seed=9))
        self.put('CHROMAGIC/BACKUPS/GB/Has Cover.gb', rom(seed=4))
        self.put('CHROMAGIC/ART/GB/Has Cover.gb.cma', b'mine')
        fetched = []
        loader = lambda group, system: library.PackIndex(group, system, INDEX if system == 'GAMEBOY' else '')
        result = library.get_covers(library.FolderTarget(self.card), log=lambda *a: None, load_index=loader,
                                    download=lambda url: fetched.append(url) or jpeg())
        art = self.card / 'CHROMAGIC/ART/GB'
        self.assertEqual(len((art / 'Favourites/Golf (World).gb.cma').read_bytes()), library.COVER_BYTES)
        self.assertTrue((art / 'renamed.gb.cma').is_file())
        self.assertFalse((art / 'Unknown Homebrew.gb.cma').exists())
        self.assertEqual((art / 'Has Cover.gb.cma').read_bytes(), b'mine')
        self.assertEqual(len(fetched), 2)
        self.assertIn('Added box art for 2 games', result)
        self.assertIn('No box art was found for 1 game', result)

    def test_replace(self):
        self.put('CHROMAGIC/BACKUPS/GB/Golf (World).gb', rom(seed=3))
        self.put('CHROMAGIC/ART/GB/Golf (World).gb.cma', b'old')
        loader = lambda group, system: library.PackIndex(group, system, INDEX)
        library.get_covers(library.FolderTarget(self.card), replace=True, log=lambda *a: None, load_index=loader,
                           download=lambda url: jpeg())
        self.assertEqual(len((self.card / 'CHROMAGIC/ART/GB/Golf (World).gb.cma').read_bytes()), library.COVER_BYTES)

    def test_clips_match_by_name_then_title(self):
        games = [('CHROMAGIC/BACKUPS/GB/Tetris (World).gb', 1, 'GB'), ('CHROMAGIC/BACKUPS/GB/Tetris (Japan).gb', 1, 'GB'),
                 ('CHROMAGIC/BACKUPS/GG/Sonic.gg', 1, 'GG')]
        pairs, unmatched = library.match_clips([Path('Tetris (World).mp4'), Path('tetris.mp4'), Path('Zelda.mp4')], games)
        self.assertEqual(pairs[Path('Tetris (World).mp4')], ['CHROMAGIC/BACKUPS/GB/Tetris (World).gb'])
        self.assertEqual(len(pairs[Path('tetris.mp4')]), 2)
        self.assertEqual(unmatched, [Path('Zelda.mp4')])

    def test_preview_from_a_clip(self):
        ffmpeg = library.find_ffmpeg()
        if not ffmpeg:
            self.skipTest('no FFmpeg')
        import subprocess
        clip = self.tmp / 'Golf.mp4'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc=duration=12:size=320x240:rate=30',
                        '-pix_fmt', 'yuv420p', str(clip)], check=True, timeout=120)
        data = library.preview_bytes(ffmpeg, clip)
        self.assertEqual(len(data), library.VIDEO_BYTES)
        self.assertEqual(data[:4], b'CMVP')
        short = self.tmp / 'Short.mp4'
        subprocess.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc=duration=4:size=320x240:rate=30',
                        '-pix_fmt', 'yuv420p', str(short)], check=True, timeout=120)
        with self.assertRaises(ValueError):
            library.preview_bytes(ffmpeg, short)

    def test_cover_matches_the_firmware_tool(self):
        if str(TOOLS) not in sys.path:
            sys.path.insert(0, str(TOOLS))
        from prepare_previews import convert_cover
        source = self.tmp / 'c.jpg'; source.write_bytes(jpeg((250, 0, 250), (700, 700)))
        dest = self.tmp / 'c.cma'
        convert_cover(source, dest)
        self.assertEqual(library.cover_bytes(source.read_bytes()), dest.read_bytes())


class MacMetadata(unittest.TestCase):
    """On a Mac, ._ metadata twins under CHROMAGIC are removed when the card target closes."""
    def test_dot_clean_on_close(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'CHROMAGIC' / 'BACKUPS').mkdir(parents=True)
            (root / '._CHROMAGIC').write_bytes(b'x')
            target = library.FolderTarget(d)
            with unittest.mock.patch.object(library.sys, 'platform', 'darwin'), \
                    unittest.mock.patch.object(library.subprocess, 'run') as run:
                target.close()
            run.assert_called_once()
            self.assertEqual(run.call_args[0][0], ['dot_clean', '-m', str(root / 'CHROMAGIC')])
            self.assertFalse((root / '._CHROMAGIC').exists())

    def test_nothing_elsewhere(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / '._CHROMAGIC').write_bytes(b'x')   # no CHROMAGIC folder: leave the card alone
            for platform in ('darwin', 'win32', 'linux'):
                with unittest.mock.patch.object(library.sys, 'platform', platform), \
                        unittest.mock.patch.object(library.subprocess, 'run') as run:
                    library.FolderTarget(d).close()
                run.assert_not_called()
            self.assertTrue((Path(d) / '._CHROMAGIC').exists())


class FakeChromatic(BaseHTTPRequestHandler):
    """Just enough of mcu/main/wifi_files.c: session, list, folder, file PUT (no overwrite), DELETE, checksum."""
    files, token, can_delete = {}, 'tok', True

    def log_message(self, *a):
        pass

    def reply(self, code, body=b'', kind='application/json'):
        self.send_response(code); self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)

    def path_arg(self):
        if '?path=' not in self.path:
            return None
        rel = unquote(self.path.split('?path=', 1)[1])
        assert rel.startswith('/'), 'the device rejects paths without a leading / (400)'
        return rel[1:]

    def authorised(self):
        if self.headers.get('X-ChroMagic-Token') != self.token:
            self.reply(401, b'{"error":"Pair with the Chromatic first."}'); return False
        return True

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        if self.path == '/api/session':
            ok = json.loads(body).get('code') == '123456'
            return self.reply(200, json.dumps({'token': self.token}).encode()) if ok else self.reply(401, b'{"error":"bad code"}')
        if self.authorised():
            rel = self.path_arg()
            if rel in self.files:
                return self.reply(409, b'{"error":"exists"}')
            self.files[rel] = None; self.reply(201, b'{"ok":true}')

    def do_PUT(self):
        if self.authorised():
            rel, body = self.path_arg(), self.rfile.read(int(self.headers['Content-Length']))
            if rel in self.files:
                return self.reply(409, b'{"error":"exists"}')
            self.files[rel] = body; self.reply(201, b'{"ok":true}')

    def do_DELETE(self):
        if not self.can_delete:
            return self.reply(405, b'Method Not Allowed', 'text/plain')
        if self.authorised():
            rel = self.path_arg()
            if self.files.get(rel) is None:   # missing, or a folder
                return self.reply(404, b'{"error":"File unavailable."}')
            del self.files[rel]; self.reply(200, b'{"ok":true}')

    def do_GET(self):
        if not self.authorised():
            return
        rel = self.path_arg()
        if self.path.startswith('/api/list'):
            if rel and rel not in self.files:
                return self.reply(404, b'{"error":"Folder unavailable."}')
            rows = []
            for name, data in self.files.items():
                parent, _, leaf = name.rpartition('/')
                if parent == rel:
                    rows.append(json.dumps({'name': leaf, 'directory': data is None, 'size': len(data or b'')}))
            return self.reply(200, ('\n'.join(rows) + '\n').encode(), 'application/x-ndjson')
        if self.path.startswith('/api/checksum'):
            data = self.files[rel]
            return self.reply(200, json.dumps({'crc32': '%08x' % zlib.crc32(data), 'bytes': len(data)}).encode())


class Wifi(unittest.TestCase):
    def setUp(self):
        FakeChromatic.files = {'CHROMAGIC': None, 'CHROMAGIC/BACKUPS': None, 'CHROMAGIC/BACKUPS/GB': None,
                               'CHROMAGIC/BACKUPS/GB/mine.gb': rom(seed=1), 'CHROMAGIC/BACKUPS/GB/Golf (World).gb': rom(seed=3)}
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), FakeChromatic)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.shutdown)
        self.address = '127.0.0.1:%d' % self.server.server_port
        # The address check only allows dotted IPs (optionally :80); the test server runs on another port.
        patcher = unittest.mock.patch.object(library.re, 'fullmatch', return_value=True)
        patcher.start(); self.addCleanup(patcher.stop)

    def test_wrong_code(self):
        with self.assertRaises(library.WifiError):
            library.WifiTarget(self.address, '000000')

    def test_covers_over_wifi_use_device_checksums(self):
        target = library.WifiTarget(self.address, '123456')
        loader = lambda group, system: library.PackIndex(group, system, INDEX)
        result = library.get_covers(target, log=lambda *a: None, load_index=loader, download=lambda url: jpeg())
        files = FakeChromatic.files
        self.assertEqual(len(files['CHROMAGIC/ART/GB/mine.gb.cma']), library.COVER_BYTES)   # by device checksum
        self.assertIn('CHROMAGIC/ART/GB/Golf (World).gb.cma', files)
        self.assertIn('Added box art for 2 games', result)
        # A second run finds both covers and writes nothing.
        target.forget('CHROMAGIC/ART'); target.forget('CHROMAGIC/ART/GB')
        self.assertIn('already has box art', library.get_covers(target, log=lambda *a: None, load_index=loader))

    def test_remove_over_wifi(self):
        FakeChromatic.files.update({'CHROMAGIC/ART': None, 'CHROMAGIC/ART/GB': None,
                                    'CHROMAGIC/ART/GB/mine.gb.cma': b'c', 'CHROMAGIC/BACKUPS/GB/mine.sav': b's'})
        target = library.WifiTarget(self.address, '123456')
        result = library.remove_games(target, ['CHROMAGIC/BACKUPS/GB/mine.gb'], log=lambda *a: None)
        self.assertNotIn('CHROMAGIC/BACKUPS/GB/mine.gb', FakeChromatic.files)
        self.assertNotIn('CHROMAGIC/ART/GB/mine.gb.cma', FakeChromatic.files)
        self.assertIn('CHROMAGIC/BACKUPS/GB/mine.sav', FakeChromatic.files)
        self.assertIn('Removed 1 game and 1 box art or clip.', result)

    def test_remove_over_wifi_on_older_firmware(self):
        FakeChromatic.can_delete = False
        self.addCleanup(setattr, FakeChromatic, 'can_delete', True)
        target = library.WifiTarget(self.address, '123456')
        with self.assertRaisesRegex(ValueError, 'Update Magic Trick'):
            library.remove_games(target, ['CHROMAGIC/BACKUPS/GB/mine.gb'], log=lambda *a: None)
        self.assertIn('CHROMAGIC/BACKUPS/GB/mine.gb', FakeChromatic.files)


if __name__ == '__main__':
    unittest.main()
