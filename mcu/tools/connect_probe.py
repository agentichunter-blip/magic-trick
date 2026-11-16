#!/usr/bin/env python3
"""Talk to a Chromatic the way the web ChroMagician does (connect.js of
ChroMagic 2), to test Magic Trick's side of the "connect" protocol.

  connect_probe.py PORT status|info|wifi
  connect_probe.py PORT ls PATH
  connect_probe.py PORT get SDPATH LOCAL
  connect_probe.py PORT put LOCAL SDPATH
  connect_probe.py PORT mkdir|rm SDPATH
  connect_probe.py PORT mv FROM TO
  connect_probe.py PORT backup rom|sav OUTSTEM
  connect_probe.py PORT restore SAVFILE [RTCFILE]
  connect_probe.py PORT sdbackup rom|sav
  connect_probe.py PORT console "LINE"
"""
import re
import sys
import time
import zlib

import serial

CONNECT_BAUD, CONSOLE_BAUD = 2_000_000, 115_200
TERMINATOR = re.compile(r'^> (ok|err)')


class Fail(Exception):
    pass


def hexenc(text):
    return (text.encode() if isinstance(text, str) else text).hex()


def parse_record(line):
    at = line.find('CONNECT ')
    if at < 0:
        return None
    words = line[at + 8:].split()
    if not words or not re.fullmatch(r'[A-Z]+', words[0]):
        return None
    return words[0], dict(w.split('=', 1) for w in words[1:] if '=' in w[1:])


class Session:
    def __init__(self, port, baud):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, baud, 0.05
        self.s.dtr = self.s.rts = False   # do not reset the MCU on open
        self.s.open()
        self.buf = b''
        self.log = []

    def close(self):
        self.s.close()

    def _fill(self, deadline):
        while time.monotonic() < deadline:
            # Read what has arrived (like the browser), not a full buffer:
            # read(n) would wait out the timeout for n bytes.
            data = self.s.read(max(1, self.s.in_waiting))
            if data:
                self.buf += data
                return True
        return False

    def line(self, timeout):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.buf:
            if not self._fill(deadline):
                return None
        raw, self.buf = self.buf.split(b'\n', 1)
        text = raw.decode(errors='replace').rstrip('\r')
        self.log.append(text)
        return text

    def bytes(self, n, timeout):
        deadline = time.monotonic() + timeout
        while len(self.buf) < n:
            if not self._fill(deadline):
                raise Fail(f'timed out receiving data ({len(self.buf)}/{n})')
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def write(self, data):
        self.s.write(data.encode() if isinstance(data, str) else data)
        self.s.flush()

    def identify(self, timeout=6.0):
        end, nxt, ask = time.monotonic() + timeout, 0, 0
        while time.monotonic() < end:
            if time.monotonic() >= nxt:
                self.write('\n'); time.sleep(0.04)
                self.write('fwversion\r\n' if ask % 2 else 'connect status\n')
                ask += 1; nxt = time.monotonic() + 0.25
            text = self.line(0.25)
            if text is None:
                continue
            rec = parse_record(text)
            if rec and rec[0] == 'STATUS':
                self.settle()
                return 'status', rec[1]
            if 'console at 2000000' in text:
                return 'fast', None
            m = re.search(r'\{[^{}]*"mcu"[^{}]*\}', text)
            if m:
                self.settle()
                return 'stock', m.group(0)
        return None, None

    def settle(self):
        last, have = time.monotonic(), len(self.buf)
        while time.monotonic() - last < 0.12:
            self._fill(time.monotonic() + 0.12)
            if len(self.buf) != have:
                have, last = len(self.buf), time.monotonic()
        self.buf = b''

    def command(self, line, on_record=lambda t, f, d: None, idle=15.0):
        self.write(line + '\n')
        while True:
            text = self.line(idle)
            if text is None:
                raise Fail(f'no answer to {line.split()[:2]}')
            if text == '> console ready':
                raise Fail('the MCU restarted')
            if TERMINATOR.match(text):
                raise Fail(f'terminator before PASS/FAIL: {text}')
            rec = parse_record(text)
            if not rec:
                continue
            kind, f = rec
            if kind == 'PASS':
                self.finish(); return
            if kind == 'FAIL':
                self.finish()
                detail = bytes.fromhex(f.get('detail', '')).decode(errors='replace')
                raise Fail(f"{f.get('error')} {detail}".strip())
            data = None
            if kind == 'DATA':
                data = self.bytes(int(f['size']), idle)
                if zlib.crc32(data) != int(f['crc'], 16):
                    raise Fail(f"block {f.get('seq')} failed its CRC")
            on_record(kind, f, data)

    def finish(self, timeout=1.0):
        while True:
            text = self.line(timeout)
            if text is None:
                raise Fail('no > ok / > err after the result')
            if TERMINATOR.match(text):
                return text

    def console(self, cmd, timeout=15.0):
        self.write('\n'); time.sleep(0.05); self.write(cmd + '\n')
        replies = []
        while True:
            text = self.line(timeout)
            if text is None:
                return False, replies
            if text.startswith('> '):
                replies.append(text[2:])
                if TERMINATOR.match(text):
                    return 'ok' in replies, replies


