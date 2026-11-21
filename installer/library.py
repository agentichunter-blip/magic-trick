"""SD card library: add games, cover art and gameplay previews in the places Magic Trick reads them.

The card is reached through a Target: a card reader (a mounted folder), the Chromatic's Wi-Fi Files
server, or USB (the ChroMagician "pcsd" protocol, slow). Paths inside a target are relative to the
SD card root and use '/', e.g. 'CHROMAGIC/BACKUPS/GB/Tetris (World).gb'.

Layout (mcu/main/rom_browser.c): games live under CHROMAGIC/BACKUPS/<system>/..., and each game's
cover and preview mirror its path, extension included:
    CHROMAGIC/ART/GB/Tetris (World).gb.cma     CHROMAGIC/VIDEO/GB/Tetris (World).gb.cmv

Cover art comes from the MiSTer Artwork Pack (box art from ScreenScraper, collected by chipster6502).
It is downloaded on this computer when the user asks for it; nothing is bundled or hosted by us.
Format and matching rules: https://github.com/chipster6502/MiSTer_artwork_pack/blob/main/PACK_FORMAT.md
"""
import array, io, json, os, re, shutil, ssl, struct, subprocess, sys, time, urllib.error, urllib.parse
import urllib.request, zipfile, zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

GAMES, ART, VIDEO = 'CHROMAGIC/BACKUPS', 'CHROMAGIC/ART', 'CHROMAGIC/VIDEO'
EXTENSIONS = {'.gb': 'GB', '.gbc': 'GBC', '.gg': 'GG', '.lnx': 'LYNX', '.lyx': 'LYNX'}
SYSTEM_NAMES = {'GB': 'Game Boy', 'GBC': 'Game Boy Color', 'GG': 'Game Gear', 'LYNX': 'Lynx'}
VIDEO_EXTENSIONS = {'.mp4', '.mkv', '.webm', '.mov', '.avi', '.m4v'}

# Which pack catalogues to try for each system folder, in order (PACK_FORMAT.md "Shared catalogues").
PACK_URL = 'https://raw.githubusercontent.com/chipster6502/artworkdb-{group}/media-box2d/docs/{system}/Artwork/{file}'
PACKS = {
    'GB': [('nintendo-handhelds', 'GAMEBOY'), ('nintendo-handhelds', 'GBC')],
    'GBC': [('nintendo-handhelds', 'GBC'), ('nintendo-handhelds', 'GAMEBOY')],
    'GG': [('sega', 'GameGear')],
    'LYNX': [('atari', 'AtariLynx')],
}
ART_CREDIT = 'Box art: ScreenScraper, via the MiSTer Artwork Pack by chipster6502.'

# Cover and preview formats (docs/media.md; same output as mcu/tools/prepare_previews.py).
WIDTH, HEIGHT, FPS, SECONDS = 128, 92, 12, 8
FRAME_BYTES = WIDTH * HEIGHT * 2
COVER_HEADER = struct.pack('<4sBBHHHI', b'CMAR', 2, 1, WIDTH, HEIGHT, 0, FRAME_BYTES)
VIDEO_HEADER = struct.Struct('<4sBBHHHIII')
COVER_BYTES = len(COVER_HEADER) + FRAME_BYTES
VIDEO_BYTES = VIDEO_HEADER.size + FPS * SECONDS * FRAME_BYTES

USB_SPEED = 3200      # bytes per second written over USB (3.0-3.5 KB/s measured 2026-11-17)
# Firmware up to MCU 1.0.0-beta.6 stats every entry of a listed folder, which rescans the folder, so a
# listing is quadratic: ~25 s for 400 games over USB (measured 2026-11-17). Allow for big folders.
LIST_TIMEOUT = 600
WIFI_SPEED = 45000    # bytes per second written over Wi-Fi (37-59 KB/s measured 2026-11-17)


def emit(kind, **data):
    try:
        import core
        core.emit(kind, **data)
    except ImportError:
        pass


def plural(n, word, words=None):
    return '%d %s' % (n, word if n == 1 else (words or word + 's'))


def progress(done, total, what):
    print('Progress: %d of %d %s' % (done, total, what), flush=True)


def minutes(nbytes, speed):
    return max(1, round(nbytes / speed / 60))


# ---------------------------------------------------------------- targets

