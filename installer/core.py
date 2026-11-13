"""Magic Trick installer: three-system firmware install, upgrade and uninstall with backup gates."""
import hashlib, json, os, re, struct, subprocess, sys, threading, time
from pathlib import Path
import fpga

RELEASE = '1.0.0-beta.5'          # firmware this installer installs
INSTALLER_VERSION = '1.0.0-beta.8'
FIRMWARE_DIR = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'firmware'
IMAGES = {
    'mcu': ('magic-trick-1.0.0-beta.5-mcu.bin', '8d6f9448b019b19c2bf06d7bcc7b0e2eaaca02358265340dc4150bceb9f90a7b'),
    'table_three_systems': ('partition-table-three-systems.bin', '20e934b5a603f40f0851baf5e5037dd36f9f1c4c2f413f6ca47757de986203a6'),
    'table_stock': ('partition-table-stock.bin', '7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820'),
    'fpga_gb': ('magic-trick-gb-gbc.fs', '5210fb75d41977324339b88c7fd5ecd665049a4921d145cd670cf574b66ca7e0'),
    'fpga_gg': ('magic-trick-game-gear.fs', 'f4a01cc17bb88b9e1732cee0f16ce3ce151bc3c1ab8968db6acd6e254d47952b'),
    'fpga_chromagic': ('chromagic-1.0.1-fpga.fs', '79791d915060a9898c9be6adc7ab22f298cbea9619c6bb765b8667db62c337a2'),
}
TABLE_OFFSET = 0x8000
STOCK_APP = (0x10000, 0x100000)
THREE_SYSTEMS_APP = (0x260000, 0x1A0000)
DATA_PARTITIONS = [(1, 2, 0x9000, 0x6000), (1, 1, 0xF000, 0x1000)]   # nvs, phy_init
SLOT_GB, SLOT_GG = 0x000000, 0x100000
# Which bundled core each FPGA slot holds, as written and verified by this installer. Reading
# the FPGA flash back isn't repeatable while a core runs, so the installer keeps its own record
# in an MCU flash sector that no partition uses (0x110000-0x260000 is free in both layouts).
# A core is skipped only if this record, the three-systems layout and the running FPGA version
# all agree; anything else rewrites both cores as before.
CORE_RECORD_OFFSET, CORE_RECORD_SIZE = 0x25F000, 0x1000
CORE_RECORD_MAGIC = b'MTCORES1'
# Progress events for the installer window: emit('plan', steps=[...]) and emit('step', key=...).
EVENT_HOOK = None


def emit(kind, **data):
    if EVENT_HOOK is not None:
        try:
            EVENT_HOOK(kind, data)
        except Exception:
            pass


TARGET = {'mcu': 'v' + RELEASE, 'fpga': '50.1', 'chromatic': 'Magic Trick 1.0 beta (ChroMagic)'}
# A JTAG reset boots the slot the running core points to next, so after installation
# either core may be running; a real power-on always starts the Game Boy core.
INSTALLED = dict(TARGET, fpga=('50.1', '50.2'))

# Applications this installer recognises in a backup. Anything else is refused.
KNOWN_APP = re.compile(r'0\.13\.[0-8](?:-art\d+(?:-wifi\d+)?)?|three-systems-mb\d+|1\.0\.0-beta(?:\.\d+)?')
KNOWN_RUNNING_MCU = re.compile(r'v(?:' + KNOWN_APP.pattern + r')')
KNOWN_RUNNING_FPGA = re.compile(r'18\.\d+|50\.[12]')


def image(key):
    """Return the path of a bundled image after checking its SHA-256."""
    name, digest = IMAGES[key]
    path = FIRMWARE_DIR / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise ValueError(f'Bundled firmware {name} is missing or damaged. Extract the complete download. Nothing was written.')
    return path


def check_images(keys=None):
    return {k: image(k) for k in (keys or IMAGES)}


def is_chromatic(vid, pid):
    """The FPGA's USB descriptor puts the player number (set by the MCU) in the low byte of the
    product ID: 0x0101 for player 1, and 0x0100 until a working MCU has set it, for example while
    the MCU can't boot. Accept any player number."""
    return vid == 0x374e and pid is not None and pid & 0xFF00 == 0x0100


def device_ports():
    from serial.tools.list_ports import comports
    return [p.device for p in comports() if is_chromatic(p.vid, p.pid)]


