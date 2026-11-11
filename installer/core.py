"""Magic Trick installer: three-system firmware install, upgrade and uninstall with backup gates."""
import hashlib, json, re, struct, subprocess, sys, threading, time
from pathlib import Path
import fpga

RELEASE = '1.0.0-beta'            # firmware this installer installs
INSTALLER_VERSION = '1.0.0-beta.3'
FIRMWARE_DIR = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'firmware'
IMAGES = {
    'mcu': ('magic-trick-1.0.0-beta-mcu.bin', 'cfbb6af4e16f5574fd6d159691e35c2cf2d6a111670e526078a286ec0cfdd23f'),
    'table_three_systems': ('partition-table-three-systems.bin', '20e934b5a603f40f0851baf5e5037dd36f9f1c4c2f413f6ca47757de986203a6'),
    'table_stock': ('partition-table-stock.bin', '7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820'),
    'fpga_gb': ('magic-trick-gb-gbc.fs', '9ac2459b9fd62d9f7be8e6f0a4b514db8a582334ff7418db657abea14f10f795'),
    'fpga_gg': ('magic-trick-game-gear.fs', 'cafa5ff80e6f1e03c5f296029f808e8bc769dd73367fca51955f0245ffea3f0c'),
    'fpga_chromagic': ('chromagic-1.0.1-fpga.fs', '79791d915060a9898c9be6adc7ab22f298cbea9619c6bb765b8667db62c337a2'),
}
TABLE_OFFSET = 0x8000
STOCK_APP = (0x10000, 0x100000)
THREE_SYSTEMS_APP = (0x260000, 0x1A0000)
DATA_PARTITIONS = [(1, 2, 0x9000, 0x6000), (1, 1, 0xF000, 0x1000)]   # nvs, phy_init
SLOT_GB, SLOT_GG = 0x000000, 0x100000
# Progress events for the installer window: emit('plan', steps=[...]) and emit('step', key=...).
EVENT_HOOK = None


def emit(kind, **data):
    if EVENT_HOOK is not None:
        try:
            EVENT_HOOK(kind, data)
        except Exception:
            pass