class Target:
    """One way of reaching the SD card. Subclasses implement _list, put, mkdir and optionally crc."""
    name = 'card'
    speed = None          # bytes/s for writes, None when it's fast enough not to mention
    can_replace = True

    def __init__(self):
        self._listings = {}

    def list(self, rel):
        """[(name, is_dir, size)] for a folder, or [] if it doesn't exist."""
        if rel not in self._listings:
            if self.speed and rel.count('/') >= 2:
                print('Reading %s…' % rel, flush=True)   # big folders take a while over Wi-Fi and USB
            self._listings[rel] = self._list(rel)
        return self._listings[rel]

    def forget(self, rel):
        self._listings.pop(rel, None)

    def walk(self, rel):
        """Every file below rel as (path, size), skipping hidden and macOS '._' files."""
        out = []
        for name, is_dir, size in self.list(rel):
            if name.startswith('.'):
                continue
            path = rel + '/' + name
            if is_dir:
                out += self.walk(path)
            else:
                out.append((path, size))
        return out

    def ensure_dir(self, rel):
        parts = rel.split('/')
        for i in range(1, len(parts) + 1):
            parent, name = '/'.join(parts[:i - 1]), parts[i - 1]
            entries = self.list(parent) if parent else self.list('')
            match = [e for e in entries if e[0].lower() == name.lower()]
            if match:
                if not match[0][1]:
                    raise ValueError('"%s" on the card is a file, not a folder.' % '/'.join(parts[:i]))
                parts[i - 1] = match[0][0]   # FAT is case-insensitive: reuse the existing spelling
                continue
            self.mkdir('/'.join(parts[:i]))
            self.forget(parent)
        return '/'.join(parts)

    def write(self, rel, data, replace=False):
        folder, name = rel.rsplit('/', 1)
        folder = self.ensure_dir(folder)
        self.put(folder + '/' + name, data, replace)
        self.forget(folder)

    def crc(self, rel):
        """(crc32, size) of a file on the card, or None if this target can't do it quickly."""
        return None

    def find(self, rel):
        """The card's spelling of a file (FAT ignores case), or None if it isn't there."""
        folder, name = rel.rsplit('/', 1)
        match = [n for n, is_dir, _ in self.list(folder) if not is_dir and n.lower() == name.lower()]
        return folder + '/' + match[0] if match else None

    def delete(self, rel):
        """Delete one file that a listing showed. Subclasses implement remove."""
        self.remove(rel)
        self.forget(rel.rsplit('/', 1)[0])

    def close(self):
        pass


class FolderTarget(Target):
    """The SD card in a card reader (or any folder holding a copy of it)."""
    name = 'card reader'

    def __init__(self, path):
        super().__init__()
        root = Path(path)
        if root.name.upper() == 'CHROMAGIC':   # accept the CHROMAGIC folder as well as the card itself
            root = root.parent
        if not root.is_dir():
            raise ValueError('That folder doesn\'t exist: ' + str(path))
        self.root = root

    def _path(self, rel):
        return self.root.joinpath(*[p for p in rel.split('/') if p])

    def _list(self, rel):
        try:
            with os.scandir(self._path(rel)) as it:
                return [(e.name, e.is_dir(), 0 if e.is_dir() else e.stat().st_size) for e in it]
        except (FileNotFoundError, NotADirectoryError):
            return []

    def mkdir(self, rel):
        self._path(rel).mkdir(exist_ok=True)

    def put(self, rel, data, replace=False):
        target = self._path(rel)
        if target.exists() and not replace:
            raise FileExistsError(rel)
        temp = target.with_name(target.name + '.mt-tmp')
        temp.write_bytes(data)
        temp.replace(target)

    def remove(self, rel):
        path = self._path(rel)
        path.unlink()
        twin = path.with_name('._' + path.name)   # macOS metadata twin on a FAT card
        if twin.is_file():
            twin.unlink()

    def crc(self, rel):
        data = self._path(rel).read_bytes()
        return rom_crcs(rel, data)

    def close(self):
        """On a Mac, files written to a FAT card get hidden ._ twins holding their metadata (such as
        the provenance tag macOS adds to files a downloaded app writes). Remove the ones under
        CHROMAGIC with macOS's own dot_clean; the Chromatic and Wi-Fi Files would list them."""
        if sys.platform != 'darwin':
            return
        top = self._path(GAMES).parent   # the CHROMAGIC folder
        if not top.is_dir():
            return
        try:
            subprocess.run(['dot_clean', '-m', str(top)], capture_output=True, timeout=120)
            stray = top.parent / ('._' + top.name)
            if stray.is_file():
                stray.unlink()
        except (OSError, subprocess.SubprocessError):
            pass   # cosmetic: the games are written either way