def open_session(port, boot_wait=None):
    """Opens the port once (on macOS every open restarts the MCU) and switches
    baud rates on the open port, as the page's retries would."""
    s = Session(port, CONSOLE_BAUD)
    if boot_wait is None:
        boot_wait = float(__import__('os').environ.get('PROBE_BOOT_WAIT', '0'))
    if boot_wait:
        time.sleep(boot_wait)
    for _ in range(2):
        for baud in (CONSOLE_BAUD, CONNECT_BAUD):
            s.s.baudrate = baud
            s.buf = b''
            s.s.reset_input_buffer()
            kind, info = s.identify()
            if kind == 'status':
                return s, info, baud
            if kind == 'stock':
                s.close()
                raise Fail(f'looks like stock firmware: {info}')
    s.close()
    raise Fail('no STATUS at either baud rate')


def main(argv):
    port, op, args = argv[1], argv[2], argv[3:]
    s, st, baud = open_session(port)
    print(f'STATUS at {baud}: {st}')
    t0 = time.monotonic()
    try:
        if op == 'status':
            s.command('connect status', lambda k, f, d: print(k, f))
        elif op == 'info':
            s.command('connect info', lambda k, f, d: print(k, {**f, 'title': bytes.fromhex(f.get('title', '')).decode()}))
        elif op == 'ls':
            def entry(k, f, d):
                if k == 'ENTRY':
                    print(('D ' if f['dir'] == '1' else '  ') + f"{int(f['size']):>10}  " + bytes.fromhex(f['name']).decode())
            s.command('connect ls ' + hexenc(args[0]), entry, 20)
        elif op == 'get':
            out = bytearray(); end = {}
            s.command('connect get ' + hexenc(args[0]),
                      lambda k, f, d: out.extend(d) if k == 'DATA' else end.update(f) if k == 'END' else None, 20)
            assert zlib.crc32(out) == int(end['crc'], 16), 'whole-file CRC mismatch'
            open(args[1], 'wb').write(out)
            print(f'{len(out)} bytes')
        elif op == 'put':
            data = open(args[0], 'rb').read()

            def send(k, f, d):
                if k == 'SEND':
                    o, n = int(f['offset']), int(f['size'])
                    s.write(data[o:o + n])
            s.command(f"connect put {hexenc(args[1])} size={len(data)} crc={zlib.crc32(data):08x}", send, 30)
            print(f'{len(data)} bytes')
        elif op in ('mkdir', 'rm'):
            s.command(f'connect {op} ' + hexenc(args[0]))
        elif op == 'mv':
            s.command(f'connect mv {hexenc(args[0])} {hexenc(args[1])}')
        elif op == 'backup':
            arts, cur = {}, {}

            def rec(k, f, d):
                if k == 'INFO':
                    print('INFO', bytes.fromhex(f.get('title', '')).decode(), f)
                elif k == 'BEGIN':
                    cur.clear(); cur.update(kind=f['kind'], data=bytearray(), seq=0)
                elif k == 'DATA':
                    assert int(f['seq']) == cur['seq']; cur['seq'] += 1; cur['data'].extend(d)
                elif k == 'END':
                    assert zlib.crc32(cur['data']) == int(f['crc'], 16) and len(cur['data']) == int(f['size'])
                    arts[cur['kind']] = bytes(cur['data'])
            s.command('connect backup ' + args[0], rec, 20)
            for kind, data in arts.items():
                open(f'{args[1]}.{kind}', 'wb').write(data)
                print(f'{kind}: {len(data)} bytes crc {zlib.crc32(data):08x}')
        elif op == 'restore':
            save = open(args[0], 'rb').read()
            rtc = open(args[1], 'rb').read()[:4] if len(args) > 1 else None
            cmd = f'connect restore size={len(save)} crc={zlib.crc32(save):08x}'
            if rtc:
                cmd += f' rtccrc={zlib.crc32(rtc):08x}'

            def rec(k, f, d):
                if k == 'SEND':
                    src = save if f['kind'] == 'sav' else rtc
                    o, n = int(f['offset']), int(f['size'])
                    s.write(src[o:o + n])
                elif k in ('READY', 'INFO'):
                    print(k, f.get('phase', ''))
            s.command(cmd, rec, 30)
        elif op == 'sdbackup':
            s.command(f'connect sdbackup {args[0]} time={int(time.time())}',
                      lambda k, f, d: print(k, bytes.fromhex(f['name']).decode() if k == 'SAVED' else f), 60)
        elif op == 'console':
            print(s.console(args[0]))
        elif op == 'resync':
            s.write('\n'); time.sleep(0.05); s.write('connect resync\n')
            print(s.line(2), s.line(2), s.line(2))
        else:
            raise SystemExit(__doc__)
        print(f'PASS in {time.monotonic() - t0:.2f} s')
    except Fail as e:
        print(f'FAIL: {e}  (in {time.monotonic() - t0:.2f} s)')
        return 1
    finally:
        s.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