TARGET = {'mcu': 'v' + RELEASE, 'fpga': '50.1', 'chromatic': 'Magic Trick 1.0 beta'}
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
    trace, if given, receives raw console text and any errors (used by --status)."""
    import serial
    for baud in (115200, 2000000):
        try:
            p = serial.Serial(port=None, baudrate=baud, timeout=.1, write_timeout=2)
            p.dtr = False; p.rts = False; p.port = port   # stock firmware's console is silent with DTR on
            p.open()
        except (OSError, ValueError) as exc:
            if trace: trace(f'[{baud}] open failed: {exc}')
            continue
        data = b''; last = 0; deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                try:
                    chunk = p.read(4096)
                except (OSError, ValueError) as exc:
                    if trace: trace(f'[{baud}] read failed: {exc}')
                    break
                if chunk and trace:
                    trace(f'[{baud}] ' + chunk.decode('ascii', errors='replace'))
                data = (data + chunk)[-65536:]
                for match in re.findall(rb'\{[^{}]+\}', data):
                    try:
                        obj = json.loads(match)
                        if all(isinstance(obj.get(k), str) for k in ('mcu', 'fpga', 'chromatic')):
                            return obj
                    except (ValueError, UnicodeDecodeError):
                        pass
                if time.monotonic() - last > 1:
                    last = time.monotonic()
                    try:
                        # Ctrl-U clears a half-typed line; works on Magic Trick and stock consoles.
                        p.write(b'\x15\rfwversion\r'); p.flush()
                    except (OSError, ValueError) as exc:   # a write timeout must not end the attempt
                        if trace: trace(f'[{baud}] write failed: {exc}')
        finally:
            try:
                p.close()
            except (OSError, ValueError):
                pass
    raise ValueError('No firmware version response.')


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
        return lines[-1] if lines else f'exit code {proc.returncode}'

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


class Session:
    """Backup folder, checkpoints and the backup itself, shared by install and uninstall."""
    def __init__(self, port, backup_dir, log, operation):
        self.port, self.log = port, log
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
        read_flash(self.port, 0, 0x400000, path, self.log)
        data = path.read_bytes()
        layout, app = validate_backup(data)
        run_tool(self.port, ['verify_flash', '0x0', str(path)])
        (self.folder / 'backup.json').write_text(json.dumps(
            {'sha256': hashlib.sha256(data).hexdigest(), 'layout': layout, 'app': app}, indent=2))
        self.checkpoint('backup_verified', layout=layout, previous_app=app)
        self.log(f'Backup verified ({layout} layout, MCU {app}).')
        return data, layout, app


def preconditions(port, log, keys):
    emit('step', key='check')
    files = check_images(keys)
    if device_ports() != [port]:
        raise ValueError('Connect exactly one Chromatic and select its USB port. Nothing was written.')
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
                waited += 20   # version() listens for up to about 20 s
                after = version(ports[0])
            elif not said:
                log('Waiting for the Chromatic to reconnect. The first time, Windows may set up drivers; '
                    'this can take a minute or two.')
                said = True
        except (OSError, ValueError):
            pass
        if after is not None and all(after.get(k) in (v if isinstance(v, tuple) else (v,)) for k, v in expected.items()):
            return after
        sleep(3)
        waited += 3
    raise ValueError('Flash verified, but the running version was not confirmed: ' + json.dumps(after) +
                     '. Power-cycle and check Status.')


def install(port, backup_dir, log, sleep=time.sleep):
    """Install or upgrade Magic Trick 1.0 beta (MCU, both FPGA cores, partition table)."""
    files, running = preconditions(port, log, ['mcu', 'table_three_systems', 'fpga_gb', 'fpga_gg', 'fpga_chromagic'])
    s = Session(port, backup_dir, log, 'install')
    _, layout, app = s.backup()
    # A multiboot core in slot 0 points the FPGA's configuration engine at slot 1. Once
    # slot 1 is partly erased the engine keeps reading it and blocks further flash access,
    # so slot 1 must only be written while a non-multiboot image is in slot 0.
    multiboot_in_slot0 = layout == 'three-systems' or (running is not None and running.get('fpga', '').startswith('50.'))
    emit('plan', steps=['check', 'backup', 'mcu'] + (['fpga_chromagic'] if multiboot_in_slot0 else []) +
         ['fpga_gg', 'fpga_gb'] + (['table'] if layout == 'stock' else []) + ['confirm'])

    # 1. MCU application at 0x260000. On the stock layout this region is unused and
    #    the old table still boots the old app, so nothing changes yet.
    emit('step', key='mcu')
    s.checkpoint('mcu_write_started')
    log('Writing the Magic Trick MCU application.')
    write_and_verify(port, THREE_SYSTEMS_APP[0], files['mcu'], log)
    s.checkpoint('mcu_verified')

    # 2. FPGA: Game Gear slot first (never booted at power-on), Game Boy slot 0 last.
    steps = [('fpga_gg', SLOT_GG, 'Game Gear'), ('fpga_gb', SLOT_GB, 'Game Boy / Game Boy Color')]
    if multiboot_in_slot0:
        steps.insert(0, ('fpga_chromagic', SLOT_GB, 'temporary ChroMagic'))
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

    port = wait_for_device(log, sleep)
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
    s = Session(port, backup_dir, log, 'uninstall')
    data, layout, _ = s.backup()
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
        confirm({'fpga': '18.38', 'mcu': 'v' + previous[1]}, log, sleep)
    except ValueError:
        # Everything was written and verified; only the read-back failed (seen on Windows while it
        # set up drivers for the stock firmware's USB layout). Not a reason to alarm the user.
        s.checkpoint('complete_unconfirmed')
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
        confirm({'fpga': '18.38', 'mcu': 'v' + app}, log, sleep)
    except ValueError:
        # Some stock firmware has no version command, so a missing answer is not a failure here.
        checkpoint('complete_unconfirmed')
        log('Restore written and verified. The running version could not be read back (older stock firmware '
            'does not report it). Turn the Chromatic off and on and check that it starts.')
        return
    checkpoint('complete')
    log('Restore complete: ChroMagic FPGA 18.38 with MCU ' + app + '. '
        'To return fully to stock, now use ModRetro\'s official updater.')