class WifiTarget(Target):
    """The Chromatic's Wi-Fi Files server (Settings > Wi-Fi Files). Needs the address and WEB CODE it shows."""
    name = 'Wi-Fi'
    speed = WIFI_SPEED
    can_replace = False   # the server never overwrites (409); deleting needs MCU 1.0.0-beta.9 or later

    def __init__(self, address, code, timeout=60):
        super().__init__()
        address = address.strip().removeprefix('http://').rstrip('/')
        if not re.fullmatch(r'[0-9.]+(:80)?', address):
            raise ValueError('Type the address the Chromatic shows, like 192.168.1.23.')
        self.base, self.timeout = 'http://' + address, timeout
        reply = self._request('POST', 'session', body=json.dumps({'code': code.strip().lower()}).encode(),
                              headers={'Content-Type': 'application/json'}, auth=False)
        self.token = json.loads(reply)['token']

    def _request(self, method, route, rel=None, body=None, headers=None, auth=True, timeout=None):
        url = self.base + '/api/' + route
        if rel is not None:
            url += '?path=' + urllib.parse.quote('/' + rel, safe='')   # the device wants a leading /
        req = urllib.request.Request(url, data=body, method=method, headers=dict(headers or {}))
        if auth:
            req.add_header('X-ChroMagic-Token', self.token)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                return r.read()
        except urllib.error.HTTPError as exc:
            try:
                detail = json.loads(exc.read()).get('error') or exc.reason
            except ValueError:
                detail = exc.reason
            raise WifiError(exc.code, str(detail)) from None
        except (urllib.error.URLError, OSError) as exc:
            raise ConnectionError('Couldn\'t reach the Chromatic at %s: %s. Check that Wi-Fi Files is open on '
                                  'it and this computer is on the same network.' % (self.base, exc)) from None

    def _list(self, rel):
        try:
            text = self._request('GET', 'list', rel, timeout=LIST_TIMEOUT).decode('utf-8')
        except WifiError as exc:
            if exc.status == 404:
                return []
            raise
        rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        return [(r['name'], bool(r['directory']), int(r.get('size') or 0)) for r in rows]

    def mkdir(self, rel):
        self._request('POST', 'folder', rel)

    def put(self, rel, data, replace=False):
        # The server refuses an existing name before reading the body and closes the connection, which
        # reaches us as a reset rather than a 409, so check the (cached) folder listing first.
        folder, name = rel.rsplit('/', 1)
        if any(n.lower() == name.lower() for n, _, _ in self.list(folder)):
            raise FileExistsError(rel)
        try:
            self._request('PUT', 'file', rel, body=data, headers={'Content-Type': 'application/octet-stream'})
        except WifiError as exc:
            if exc.status == 409:
                raise FileExistsError(rel) from None
            raise

    def crc(self, rel):
        reply = json.loads(self._request('GET', 'checksum', rel))   # the Chromatic reads the file itself
        return [(int(reply['crc32'], 16), int(reply['bytes']))]

    def remove(self, rel):
        try:
            self._request('DELETE', 'file', rel)
        except WifiError as exc:
            if exc.status == 405:   # firmware before DELETE /api/file
                raise ValueError('This Chromatic\'s firmware can\'t delete over Wi-Fi yet. Update Magic Trick, or '
                                 'use a card reader or USB.') from None
            if exc.status == 404:
                raise FileNotFoundError(rel) from None
            raise


class WifiError(Exception):
    def __init__(self, status, text):
        super().__init__('The Chromatic said: %s (%d)' % (text, status))
        self.status = status