def version(port, timeout=20, trace=None):
    """Ask the running firmware for its versions. Opening the port can restart the MCU.
    trace, if given, receives raw console text and any errors (used by --status).

    The stock console only takes commands for a few seconds after its first prompt, then
    ignores input until the next reset. Windows often opens the port too late for that,
    so if a plain read gets no answer, restart the MCU with the port open and ask right away."""
    for baud, reset, listen in ((115200, False, timeout), (115200, True, RESET_LISTEN), (2000000, False, timeout)):
        try:
            found = _ask_version(port, baud, reset, listen, trace)
        except DownloadMode:
            raise
        except (OSError, ValueError) as exc:
            if trace: trace(f'[{baud}] open failed: {exc}')
            continue
        if found is not None:
            return found
    raise ValueError('No firmware version response.')


RESET_LISTEN = 10   # seconds to listen after restarting the MCU


class DownloadMode(ValueError):
    """The MCU restarted into its ROM loader instead of the application. Seen on Windows right
    after an FPGA write; only a power cycle brings the application back."""


def _set_rts(p, state):
    p.rts = state
    p.dtr = p.dtr   # Windows usbser.sys only sends the new RTS state along with a DTR update (as esptool does)


def _ask_version(port, baud, reset, listen, trace):
    import serial
    if trace:   # tracing is diagnostics only; it must never end an attempt
        raw_trace = trace
        def trace(text):
            try:
                raw_trace(text)
            except Exception:
                pass
    tag = f'[{baud}{" after restart" if reset else ""}] '
    p = serial.Serial(port=None, baudrate=baud, timeout=.1, write_timeout=2)
    p.dtr = False; p.rts = False; p.port = port   # stock firmware's console is silent with DTR on
    p.open()
    try:
        if reset:   # esptool's hard reset: RTS pulls the MCU's EN low
            if trace: trace(tag + 'restarting the MCU')
            _set_rts(p, True); time.sleep(.1); _set_rts(p, False)
        every = .3 if reset else 1
        data = b''; last = 0; deadline = time.monotonic() + listen
        early = None
        while time.monotonic() < deadline:
            try:
                chunk = p.read(4096)
            except (OSError, ValueError) as exc:
                if trace: trace(tag + f'read failed: {exc}')
                return None
            if chunk and trace:
                trace(tag + chunk.decode('ascii', errors='replace'))
            data = (data + chunk)[-65536:]
            if reset and b'waiting for download' in data:
                # The MCU came up in its ROM loader instead of the app; only a power cycle helps.
                if trace: trace(tag + 'the MCU started in download mode; power-cycle the Chromatic')
                raise DownloadMode('The Chromatic needs to be turned off and on.')
            for match in re.findall(rb'\{[^{}]+\}', data):
                try:
                    obj = json.loads(match)
                    if all(isinstance(obj.get(k), str) for k in ('mcu', 'fpga', 'chromatic')):
                        if obj['fpga'] != '0.0':
                            return obj
                        # Just after the MCU starts (on macOS, opening the port restarts it), the
                        # console can answer before the FPGA has reported its version. Ask again.
                        early, data = obj, b''
                        break
                except (ValueError, UnicodeDecodeError):
                    pass
            if time.monotonic() - last > every:
                last = time.monotonic()
                try:
                    # Ctrl-U clears a half-typed line; works on Magic Trick and stock consoles.
                    p.write(b'\x15\rfwversion\r'); p.flush()
                except (OSError, ValueError) as exc:   # a write timeout must not end the attempt
                    if trace: trace(tag + f'write failed: {exc}')
        return early
    finally:
        try:
            p.close()
        except (OSError, ValueError):
            pass


def known_running(v):
    return (KNOWN_RUNNING_MCU.fullmatch(v.get('mcu', '')) is not None and
            KNOWN_RUNNING_FPGA.fullmatch(v.get('fpga', '')) is not None)


def parse_table(table):
    """Parse an ESP-IDF partition table, requiring its MD5 record."""
    entries = []
    for i in range(0, 0xC00, 32):
        record = table[i:i + 32]
        if record[:2] == b'\xeb\xeb':
            if hashlib.md5(table[:i]).digest() != record[16:32]:
                raise ValueError('Partition table checksum mismatch. Nothing was written.')
            return entries
        if record[:2] == b'\xff\xff':
            raise ValueError('Partition table checksum missing. Nothing was written.')
        magic, typ, subtype, offset, size = struct.unpack('<HBBII', record[:12])
        if magic != 0x50AA:
            raise ValueError('Unrecognized partition table. Nothing was written.')
        entries.append((typ, subtype, offset, size))
    raise ValueError('Invalid partition table. Nothing was written.')


def classify(entries):
    apps = [e[2:] for e in entries if e[0] == 0]
    data = [e for e in entries if e[0] != 0]
    if data != DATA_PARTITIONS:
        raise ValueError('Unsupported data partitions. Nothing was written.')
    if apps == [STOCK_APP]:
        return 'stock'
    if apps == [THREE_SYSTEMS_APP]:
        return 'three-systems'
    raise ValueError('Unsupported application layout. Nothing was written.')


