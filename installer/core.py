"""Magic Trick installer: paired MCU/FPGA installation with backup gates."""
import hashlib, json, re, struct, time
import fpga
from pathlib import Path

HASH = '83a8cbc4137b1a4c0cd4457194a4e862a4c04d87db4a03af0b58c01a709e5144'
URL = 'https://github.com/agentichunter-blip/magic-trick/releases/download/v1.0.1-art31/mcu.bin'
SIZE = 921360

def validate_image(data):
    if len(data) != SIZE or hashlib.sha256(data).hexdigest() != HASH:
        raise ValueError('Firmware verification failed. Nothing was written.')

def compatible(v):
    return (v.get('fpga') == '18.38' and
            re.fullmatch(r'v0\.13\.4(?:-art\d+)?', v.get('mcu', '')) is not None and
            re.fullmatch(r'1\.0\.1(?:-art\d+)?(?: \(4\.2\))?', v.get('chromatic', '')) is not None)

def supported_source(v):
    stock = {'v4.2':'v0.13.4', 'v4.5':'v0.13.7', 'v4.6':'v0.13.8'}
    return compatible(v) or (stock.get(v.get('chromatic')) == v.get('mcu')
        and v.get('chromatic') in stock and re.fullmatch(r'18\.\d+',v.get('fpga','')) is not None)

def device_ports():
    from serial.tools.list_ports import comports
    return [p.device for p in comports() if (p.vid,p.pid)==(0x374e,0x0101)]

def version(port):
    import serial
    for baud in (115200,2000000):
        p = serial.Serial(port=None, baudrate=baud, timeout=.1, write_timeout=2)
        p.dtr = False; p.rts = False; p.port = port
        p.open()
        data = b''; last = 0; deadline = time.monotonic() + 5
        try:
            while time.monotonic() < deadline:
                data = (data + p.read(4096))[-65536:]
                for match in re.findall(rb'\{[^{}]+\}', data):
                    try:
                        obj = json.loads(match)
                        if all(isinstance(obj.get(k),str) for k in ('mcu', 'fpga', 'chromatic')):
                            return obj
                    except (ValueError, UnicodeDecodeError): pass
                if time.monotonic() - last > 1:
                    p.write(b'\x15\rfwversion\r'); p.flush(); last = time.monotonic()
        finally: p.close()
    raise ValueError('No firmware version response.')

def identify_backup(data):
    # ESP-IDF app descriptor: image header + first segment header, then esp_app_desc_t.
    desc=data[0x10020:0x10120]
    if len(desc)!=256 or struct.unpack('<I',desc[:4])[0]!=0xabcd5432:
        raise ValueError('Unrecognized application descriptor. Nothing was written.')
    app=desc[48:80].split(b'\0',1)[0]
    ver=desc[16:48].split(b'\0',1)[0].decode('ascii',errors='replace')
    if app!=b'mcu_fw' or re.fullmatch(r'0\.13\.[0-8](?:-art\d+)?',ver) is None:
        raise ValueError('Unsupported MCU application '+ver+'. Nothing was written.')
    return ver

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

def install(port, firmware, backup_dir, log, fpga_image=None):
    data = Path(firmware).read_bytes(); validate_image(data)
    if device_ports()!=[port]:
        raise ValueError('Connect exactly one Chromatic and select its USB port. Nothing was written.')
    try: v = version(port)
    except ValueError: v = None
    if v is not None and not supported_source(v):
        # An interrupted paired install can have the new MCU and the old FPGA.
        if not (v.get('mcu')=='v0.13.4-art31' and v.get('chromatic')=='1.0.1-art31 (4.2)'):
            raise ValueError('Unsupported firmware: ' + json.dumps(v))
    if fpga_image is None or hashlib.sha256(Path(fpga_image).read_bytes()).hexdigest()!=fpga.FPGA_HASH:
        raise ValueError('FPGA image missing or checksum mismatch. Nothing was written.')
    fpga.preflight(log)
    log('Device versions: ' + json.dumps(v))
    folder = Path(backup_dir)
    folder.mkdir(parents=True, exist_ok=False)
    backup = folder / 'mcu-backup.bin'
    log('Backing up 4 MiB MCU flash. Keep USB and power connected.')
    run_tool(port, ['read_flash','0x0','0x400000',str(backup)])
    original = backup.read_bytes(); validate_backup(original)
    previous_mcu=identify_backup(original)
    if v is None:
        log('Older console: identified MCU '+previous_mcu+' from verified backup layout.')
    run_tool(port, ['verify_flash','0x0',str(backup)])
    (folder / 'backup.json').write_text(json.dumps({'versions':v,'sha256':hashlib.sha256(original).hexdigest()},indent=2))
    state={'stage':'backup_verified','previous_versions':v,'target_fpga':'18.38'}
    def checkpoint(stage):
        state['stage']=stage
        (folder/'installation.json').write_text(json.dumps(state,indent=2))
    checkpoint('mcu_write_started')
    log('Backup verified. Installing Magic Trick MCU application.')
    run_tool(port, ['--before','default_reset','--after','hard_reset','write_flash','--flash_mode','dio','--flash_freq','40m','--flash_size','4MB','0x10000',str(firmware)])
    run_tool(port, ['verify_flash','0x10000',str(firmware)])
    checkpoint('mcu_verified_fpga_pending')
    log('MCU verified. Installing and verifying FPGA firmware. Do not disconnect.')
    try:
        fpga.flash(fpga_image,log)
    except Exception as exc:
        checkpoint('partial_install_fpga_failed')
        raise ValueError('MCU was installed, but FPGA installation did not finish. Keep the backup and retry the paired installation after checking USB drivers. '+str(exc)) from exc
    checkpoint('both_images_verified')
    log('Both images verified. Checking running firmware…')
    time.sleep(3)
    after = None
    for attempt in range(3):
        try:
            ports=device_ports()
            if len(ports)==1: after=version(ports[0])
        except (OSError, ValueError): pass
        if after == {'mcu':'v0.13.4-art31','fpga':'18.38','chromatic':'1.0.1-art31 (4.2)'}: break
        time.sleep(2)
    if after != {'mcu':'v0.13.4-art31','fpga':'18.38','chromatic':'1.0.1-art31 (4.2)'}:
        raise ValueError('Flash verified, but running version was not confirmed. Power-cycle and check Status. Backup: ' + str(folder))
    checkpoint('complete')
    log('Installation complete. Firmware and FPGA versions verified.')