class UsbTarget(Target):
    """USB file transfers (menu: USB TRANSFERS, with Wi-Fi off). About 3 KB/s for writes."""
    name = 'USB'
    speed = USB_SPEED
    BAUD = 2_000_000

    def __init__(self, port_name, log=print):
        super().__init__()
        import serial
        port = serial.Serial(None, self.BAUD, timeout=0.25, dsrdtr=False, rtscts=False)
        port.port, port.dtr, port.rts = port_name, False, False
        port.open()
        self.port, self.seq, self._buf, self.deadline = port, 0, b'', 0
        # macOS raises DTR/RTS whenever a port opens, which restarts the MCU: wait for its console.
        start, seen = time.time(), b''
        while time.time() - start < 40:
            port.write(b'\r'); seen = (seen + port.read(4096))[-64:]
            if b'mcu>' in seen or b'fwversion' in seen:
                break
        time.sleep(3)
        port.reset_input_buffer(); port.write(b'\r\n'); port.flush(); time.sleep(0.2); port.reset_input_buffer()
        on = None
        for _ in range(4):   # just after a cart session the console can be slow to answer
            on = self._usb_transfers_on()
            if on is not None:
                break
            time.sleep(1)
        if on is None:
            port.close()
            raise ValueError('The Chromatic didn\'t answer over USB. Switch it off and on, then try again.')
        if not on:
            port.close()
            raise ValueError('USB TRANSFERS is off. On the Chromatic, press Menu and open the System tab: set WI-FI '
                             'off and USB TRANSFERS on, then try again.')
        self._timeout(30)
        port.write(b'\rpcsd\r'); port.flush()
        ready = self._record()
        if ready != 'READY protocol=1 block=1024':
            port.close()
            raise ValueError('The Chromatic didn\'t start a USB transfer session (%r).' % ready)
        log('Connected over USB. Writes run at about 3 KB/s.')

    def _usb_transfers_on(self):
        for baud in (self.BAUD, 115_200):
            self.port.baudrate = baud
            self.port.reset_input_buffer()
            self.port.write(b'\x15\rpcstatus\r')
            end, buf = time.time() + 0.75, b''
            while time.time() < end:
                buf += self.port.read(512)
                m = re.search(rb'PCSTATUS enabled=([01])', buf)
                if m:
                    self.port.baudrate = self.BAUD
                    return m.group(1) == b'1'
        self.port.baudrate = self.BAUD
        return None   # no answer (not the same as off)

    def _timeout(self, seconds):
        self.deadline = time.time() + seconds

    def _record(self):
        while time.time() < self.deadline:
            while b'\n' in self._buf:
                line, self._buf = self._buf.split(b'\n', 1)
                p = line.find(b'PCSD ')
                if p >= 0:
                    text = line[p + 5:].decode('utf-8', 'replace').strip()
                    if text.startswith('FAIL error='):
                        raise UsbError(text[11:])
                    return text
            self._buf += self.port.read(512)
        raise TimeoutError('The Chromatic stopped answering over USB.')

    def _send(self, payload):
        frame = (b'CF01' + self.seq.to_bytes(4, 'little') + len(payload).to_bytes(4, 'little') +
                 zlib.crc32(payload).to_bytes(4, 'little') + payload)
        for i in range(0, len(frame), 128):   # the device drops unpaced frames
            self.port.write(frame[i:i + 128]); self.port.flush(); time.sleep(0.001)

    def _ok(self):
        rec = self._record()
        if rec != 'OK seq=%d' % self.seq:
            raise UsbError('expected OK seq=%d, got %r' % (self.seq, rec))
        self.seq += 1

    def _exchange(self, payload, seconds=60):
        self._timeout(seconds); self._send(payload); self._ok()

    def _list(self, rel):
        # Any failed command ends the device's whole session, so never ask for a folder that isn't there.
        if rel:
            parent, name = rel.rsplit('/', 1) if '/' in rel else ('', rel)
            if not any(n == name and d for n, d, _ in self.list(parent)):
                return []
        self._timeout(LIST_TIMEOUT)
        self._send(b'\x02' + ('/' + rel).encode())
        out = []
        while True:
            rec = self._record()
            if rec.startswith('ENTRY '):
                e = json.loads(rec[6:])
                out.append((e['name'], bool(e['directory']), int(e.get('size') or 0)))
            elif rec == 'OK seq=%d' % self.seq:
                self.seq += 1
                return out
            else:
                raise UsbError('unexpected listing reply %r' % rec)

    def mkdir(self, rel):
        self._exchange(b'\x04/' + rel.encode())

    def put(self, rel, data, replace=False):
        if any(n.lower() == rel.rsplit('/', 1)[1].lower() for n, _, _ in self.list(rel.rsplit('/', 1)[0])):
            if not replace:
                raise FileExistsError(rel)
            self._exchange(b'\x05/' + rel.encode())
        self._exchange(b'\x07' + len(data).to_bytes(4, 'little') + zlib.crc32(data).to_bytes(4, 'little') +
                       ('/' + rel).encode())
        for i in range(0, len(data), 1024):
            self._exchange(b'\x08' + data[i:i + 1024])
        self._exchange(b'\x09', seconds=120)   # the device re-reads the file to verify it

    def remove(self, rel):
        self._exchange(b'\x05/' + rel.encode())   # only called for listed files: a failure ends the session

    def close(self):
        try:
            self._exchange(b'\0', seconds=10)
            self._record()
        except (UsbError, TimeoutError, OSError):
            pass
        finally:
            self.port.close()


class UsbError(Exception):
    def __init__(self, text):
        super().__init__('USB transfer failed: ' + text)


def open_target(spec, log=print):
    """'wifi:ADDRESS:CODE', 'usb:PORT' or a folder path (used by the command line)."""
    if spec.startswith('wifi:'):
        address, code = spec[5:].rsplit(':', 1)
        return WifiTarget(address, code)
    if spec.startswith('usb:'):
        return UsbTarget(spec[4:], log)
    return FolderTarget(spec)


# ---------------------------------------------------------------- games on the card

def system_of(name, data=None):
    """System folder for a game file, from its extension (and the Game Boy header's CGB flag)."""
    ext = os.path.splitext(name)[1].lower()
    system = EXTENSIONS.get(ext)
    if system == 'GB' and data is not None and len(data) > 0x143 and data[0x143] == 0xC0:
        return 'GBC'   # Game Boy Color only, despite a .gb name
    return system


def card_games(target):
    """[(path, size, system)] for every game under CHROMAGIC/BACKUPS."""
    out = []
    for path, size in target.walk(GAMES):
        top = path[len(GAMES) + 1:].split('/', 1)[0].upper()
        system = top if top in PACKS and '/' in path[len(GAMES) + 1:] else system_of(path)
        if system and os.path.splitext(path)[1].lower() in EXTENSIONS:
            out.append((path, size, system))
    return out


def mirror(game_path, folder, suffix):
    """CHROMAGIC/BACKUPS/GB/x.gb -> CHROMAGIC/ART/GB/x.gb.cma"""
    return folder + game_path[len(GAMES):] + suffix


def existing(target, folder):
    return {p.lower() for p, _ in target.walk(folder)}


def rom_crcs(name, data):
    """CRC32/size pairs a dump may be catalogued under (Lynx: also without its 64-byte .lnx header)."""
    out = [(zlib.crc32(data), len(data))]
    if data[:4] == b'LYNX' and len(data) > 64:
        out.append((zlib.crc32(data[64:]), len(data) - 64))
    return out