def core_record(slots):
    """One flash sector recording {slot offset: SHA-256 of the bundled .fs written there}."""
    body = json.dumps({f'0x{slot:06x}': digest for slot, digest in sorted(slots.items())}).encode()
    blob = CORE_RECORD_MAGIC + len(body).to_bytes(2, 'little') + body + hashlib.sha256(body).digest()
    return blob + b'\xff' * (CORE_RECORD_SIZE - len(blob))


def parse_core_record(flash):
    """Read the record from a full MCU flash image. Anything unexpected reads as 'nothing known'."""
    sector = flash[CORE_RECORD_OFFSET:CORE_RECORD_OFFSET + CORE_RECORD_SIZE]
    if not sector.startswith(CORE_RECORD_MAGIC):
        return {}
    size = int.from_bytes(sector[8:10], 'little')
    body, digest = sector[10:10 + size], sector[10 + size:10 + size + 32]
    if len(body) != size or 10 + size + 32 > CORE_RECORD_SIZE or hashlib.sha256(body).digest() != digest:
        return {}
    try:
        items = json.loads(body)
    except ValueError:
        return {}
    if not isinstance(items, dict):
        return {}
    return {int(k, 16): v for k, v in items.items()
            if re.fullmatch(r'0x[0-9a-f]{6}', k) and isinstance(v, str) and re.fullmatch(r'[0-9a-f]{64}', v)}


def fpga_plan(layout, running, record):
    """Which FPGA writes an install needs: (write ChroMagic first, write Game Gear, write Game Boy)."""
    multiboot_in_slot0 = layout == 'three-systems' or (running is not None and running.get('fpga', '').startswith('50.'))
    trusted = layout == 'three-systems' and running is not None and running.get('fpga', '').startswith('50.')
    gg_current = trusted and record.get(SLOT_GG) == IMAGES['fpga_gg'][1]
    gb_current = trusted and record.get(SLOT_GB) == IMAGES['fpga_gb'][1]
    write_gg = not gg_current
    # Writing slot 1 needs a non-multiboot core in slot 0 first, which then has to be replaced.
    temporary = write_gg and multiboot_in_slot0
    write_gb = temporary or not gb_current
    return temporary, write_gg, write_gb


def app_info(data, offset):
    """Return (project, version) from the ESP-IDF app descriptor at offset, or None."""
    if data[offset] != 0xE9:
        return None
    desc = data[offset + 0x20:offset + 0x120]
    if len(desc) != 256 or struct.unpack('<I', desc[:4])[0] != 0xABCD5432:
        return None
    ver = desc[16:48].split(b'\0', 1)[0].decode('ascii', errors='replace')
    project = desc[48:80].split(b'\0', 1)[0].decode('ascii', errors='replace')
    return project, ver


def known_app(info):
    return info is not None and info[0] == 'mcu_fw' and KNOWN_APP.fullmatch(info[1]) is not None


def validate_backup(data):
    """Check a full 4 MiB MCU backup. Returns (layout, running app version)."""
    if len(data) != 0x400000:
        raise ValueError('Backup is incomplete. Nothing was written.')
    if data[0x1000] != 0xE9:
        raise ValueError('No valid bootloader in the backup. Nothing was written.')
    layout = classify(parse_table(data[TABLE_OFFSET:TABLE_OFFSET + 0xC00]))
    offset = STOCK_APP[0] if layout == 'stock' else THREE_SYSTEMS_APP[0]
    info = app_info(data, offset)
    if not known_app(info):
        raise ValueError('Unsupported MCU application ' + (info[1] if info else 'image') + '. Nothing was written.')
    return layout, info[1]


# esptool runs in its own process so a stuck serial port can never hang the installer: if it prints
# nothing for this long it is stopped. Its progress output arrives at least every few seconds.
ESPTOOL_IDLE_TIMEOUT = 120
FATAL_EXIT_GRACE = 15
ESPTOOL_STUCK = ('The MCU tool stopped responding. Unplug the USB cable, wait 5 seconds, plug it back in '
                 '(keep the Chromatic switched on), then run the same operation again.')


def esptool_command():
    """The bundled esptool, run as a child process (app.py handles --esptool in the packaged app)."""
    if getattr(sys, 'frozen', False):
        return [sys.executable, '--esptool']
    return [sys.executable, '-m', 'esptool']


def reader_command():
    """Our resilient flash reader (read_flash_session), run as a child process."""
    if getattr(sys, 'frozen', False):
        return [sys.executable, '--read-flash']
    return [sys.executable, str(Path(__file__).with_name('app.py')), '--read-flash']


def run_tool(port, args, idle_timeout=None):
    run_child(esptool_command() + ['--chip', 'esp32', '--port', port, '--baud', '115200'] + list(args), idle_timeout)


