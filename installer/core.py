"""Magic Trick installer: narrowly scoped, fail-closed MCU upgrade."""
import hashlib, json, re, struct, time
from pathlib import Path

HASH = '83a8cbc4137b1a4c0cd4457194a4e862a4c04d87db4a03af0b58c01a709e5144'
URL = 'https://github.com/agentichunter-blip/chromagic-preview-mod/releases/download/v1.0.1-art31/mcu.bin'
SIZE = 921360

def validate_image(data):
    if len(data) != SIZE or hashlib.sha256(data).hexdigest() != HASH:
        raise ValueError('Firmware verification failed. Nothing was written.')

def compatible(v):
    return (v.get('fpga') == '18.38' and
            re.fullmatch(r'v0\.13\.4(?:-art\d+)?', v.get('mcu', '')) is not None and
            re.fullmatch(r'1\.0\.1(?:-art\d+)?(?: \(4\.2\))?', v.get('chromatic', '')) is not None)

def version(port):
    import serial
    p = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
    p.dtr = False; p.rts = False; p.port = port
    p.open()
    data = b''; last = 0; deadline = time.monotonic() + 15
    try:
        while time.monotonic() < deadline:
            data = (data + p.read(4096))[-65536:]
            for match in re.findall(rb'\{[^{}]+\}', data):
                try:
                    obj = json.loads(match)
                    if all(k in obj for k in ('mcu', 'fpga', 'chromatic')):
                        return obj
                except (ValueError, UnicodeDecodeError):
                    pass
            if time.monotonic() - last > 1:
                p.write(b'\x15\rfwversion\r'); p.flush(); last = time.monotonic()
    finally:
        p.close()
    raise ValueError('No firmware version response. Power on the device, close other USB apps, and retry. Older firmware without fwversion is not supported by this installer.')

def validate_backup(data):
    if len(data) != 0x400000:
        raise ValueError('Backup is incomplete. Nothing was written.')
    table = data[0x8000:0x9000]
    entries = []
    for i in range(0, 0xC00, 32):
        record = table[i:i+32]
        if record[:2] == b'\xeb\xeb':
            if hashlib.md5(table[:i]).digest() != record[16:32]:
                raise ValueError('Partition table checksum mismatch.')
            break
        if record[:2] == b'\xff\xff':
            raise ValueError('Partition table checksum missing.')
        magic, typ, subtype, offset, size = struct.unpack('<HBBII', record[:12])
        if magic != 0x50AA:
            raise ValueError('Unrecognized partition table.')
        entries.append((typ, subtype, offset, size))
    else:
        raise ValueError('Invalid partition table.')
    apps = [e for e in entries if e[0] == 0]
    if apps != [(0, 0, 0x10000, 0x100000)]:
        raise ValueError('Unsupported application layout. Nothing was written.')
    for typ, subtype, offset, size in entries:
        if typ != 0 and offset < 0x110000 and offset + size > 0x10000:
            raise ValueError('Overlapping partition. Nothing was written.')
    if data[0x10000] != 0xE9:
        raise ValueError('No valid existing application.')

def run_tool(port, args):
    import esptool
    esptool.main(['--chip','esp32','--port',port,'--baud','115200'] + args)

def install(port, firmware, backup_dir, log):
    data = Path(firmware).read_bytes(); validate_image(data)
    v = version(port)
    if not compatible(v):
        raise ValueError('Unsupported firmware: ' + json.dumps(v))
    log('Compatible device: ' + json.dumps(v))
    folder = Path(backup_dir)
    folder.mkdir(parents=True, exist_ok=False)
    backup = folder / 'mcu-backup.bin'
    log('Backing up 4 MiB MCU flash. Keep USB and power connected.')
    run_tool(port, ['read_flash','0x0','0x400000',str(backup)])
    original = backup.read_bytes(); validate_backup(original)
    run_tool(port, ['verify_flash','0x0',str(backup)])
    (folder / 'backup.json').write_text(json.dumps({'versions':v,'sha256':hashlib.sha256(original).hexdigest()},indent=2))
    log('Backup verified. Installing the MCU application only.')
    run_tool(port, ['--before','default_reset','--after','hard_reset','write_flash','--flash_mode','dio','--flash_freq','40m','--flash_size','4MB','0x10000',str(firmware)])
    run_tool(port, ['verify_flash','0x10000',str(firmware)])
    log('Flash verified. Checking running firmware…')
    time.sleep(3)
    after = version(port)
    if after != {'mcu':'v0.13.4-art31','fpga':'18.38','chromatic':'1.0.1-art31 (4.2)'}:
        raise ValueError('Flash verified, but running version was not confirmed. Power-cycle and check Status. Backup: ' + str(folder))
    log('Installation complete. Firmware and FPGA versions verified.')