# ---------------------------------------------------------------- adding games

def collect_games(sources):
    """Game files from files, folders and .zip archives: [(name, loader, size)]."""
    found = []
    for source in map(Path, sources):
        files = sorted(p for p in source.rglob('*') if p.is_file()) if source.is_dir() else [source]
        for f in files:
            if f.name.startswith('.'):
                continue
            ext = f.suffix.lower()
            if ext in EXTENSIONS:
                found.append((f.name, f.read_bytes, f.stat().st_size))
            elif ext == '.zip':
                try:
                    with zipfile.ZipFile(f) as z:
                        for info in z.infolist():
                            inner = Path(info.filename).name
                            if not info.is_dir() and Path(inner).suffix.lower() in EXTENSIONS:
                                found.append((inner, lambda f=f, n=info.filename: zipfile.ZipFile(f).read(n),
                                              info.file_size))
                except zipfile.BadZipFile:
                    print('Skipped %s: not a readable zip file.' % f.name)
    return found


LETTER_RANGE = re.compile(r'^([0-9A-Z])-([0-9A-Z])$', re.I)


def letter_folder(target, system_folder, name):
    """The A-Z range folder (like 0-F, G-L, M-R, S-Z) a game belongs in, if the system folder is split that way.
    Numbers, symbols and non-Latin titles go to the range that starts with 0 (or the first range)."""
    ranges = []
    for entry, is_dir, _ in target.list(system_folder):
        m = LETTER_RANGE.match(entry) if is_dir else None
        if m:
            ranges.append((m.group(1).upper(), m.group(2).upper(), entry))
    if not ranges:
        return None
    ranges.sort()
    first = name[:1].upper()
    if not ('A' <= first <= 'Z' or '0' <= first <= '9'):
        first = '0'
    for low, high, entry in ranges:
        if low <= first <= high:
            return entry
    return ranges[0][2] if first < ranges[0][0] else ranges[-1][2]


def add_games(target, sources, log=print):
    """Copy game files into CHROMAGIC/BACKUPS/<system>/. Games already on the card (same name) are skipped."""
    emit('step', key='lib_scan')
    games = collect_games(sources)
    if not games:
        raise ValueError('No games found there. Pick .gb, .gbc, .gg, .lnx or .lyx files, or zip files with '
                         'them inside.')
    on_card = {}
    for path, size, system in card_games(target):
        on_card.setdefault(Path(path).name.lower(), size)
    todo, skipped = [], 0
    seen = set()
    for name, loader, size in games:
        if name.lower() in on_card or name.lower() in seen:
            skipped += 1
            continue
        seen.add(name.lower())
        todo.append((name, loader, size))
    total = sum(s for _, _, s in todo)
    log('Found %d games: %d new, %d already on the card.' % (len(games), len(todo), skipped))
    if target.speed and todo:
        log('About %d min to copy %.1f MB over %s.' % (minutes(total, target.speed), total / 1e6, target.name))
    emit('step', key='lib_copy')
    added, systems = 0, {}
    for i, (name, loader, size) in enumerate(todo):
        data = loader()
        system = system_of(name, data)
        folder = '%s/%s' % (GAMES, system)
        sub = letter_folder(target, folder, name)   # keep a card that's sorted A-Z sorted
        if sub:
            folder += '/' + sub
        try:
            target.write('%s/%s' % (folder, name), data)
            added += 1; systems[system] = systems.get(system, 0) + 1
            log('Added %s' % (folder[len(GAMES) + 1:] + '/' + name))
        except FileExistsError:
            skipped += 1
        progress(i + 1, len(todo), 'games')
    by = ', '.join('%d %s' % (n, SYSTEM_NAMES[s]) for s, n in sorted(systems.items()))
    text = 'Added %s.' % plural(added, 'game') + (' (' + by + ')' if by else '')
    if skipped:
        text += (' 1 was already on the card, so it was skipped.' if skipped == 1 else
                 ' %d were already on the card, so they were skipped.' % skipped)
    return text


def remove_games(target, paths, log=print):
    """Delete games (card paths under CHROMAGIC/BACKUPS) with their box art and gameplay clip. Saves and
    save states sit next to each game and are kept, so a game added again later picks up where it was."""
    for path in paths:
        if not path.startswith(GAMES + '/') or os.path.splitext(path)[1].lower() not in EXTENSIONS:
            raise ValueError('Not a game on the card: ' + path)
    emit('step', key='lib_remove')
    removed, media, missing = 0, 0, 0
    for i, path in enumerate(paths):
        found = target.find(path)
        if not found:
            missing += 1
            log('Already gone: %s' % path[len(GAMES) + 1:])
        else:
            target.delete(found)
            removed += 1
            log('Removed %s' % found[len(GAMES) + 1:])
            for folder, suffix in ((ART, '.cma'), (VIDEO, '.cmv')):
                extra = target.find(mirror(found, folder, suffix))
                if extra:
                    target.delete(extra)
                    media += 1
        progress(i + 1, len(paths), 'games')
    text = 'Removed %s' % plural(removed, 'game')
    text += (' and %s.' % plural(media, 'box art or clip', 'box art and clips')) if media else '.'
    if missing:
        text += ' %s already gone.' % ('1 was' if missing == 1 else '%d were' % missing)
    return text + ' Their saves and save states are still on the card.'