def run_child(cmd, idle_timeout=None):
    idle_timeout = ESPTOOL_IDLE_TIMEOUT if idle_timeout is None else idle_timeout
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0) if sys.platform == 'win32' else 0)
    last_output, tail, fatal = [time.monotonic()], [], []

    def pump():
        while True:
            chunk = proc.stdout.read1(4096)
            if not chunk:
                return
            last_output[0] = time.monotonic()
            text = chunk.decode('utf-8', errors='replace')
            tail.append(text); del tail[:-40]
            if 'A fatal error occurred' in text or 'FatalError' in text or 'Traceback' in text:
                fatal.append(text)
            sys.stdout.write(text); sys.stdout.flush()

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    def last_line():
        lines = [l for l in ''.join(tail).replace('\r', '\n').splitlines() if l.strip()]
        fatal_lines = [l for l in lines if 'A fatal error occurred' in l]   # more useful than a trailing help link
        return (fatal_lines or lines)[-1] if lines else f'exit code {proc.returncode}'

    while proc.poll() is None:
        quiet = time.monotonic() - last_output[0]
        if fatal and quiet > FATAL_EXIT_GRACE:
            # Seen on Windows: after an error the tool can hang while closing the serial port.
            proc.kill(); proc.wait(); reader.join(5)
            raise ValueError('The MCU tool failed: ' + last_line())
        if quiet > idle_timeout:
            proc.kill(); proc.wait()
            raise ValueError(ESPTOOL_STUCK)
        time.sleep(0.2)
    reader.join(5)
    if proc.returncode:
        raise ValueError('The MCU tool failed: ' + last_line())


TOOL_ATTEMPTS = 3
READ_CHUNK = 0x10000      # 64 KiB per read; a failed chunk is re-read on a fresh connection
READ_ATTEMPTS = 6


def read_flash(port, offset, size, path, log):
    """Read flash into path through the resilient reader child. Over Windows USB serial the
    Chromatic's link drops the odd byte during long reads, so a single esptool read_flash of
    4 MiB fails at a random point; reading in chunks and retrying a failed one fixes that."""
    path = Path(path)
    if path.exists():
        path.unlink()
    run_child(reader_command() + [port, hex(offset), hex(size), str(path)])
    if not path.is_file() or path.stat().st_size != size:
        raise ValueError('Reading the MCU flash did not complete. Nothing was written.')


def read_flash_session(port, offset, size, out, chunk=READ_CHUNK, attempts=READ_ATTEMPTS, baud=115200):
    """Runs in the child process: connect once, read chunk by chunk, reconnect and re-read a chunk
    that fails. Writes out only when every byte has been read."""
    import serial
    from esptool.cmds import detect_chip
    from esptool.util import FatalError
    esp, data, pos, end = None, bytearray(), offset, offset + size

    def connect():
        e = detect_chip(port, baud, 'default_reset').run_stub()
        e.flash_spi_attach(0)
        e.flash_set_parameters(0x400000)
        return e

    def drop(e):
        try:
            e._port.close()
        except Exception:
            pass

    try:
        while pos < end:
            n = min(chunk, end - pos)
            for attempt in range(1, attempts + 1):
                try:
                    if esp is None:
                        esp = connect()
                    block = esp.read_flash(pos, n)
                    if len(block) != n:
                        raise FatalError(f'short read ({len(block)} of {n} bytes)')
                    break
                except (FatalError, OSError, serial.SerialException, struct.error) as exc:
                    print(f'Read at 0x{pos:06x} failed ({exc}); reconnecting and retrying '
                          f'(attempt {attempt} of {attempts}).', flush=True)
                    if esp is not None:
                        drop(esp)
                    esp = None
                    if attempt == attempts:
                        raise
                    time.sleep(1)
            data += block
            pos += n
            print(f'Read {round(100 * (pos - offset) / size)}% of the MCU flash.', flush=True)
        Path(out).write_bytes(bytes(data))
    finally:
        if esp is not None:
            try:
                esp.hard_reset()
            except Exception:
                pass
            drop(esp)


def write_and_verify(port, offset, path, log=print):
    """Write and verify one image. A failed attempt is retried: rewriting the same data is safe."""
    for attempt in range(1, TOOL_ATTEMPTS + 1):
        try:
            run_tool(port, ['--before', 'default_reset', '--after', 'hard_reset', 'write_flash',
                            '--flash_mode', 'dio', '--flash_freq', '40m', '--flash_size', '4MB', hex(offset), str(path)])
            run_tool(port, ['verify_flash', hex(offset), str(path)])
            return
        except ValueError as exc:
            if attempt == TOOL_ATTEMPTS:
                raise
            log(f'MCU write at 0x{offset:06x} failed ({exc}); retrying.')


def write_record(port, folder, slots, log):
    """Write the core record (see CORE_RECORD_OFFSET) through esptool, which verifies it."""
    path = Path(folder) / 'core-record.bin'
    path.write_bytes(core_record(slots))
    write_and_verify(port, CORE_RECORD_OFFSET, path, log)


def wait_for_device(log, sleep, timeout=45):
    """After an FPGA write the USB serial port (provided by the FPGA) disappears while the
    FPGA reboots, and can come back under a new name. Wait for exactly one Chromatic."""
    for round_ in range(2):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ports = device_ports()
            if len(ports) == 1:
                sleep(3)   # let the MCU finish starting before esptool resets it
                return ports[0]
            sleep(1)
        if round_ == 0:
            log('The Chromatic has not reconnected yet; restarting its FPGA from flash.')
            fpga.reset(log)
    raise ValueError('The Chromatic did not reconnect over USB after the FPGA write. Unplug and reconnect it, '
                     'then run the installer again.')


BACKUP_ROUNDS = 3
BACKUP_SETTLE = 60   # seconds
NOT_PROGRAMMABLE = ('The Chromatic did not enter its programming mode, so the backup could not be read. Nothing was '
                    'written. Turn the Chromatic off and on, stay in its menu without starting a game, wait a minute, '
                    'then run the installer again.')


class Session:
    """Backup folder, checkpoints and the backup itself, shared by install and uninstall."""
    def __init__(self, port, backup_dir, log, operation, sleep=time.sleep):
        self.port, self.log, self.sleep = port, log, sleep
        self.folder = Path(backup_dir)
        self.state = {'operation': operation, 'release': RELEASE, 'installer': INSTALLER_VERSION, 'stage': 'starting'}

    def checkpoint(self, stage, **extra):
        self.state.update(stage=stage, **extra)
        (self.folder / 'installation.json').write_text(json.dumps(self.state, indent=2))

    def backup(self):
        emit('step', key='backup')
        self.folder.mkdir(parents=True, exist_ok=False)
        path = self.folder / 'mcu-backup.bin'
        self.log('Backing up the 4 MiB MCU flash. Keep USB and power connected.')
        for round_ in range(1, BACKUP_ROUNDS + 1):
            try:
                read_flash(self.port, 0, 0x400000, path, self.log)
                break
            except ValueError as exc:
                text = str(exc).lower()
                if 'connect' not in text and 'boot mode' not in text:
                    raise
                if round_ == BACKUP_ROUNDS:
                    raise ValueError(NOT_PROGRAMMABLE) from exc
                # Seen for about a minute after Magic Trick resumed a game on the other core at
                # power-on: every reset left the MCU outside its programming mode. It passes on its own.
                self.log('The MCU did not enter its programming mode (the Chromatic may still be starting a game). '
                         f'Waiting {BACKUP_SETTLE} seconds and trying again. Nothing has been written.')
                self.sleep(BACKUP_SETTLE)
                self.port = wait_for_device(self.log, self.sleep)
        data = path.read_bytes()
        layout, app = validate_backup(data)
        run_tool(self.port, ['verify_flash', '0x0', str(path)])
        (self.folder / 'backup.json').write_text(json.dumps(
            {'sha256': hashlib.sha256(data).hexdigest(), 'layout': layout, 'app': app}, indent=2))
        self.checkpoint('backup_verified', layout=layout, previous_app=app)
        self.log(f'Backup verified ({layout} layout, MCU {app}).')
        return data, layout, app


LINUX_ACCESS_HELP = ('On Linux, allow access to the Chromatic once: see "Linux: USB permissions" in the installer guide, '
                     'then unplug and replug the Chromatic. Nothing was written.')


def check_port_access(port, platform=sys.platform):
    """Linux: a serial port the user can't open would otherwise look like old firmware that doesn't answer."""
    if platform.startswith('linux') and os.path.exists(port) and not os.access(port, os.R_OK | os.W_OK):
        raise ValueError(f'This user can\'t open the Chromatic\'s serial port ({port}). ' + LINUX_ACCESS_HELP)


def preconditions(port, log, keys):
    emit('step', key='check')
    files = check_images(keys)
    if device_ports() != [port]:
        raise ValueError('Connect exactly one Chromatic and select its USB port. Nothing was written.')
    check_port_access(port)
    try:
        running = version(port)
    except ValueError:
        running = None   # older stock firmware has no fwversion command
    if running is not None and not known_running(running):
        raise ValueError('Unsupported firmware: ' + json.dumps(running) + '. Nothing was written.')
    log('Device versions: ' + json.dumps(running))
    fpga.preflight(log)
    return files, running


CONFIRM_WINDOW = 180   # seconds; Windows may install drivers for a new USB layout first