# ---------------------------------------------------------------- cover art

def _ssl_context():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def fetch(url, attempts=3):
    for attempt in range(attempts):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'MagicTrickInstaller'})
            with urllib.request.urlopen(req, timeout=60, context=_ssl_context()) as r:
                return r.read()
        except urllib.error.HTTPError as exc:
            if exc.code == 404 or attempt == attempts - 1:
                raise
        except (urllib.error.URLError, OSError):
            if attempt == attempts - 1:
                raise
        time.sleep(1 + attempt)


def bare_title(name):
    """'F-18 Hornet (USA) [!]' -> 'f 18 hornet'"""
    name = re.sub(r'\s*[\(\[][^\)\]]*[\)\]]', '', name)
    return ' '.join(re.sub(r'[^0-9a-z]+', ' ', name.lower()).split())


class PackIndex:
    """One pack catalogue's index.tsv: every known dump -> the key of the image that represents it."""

    def __init__(self, group, system, text):
        self.group, self.system = group, system
        self.keys, self.names, self.crcs, self.titles = set(), {}, {}, {}
        for line in text.splitlines():
            if not line or line.startswith('#'):
                continue
            cols = line.split('\t')
            if len(cols) < 4:
                continue
            name, crc, size, key = cols[:4]
            self.keys.add(key)
            self.names[name.lower()] = key
            if crc and size:
                self.crcs[(int(crc, 16), int(size))] = key
            self.titles.setdefault(bare_title(name), set()).add(key)

    def url(self, key):
        return PACK_URL.format(group=self.group, system=self.system, file=urllib.parse.quote(key + '.jpg'))

    @classmethod
    def load(cls, group, system):
        return cls(group, system, fetch(PACK_URL.format(group=group, system=system, file='index.tsv')).decode('utf-8'))


def resolve(stem, indexes, crcs=None):
    """Match a game to a pack image: exact name, catalogued name, CRC32+size, then a unique bare title.
    Returns (index, key, how) or None. Steps run across every sibling catalogue before the next step."""
    lower = stem.lower()
    for index in indexes:
        if stem in index.keys:
            return index, stem, 'name'
    for index in indexes:
        if lower in index.names:
            return index, index.names[lower], 'name'
    for crc in crcs or ():
        for index in indexes:
            if crc in index.crcs:
                return index, index.crcs[crc], 'checksum'
    title = bare_title(stem)
    for index in indexes:
        keys = index.titles.get(title, set())
        if len(keys) == 1:
            return index, next(iter(keys)), 'title'
        if len(keys) > 1:
            return None   # a coin-flip image is worse than none
    return None