def matches(after, expected):
    return after is not None and all(after.get(k) in (v if isinstance(v, tuple) else (v,)) for k, v in expected.items())


def recheck(folder):
    """After ALMOST DONE: the user power-cycled; ask the Chromatic again. Returns (confirmed, versions).
    On success the run's installation.json is marked complete."""
    path = Path(folder) / 'installation.json'
    state = json.loads(path.read_text())
    expected = state.get('expected')
    ports = device_ports()
    if not expected or len(ports) != 1:
        return False, None
    after = version(ports[0])
    if not matches(after, expected):
        return False, after
    state.update(stage='complete', confirmed_later=after)
    path.write_text(json.dumps(state, indent=2))
    return True, after


def confirm(expected, log, sleep, window=CONFIRM_WINDOW):
    """Restart and wait for the expected versions. The serial port is looked up again each time:
    its name can change (Windows gives the stock firmware's USB layout a new COM number)."""
    emit('step', key='confirm')
    fpga.reset(log)
    sleep(8)
    after, waited, said, asked = None, 8, False, False
    while waited < window:
        try:
            ports = device_ports()
            if len(ports) == 1:
                if asked:
                    log('The Chromatic hasn\'t confirmed its version yet; asking again (up to 3 minutes).')
                asked = True
                waited += 50   # version() listens for up to about 50 s
                after = version(ports[0])
            elif not said:
                log('Waiting for the Chromatic to reconnect. The first time, Windows may set up drivers; '
                    'this can take a minute or two.')
                said = True
        except DownloadMode:
            log('The Chromatic restarted into its download mode, so it can\'t answer until it is turned off and on.')
            break
        except (OSError, ValueError):
            pass
        if matches(after, expected):
            return after
        sleep(3)
        waited += 3
    raise ValueError('Flash verified, but the running version was not confirmed: ' + json.dumps(after) +
                     '. Power-cycle and check Status.')


def install(port, backup_dir, log, sleep=time.sleep):
    """Install or upgrade Magic Trick 1.0 beta (MCU, both FPGA cores, partition table)."""
    files, running = preconditions(port, log, ['mcu', 'table_three_systems', 'fpga_gb', 'fpga_gg', 'fpga_chromagic'])
    s = Session(port, backup_dir, log, 'install', sleep)
    data, layout, app = s.backup()
    port = s.port   # can change if the backup had to restart the FPGA
    # A multiboot core in slot 0 points the FPGA's configuration engine at slot 1. Once
    # slot 1 is partly erased the engine keeps reading it and blocks further flash access,
    # so slot 1 must only be written while a non-multiboot image is in slot 0 (fpga_plan).
    record = parse_core_record(data) if layout == 'three-systems' else {}
    temporary, write_gg, write_gb = fpga_plan(layout, running, record)
    s.checkpoint('fpga_planned', core_record=record, write_chromagic=temporary, write_gg=write_gg, write_gb=write_gb)
    emit('plan', steps=['check', 'backup', 'mcu'] + (['fpga_chromagic'] if temporary else []) +
         (['fpga_gg'] if write_gg else []) + (['fpga_gb'] if write_gb else []) +
         (['table'] if layout == 'stock' else []) + ['confirm'])

    # 1. MCU application at 0x260000. On the stock layout this region is unused and
    #    the old table still boots the old app, so nothing changes yet.
    emit('step', key='mcu')
    s.checkpoint('mcu_write_started')
    log('Writing the Magic Trick MCU application.')
    write_and_verify(port, THREE_SYSTEMS_APP[0], files['mcu'], log)
    s.checkpoint('mcu_verified')

    # 2. FPGA: Game Gear slot first (never booted at power-on), Game Boy slot 0 last.
    steps = ([('fpga_chromagic', SLOT_GB, 'temporary ChroMagic')] if temporary else []) + \
            ([('fpga_gg', SLOT_GG, 'Game Gear')] if write_gg else []) + \
            ([('fpga_gb', SLOT_GB, 'Game Boy / Game Boy Color')] if write_gb else [])
    for slot, label, writing in ((SLOT_GG, 'Game Gear', write_gg), (SLOT_GB, 'Game Boy / Game Boy Color', write_gb)):
        if not writing:
            log(f'The {label} FPGA core on your Chromatic is already this version. Skipping it.')
    keep = {slot: digest for slot, digest in record.items()
            if not (slot == SLOT_GG and write_gg) and not (slot == SLOT_GB and (write_gb or temporary))}
    if steps and keep != record:
        # Forget the slots about to be written first, so an interrupted write is never skipped later.
        s.checkpoint('core_record_cleared')
        write_record(port, s.folder, keep, log)
    try:
        for key, offset, label in steps:
            emit('step', key=key)
            s.checkpoint(f'fpga_{key}_started')
            log(f'Writing the {label} FPGA core. This can take several attempts; keep USB and power connected.')
            attempts = fpga.flash(files[key], offset, log, sleep=sleep)
            s.checkpoint(f'fpga_{key}_verified', **{f'{key}_attempts': attempts})
    except Exception as exc:
        s.checkpoint('partial_install_fpga_failed')
        raise ValueError('The FPGA write did not finish. The device can still be reached over USB: run the installer again. ' + str(exc)) from exc

    if steps:
        port = wait_for_device(log, sleep)
        write_record(port, s.folder, {SLOT_GB: IMAGES['fpga_gb'][1], SLOT_GG: IMAGES['fpga_gg'][1]}, log)
        s.checkpoint('core_record_written')
    # 3. Partition table last: this switches the MCU to the new application.
    if layout == 'stock':
        emit('step', key='table')
        s.checkpoint('table_write_started')
        log('Switching the MCU to the new partition layout.')
        write_and_verify(port, TABLE_OFFSET, files['table_three_systems'], log)
    s.checkpoint('all_images_verified')
    log('All images verified. Restarting and checking versions…')
    confirm(INSTALLED, log, sleep)
    s.checkpoint('complete')
    log('Magic Trick ' + RELEASE + ' is installed. Turn the Chromatic off and on once. Backup: ' + str(s.folder))


def uninstall(port, backup_dir, log, sleep=time.sleep):
    """Restore the ChroMagic 1.0.1 FPGA and the application kept at 0x10000."""
    files, running = preconditions(port, log, ['fpga_chromagic', 'table_stock'])
    s = Session(port, backup_dir, log, 'uninstall', sleep)
    data, layout, _ = s.backup()
    port = s.port   # can change if the backup had to restart the FPGA
    if layout != 'three-systems':
        s.checkpoint('nothing_to_uninstall')
        raise ValueError('Magic Trick 1.0 is not installed (standard layout found). Nothing was written.')
    previous = app_info(data, STOCK_APP[0])
    if not known_app(previous) or previous[1].startswith(('three-systems', '1.0.0')):
        s.checkpoint('no_previous_app')
        raise ValueError('No earlier application was found at 0x10000 to restore. Nothing was written. '
                         'Open a GitHub issue with installation.json.')
    emit('plan', steps=['check', 'backup', 'fpga_chromagic', 'table', 'confirm'])
    emit('step', key='fpga_chromagic')
    s.checkpoint('fpga_started', restoring_app=previous[1])
    log('Writing the ChroMagic 1.0.1 FPGA to slot 0. This can take several attempts.')
    try:
        attempts = fpga.flash(files['fpga_chromagic'], SLOT_GB, log, sleep=sleep)
    except Exception as exc:
        s.checkpoint('partial_uninstall_fpga_failed')
        raise ValueError('The FPGA write did not finish. Run the uninstaller again. ' + str(exc)) from exc
    s.checkpoint('fpga_verified', fpga_attempts=attempts)
    port = wait_for_device(log, sleep)
    emit('step', key='table')
    log('Restoring the standard partition layout (application ' + previous[1] + ').')
    write_and_verify(port, TABLE_OFFSET, files['table_stock'], log)
    s.checkpoint('table_restored')
    try:
        expected = {'fpga': '18.38', 'mcu': 'v' + previous[1]}
        confirm(expected, log, sleep)
    except ValueError:
        # Everything was written and verified; only the read-back failed (seen on Windows while it
        # set up drivers for the stock firmware's USB layout). Not a reason to alarm the user.
        s.checkpoint('complete_unconfirmed', expected=expected)
        log('Uninstall written and verified. The running version could not be read back. Turn the Chromatic '
            'off and on and check that it starts. To return fully to stock, now use ModRetro\'s official updater.')
        return
    s.checkpoint('complete')
    log('Uninstall complete: ChroMagic FPGA 18.38 with MCU ' + previous[1] + '. '
        'To return fully to stock, now use ModRetro\'s official updater.')


CLOUD_FOLDERS = {'onedrive': 'OneDrive', 'dropbox': 'Dropbox', 'icloud': 'iCloud Drive',
                 'mobile documents': 'iCloud Drive', 'google drive': 'Google Drive', 'googledrive': 'Google Drive',
                 'cloudstorage': 'a cloud service', 'box sync': 'Box', 'pcloud': 'pCloud', 'mega': 'MEGA'}


def cloud_service(path):
    """Name of the cloud service a folder probably syncs to, or None."""
    for part in Path(path).expanduser().parts:
        name = part.lower()
        for key, service in CLOUD_FOLDERS.items():
            if name == key or name.startswith(key + ' ') or name.startswith(key + '-') or (key in ('onedrive', 'dropbox', 'icloud', 'cloudstorage', 'mobile documents') and key in name):
                return service
    return None