def cover_bytes(image_data):
    """A JPEG/PNG -> CMAR v2: 128x92 RGB565 with black padding (mcu/tools/prepare_previews.convert_cover)."""
    from PIL import Image, ImageOps
    with Image.open(io.BytesIO(image_data)) as opened:
        image = ImageOps.exif_transpose(opened).convert('RGBA')
    image = ImageOps.contain(image, (WIDTH, HEIGHT), Image.Resampling.LANCZOS).convert('RGBA')
    canvas = Image.new('RGBA', (WIDTH, HEIGHT), (0, 0, 0, 255))
    canvas.alpha_composite(image, ((WIDTH - image.width) // 2, (HEIGHT - image.height) // 2))
    pixels = array.array('H', (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in canvas.convert('RGB').getdata()))
    if sys.byteorder != 'little':
        pixels.byteswap()
    return COVER_HEADER + opaque(pixels.tobytes())


def opaque(data):
    """Magenta 0xF81F is the firmware's transparent key: nudge it to a neighbouring opaque colour."""
    data = bytearray(data)
    start = 0
    while True:
        i = data.find(b'\x1f\xf8', start)
        if i < 0:
            return bytes(data)
        if i % 2 == 0:
            data[i] = 0x1e
        start = i + 1


def get_covers(target, replace=False, log=print, load_index=PackIndex.load, download=fetch):
    """Find a cover for every game without one and write it next to the game's path under CHROMAGIC/ART."""
    emit('step', key='lib_scan')
    games = card_games(target)
    if not games:
        raise ValueError('There are no games on this card yet. Add some games first.')
    have = existing(target, ART)
    replace = replace and target.can_replace
    todo = [(p, s, sys_) for p, s, sys_ in games if replace or mirror(p, ART, '.cma').lower() not in have]
    log('%d games on the card; %d need a cover.' % (len(games), len(todo)))
    if not todo:
        return 'Every game on the card already has box art. Nothing to add.'

    emit('step', key='art_match')
    indexes, loaded = {}, {}
    for system in sorted({s for _, _, s in todo}):
        indexes[system] = []
        for group, pack_system in PACKS[system]:
            if (group, pack_system) not in loaded:
                log('Downloading the %s cover list…' % pack_system)
                loaded[(group, pack_system)] = load_index(group, pack_system)
            indexes[system].append(loaded[(group, pack_system)])
    matched, missing = [], []
    for i, (path, size, system) in enumerate(todo):
        stem = os.path.splitext(Path(path).name)[0]
        found = resolve(stem, indexes[system])
        if not found:
            crcs = None
            try:
                crcs = target.crc(path)
            except Exception as exc:   # a checksum is only a better guess; carry on without it
                log('Couldn\'t checksum %s: %s' % (path, exc))
            if crcs:
                found = resolve(stem, indexes[system], crcs)
        if found:
            matched.append((path, found))
        else:
            missing.append(path)
        progress(i + 1, len(todo), 'games matched')
    log('Matched %d of %d games.' % (len(matched), len(todo)))
    if target.speed and matched:
        log('About %d min to write %d covers over %s.' % (minutes(len(matched) * COVER_BYTES, target.speed),
                                                          len(matched), target.name))

    emit('step', key='art_write')
    written, failed = 0, []
    cache = {}

    def make(item):
        path, (index, key, how) = item
        url = index.url(key)
        if url not in cache:
            cache[url] = cover_bytes(download(url))
        return cache[url]

    with ThreadPoolExecutor(6) as pool:
        futures = [(item, pool.submit(make, item)) for item in matched]
        for i, (item, future) in enumerate(futures):
            path, (index, key, how) = item
            try:
                target.write(mirror(path, ART, '.cma'), future.result(), replace)
                written += 1
            except FileExistsError:
                pass
            except (OSError, ValueError, urllib.error.URLError) as exc:
                failed.append(path); log('Couldn\'t add a cover for %s: %s' % (path, exc))
            progress(i + 1, len(futures), 'covers written')
    if missing:
        log('No cover found for %d games:' % len(missing))
        for path in missing:
            log('  ' + path[len(GAMES) + 1:])
    summary = 'Added box art for %s.' % plural(written, 'game')
    if missing:
        summary += ' No box art was found for %s (homebrew and rare games often have none).' % plural(len(missing), 'game')
    if failed:
        summary += ' %s couldn\'t be added; try again.' % plural(len(failed), 'one', 'ones')
    return summary


def covers_for(games, log=print, load_index=PackIndex.load, download=fetch):
    """Box art for games that aren't on a card (the Cartridge tab): [(name, data)] -> {name: cover}."""
    loaded, out = {}, {}
    for name, data in games:
        system = system_of(name, data)
        if not system:
            continue
        indexes = []
        for group, pack_system in PACKS[system]:
            if (group, pack_system) not in loaded:
                log('Downloading the %s cover list…' % pack_system)
                loaded[(group, pack_system)] = load_index(group, pack_system)
            indexes.append(loaded[(group, pack_system)])
        found = resolve(os.path.splitext(name)[0], indexes, rom_crcs(name, data))
        if not found:
            log('No box art found for %s.' % name)
            continue
        index, key, _ = found
        try:
            out[name] = cover_bytes(download(index.url(key)))
        except (OSError, ValueError, urllib.error.URLError) as exc:
            log('Couldn\'t get box art for %s: %s' % (name, exc))
    return out


# ---------------------------------------------------------------- gameplay previews

def find_ffmpeg():
    """The FFmpeg bundled with the installer (imageio-ffmpeg), else one installed on this computer."""
    try:
        import imageio_ffmpeg
        bundled = imageio_ffmpeg.get_ffmpeg_exe()
        if bundled and Path(bundled).is_file():
            return bundled
    except (ImportError, RuntimeError, OSError):
        pass
    found = shutil.which('ffmpeg')
    for candidate in ([found] if found else []) + ['/opt/homebrew/bin/ffmpeg', '/usr/local/bin/ffmpeg', '/usr/bin/ffmpeg']:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def _no_window():
    """Keep Windows from flashing a console window for each clip."""
    return {'creationflags': subprocess.CREATE_NO_WINDOW} if sys.platform == 'win32' else {}


def preview_bytes(ffmpeg, source):
    """Eight seconds at 12 fps, 128x92 RGB565, skipping the first two seconds (docs/media.md)."""
    cmd = [ffmpeg, '-nostdin', '-v', 'error', '-ss', '2', '-i', str(source), '-t', str(SECONDS), '-an', '-vf',
           f'fps={FPS},scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease:flags=neighbor,'
           f'pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:black',
           '-frames:v', str(FPS * SECONDS), '-pix_fmt', 'rgb565le', '-f', 'rawvideo', 'pipe:1']
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **_no_window())
    if result.returncode:
        raise ValueError(result.stderr.decode(errors='replace').strip() or 'FFmpeg failed')
    n, rem = divmod(len(result.stdout), FRAME_BYTES)
    if rem or n != FPS * SECONDS:
        raise ValueError('the clip must be at least ten seconds long')
    return VIDEO_HEADER.pack(b'CMVP', 1, 1, WIDTH, HEIGHT, FPS, n, FRAME_BYTES, 0) + opaque(result.stdout)


def match_clips(clips, games):
    """Pair each clip with the games it belongs to: same file name, else the same bare title
    (so one clip serves every region of a game). Returns ({clip: [game paths]}, [unmatched clips])."""
    by_stem, by_title = {}, {}
    for path, _, _ in games:
        stem = os.path.splitext(Path(path).name)[0]
        by_stem.setdefault(stem.lower(), []).append(path)
        by_title.setdefault(bare_title(stem), []).append(path)
    pairs, unmatched = {}, []
    for clip in clips:
        stem = Path(clip).stem
        hits = by_stem.get(stem.lower()) or by_title.get(bare_title(stem))
        if hits:
            pairs[clip] = hits
        else:
            unmatched.append(clip)
    return pairs, unmatched


def add_previews(target, sources, replace=False, log=print, ffmpeg=None):
    """Convert gameplay clips and write them next to their games under CHROMAGIC/VIDEO."""
    emit('step', key='lib_scan')
    ffmpeg = ffmpeg or find_ffmpeg()
    if not ffmpeg:
        raise ValueError('The video converter (FFmpeg) is missing from this installer. Download the installer '
                         'again, or install FFmpeg on this computer.')
    clips = []
    for source in map(Path, sources):
        clips += sorted(p for p in source.rglob('*') if p.suffix.lower() in VIDEO_EXTENSIONS) \
            if source.is_dir() else [source]
    clips = [c for c in clips if not c.name.startswith('.')]
    if not clips:
        raise ValueError('No video clips found there. Pick .mp4, .mkv, .webm, .mov, .avi or .m4v files. If you '
                         'downloaded a pack, unzip it first.')
    games = card_games(target)
    if not games:
        raise ValueError('There are no games on this card yet. Add the games first, then their clips.')
    pairs, unmatched = match_clips(clips, games)
    have = existing(target, VIDEO)
    replace = replace and target.can_replace
    jobs = [(clip, [g for g in hits if replace or mirror(g, VIDEO, '.cmv').lower() not in have])
            for clip, hits in pairs.items()]
    jobs = [(c, g) for c, g in jobs if g]
    count = sum(len(g) for _, g in jobs)
    log('%d clips: %d match games on the card, %d previews to write.' % (len(clips), len(pairs), count))
    if target.speed and count:
        log('About %d min to write %d previews over %s (2.3 MB each).' % (
            minutes(count * VIDEO_BYTES, target.speed), count, target.name))
    emit('step', key='vid_write')
    written, failed, done = 0, [], 0
    for clip, hits in jobs:
        try:
            data = preview_bytes(ffmpeg, clip)
        except (OSError, ValueError) as exc:
            failed.append(clip.name); log('Couldn\'t convert %s: %s' % (clip.name, exc))
            done += len(hits); progress(done, count, 'previews'); continue
        for game in hits:
            try:
                target.write(mirror(game, VIDEO, '.cmv'), data, replace)
                written += 1; log('Preview for %s' % game[len(GAMES) + 1:])
            except FileExistsError:
                pass
            done += 1; progress(done, count, 'previews')
    for clip in unmatched:
        log('No game on the card matches the clip %s' % Path(clip).name)
    summary = 'Added %s.' % plural(written, 'gameplay clip')
    if unmatched:
        summary += (' %s didn\'t match any game on the card, so %s skipped. To use one of yours, give it the '
                    'same name as the game file (for example Tetris (World).mp4).' % (
                        plural(len(unmatched), 'clip'), 'it was' if len(unmatched) == 1 else 'they were'))
    if failed:
        summary += ' %s couldn\'t be read (it must be a video at least 10 seconds long).' % plural(len(failed), 'clip')
    return summary


OPERATIONS = {'games': add_games, 'covers': get_covers, 'previews': add_previews, 'list': card_games,
              'remove': remove_games}


def main(argv):
    """app.py --library games|covers|previews|list|remove TARGET [FILES...] [--replace]
    TARGET: the SD card folder, wifi:ADDRESS:CODE, or usb:PORT. remove takes card paths, as list prints."""
    replace = '--replace' in argv
    args = [a for a in argv if a != '--replace']
    if len(args) < 2 or args[0] not in OPERATIONS:
        print(main.__doc__); return 2
    target = open_target(args[1])
    try:
        if args[0] == 'covers':
            result = get_covers(target, replace)
        elif args[0] == 'games':
            result = add_games(target, args[2:])
        elif args[0] == 'list':
            result = '\n'.join(path for path, _, _ in card_games(target))
        elif args[0] == 'remove':
            result = remove_games(target, args[2:])
        else:
            result = add_previews(target, args[2:], replace)
    finally:
        target.close()
    print(result)
    return 0