def check_restore_backup(path):
    """Validate a backup chosen for Restore. Only a standard-layout backup (the one made before
    Magic Trick 1.0 was first installed) can be restored. Returns (data, app version)."""
    path = Path(path)
    data = path.read_bytes()
    layout, app = validate_backup(data)
    if layout != 'stock':
        raise ValueError('This backup was made while Magic Trick 1.0 was installed. Choose the mcu-backup.bin '
                         'from your first install (its backup.json says "layout": "stock"). Nothing was written.')
    meta = path.with_name('backup.json')
    if meta.is_file():
        try:
            expected = json.loads(meta.read_text()).get('sha256')
        except ValueError:
            expected = None
        if expected and expected != hashlib.sha256(data).hexdigest():
            raise ValueError('mcu-backup.bin does not match its backup.json; the file was changed or damaged. '
                             'Nothing was written.')
    return data, app



def restore(backup_file, backup_dir, log, sleep=time.sleep):
    """Return a device to the state saved before Magic Trick 1.0 was installed, whatever state it
    is in now: ChroMagic 1.0.1 FPGA in slot 0, then the saved MCU flash from 0x8000 on.

    Unlike install and uninstall this does not require the device's current flash to be valid,
    so it also recovers an interrupted partition-table write. The USB serial port comes from the
    FPGA, so the FPGA is written first (over JTAG, which works whatever the FPGA flash holds)."""
    emit('step', key='check')
    data, app = check_restore_backup(backup_file)
    files = check_images(['fpga_chromagic'])
    fpga.preflight(log)
    folder = Path(backup_dir)
    folder.mkdir(parents=True, exist_ok=False)
    state = {'operation': 'restore', 'release': RELEASE, 'installer': INSTALLER_VERSION, 'backup_file': str(backup_file), 'restoring_app': app}

    def checkpoint(stage, **extra):
        state.update(stage=stage, **extra)
        (folder / 'installation.json').write_text(json.dumps(state, indent=2))

    checkpoint('starting')
    ports = device_ports()
    if len(ports) > 1:
        raise ValueError('Connect exactly one Chromatic. Nothing was written.')
    emit('plan', steps=['check'] + (['precopy'] if ports else []) + ['fpga_chromagic', 'mcu_restore', 'confirm'])
    if ports:
        emit('step', key='precopy')
        # Keep a raw copy of the flash as it is now, valid or not, before overwriting it.
        log('Saving the current 4 MiB MCU flash (it may be damaged; it is kept only as a precaution).')
        try:
            read_flash(ports[0], 0, 0x400000, folder / 'pre-restore-flash.bin', log)
            checkpoint('pre_restore_saved')
        except (Exception, SystemExit) as exc:
            checkpoint('pre_restore_unreadable', error=str(exc))
            log('The current flash could not be read; continuing with the restore.')
    else:
        # No serial port means the FPGA isn't running a working core; JTAG still works.
        checkpoint('no_serial_port')
        log('No USB serial port yet (the FPGA is not running). Repairing the FPGA first.')

    emit('step', key='fpga_chromagic')
    checkpoint('fpga_started')
    log('Writing the ChroMagic 1.0.1 FPGA to slot 0. This can take several attempts.')
    try:
        attempts = fpga.flash(files['fpga_chromagic'], SLOT_GB, log, sleep=sleep)
    except Exception as exc:
        checkpoint('fpga_failed')
        raise ValueError('The FPGA write did not finish. Run Restore again. ' + str(exc)) from exc
    checkpoint('fpga_verified', fpga_attempts=attempts)
    port = wait_for_device(log, sleep)

    # The bootloader (below 0x8000) is never changed by this installer, so leave it alone.
    image_path = folder / 'restore-0x8000.bin'
    image_path.write_bytes(data[TABLE_OFFSET:])
    emit('step', key='mcu_restore')
    checkpoint('mcu_write_started')
    log('Writing the saved MCU flash (partition table, settings and application ' + app + '). '
        'This takes a few minutes; keep USB and power connected.')
    write_and_verify(port, TABLE_OFFSET, image_path, log)
    image_path.unlink()
    checkpoint('mcu_verified')
    try:
        expected = {'fpga': '18.38', 'mcu': 'v' + app}
        confirm(expected, log, sleep)
    except ValueError:
        # Some stock firmware has no version command, so a missing answer is not a failure here.
        checkpoint('complete_unconfirmed', expected=expected)
        log('Restore written and verified. The running version could not be read back (older stock firmware '
            'does not report it). Turn the Chromatic off and on and check that it starts.')
        return
    checkpoint('complete')
    log('Restore complete: ChroMagic FPGA 18.38 with MCU ' + app + '. '
        'To return fully to stock, now use ModRetro\'s official updater.')
