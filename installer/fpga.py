"""Bundled upstream openFPGALoader adapter; never uses a tool from PATH."""
import os, re, subprocess, sys, time
from pathlib import Path

# While a multiboot image sits in FPGA flash slot 0, the first flash write after
# a reboot fails during erase ("write to flash failed"); re-running succeeds.
# Hardware measurement: 1 to 5 attempts. A failed attempt leaves JTAG usable,
# so retrying is safe; every completed write is read back and verified. Without an
# FPGA restart between attempts the flash interface sometimes stays unresponsive.
WRITE_ATTEMPTS = 12
RETRY_SETTLE = 10
ATTEMPT_TIMEOUT = 600


class ProgrammerError(ValueError):
    def __init__(self, message, output=''):
        super().__init__(message)
        self.output = output


class ProgrammerStuck(ProgrammerError):
    pass


# Seen on hardware: the USB programming chip can stop reaching the FPGA mid-write (the screen
# goes dark and JTAG finds no device). Retrying can't fix that; unplugging the USB cable does.
STUCK_SIGNS = ('no device found', 'found 0 devices', 'Unknown device with IDCODE')
STUCK_MESSAGE = ('The USB programmer stopped responding. Unplug the USB cable, wait 5 seconds, plug it back in '
                 '(keep the Chromatic switched on), then run the same operation again. Nothing else is needed.')


def looks_stuck(output):
    return any(sign in output for sign in STUCK_SIGNS)


def tool():
    root = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'programmer'
    name = 'openFPGALoader.exe' if sys.platform == 'win32' else 'openFPGALoader'
    return root / 'tools' / name


def run(args, log, timeout=120):
    program = tool()
    if not program.is_file():
        raise ValueError('Bundled FPGA programmer is missing. Extract the complete download.')
    env = os.environ.copy()
    env['OPENFPGALOADER_SOJ_DIR'] = str(program.parent / 'share' / 'openFPGALoader')
    result = subprocess.run([str(program), *args], capture_output=True, text=True, timeout=timeout, env=env,
                            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
    output = (result.stdout or '') + (result.stderr or '')
    # Progress bars repaint with carriage returns; keep the log readable.
    log('\n'.join(line for line in output.replace('\r', '\n').splitlines()
                  if line.strip() and '%' not in line))
    if result.returncode:
        raise ProgrammerError('FPGA programmer failed. Check USB drivers and the installation log; do not erase the device.',
                              output)
    return output


def preflight(log):
    run(['--version'], log)
    scan = run(['--scan-usb'], log)
    if len(re.findall(r'^\s*\d+\s+\d+\s+0x33aa:0x0120\b', scan, re.M | re.I)) != 1:
        raise ValueError('Connect exactly one Chromatic. Its programmer must be accessible. On Windows complete MRUpdater USB driver setup; on Linux check USB permissions.')
    try:
        found = run(['--cable', 'gwu2x', '--detect'], log)
    except ProgrammerError as exc:
        found = exc.output
    if not re.search(r'family|model|IDCODE|GW5A', found, re.I) or looks_stuck(found):
        raise ValueError('The Chromatic\'s FPGA isn\'t answering. Make sure the Chromatic is switched on, then unplug '
                         'the USB cable, wait 5 seconds and plug it back in. Nothing was written.')
    if not re.search(r'GW5A-25|0x0*1281b\b', found, re.I):
        raise ValueError('Unsupported FPGA hardware. Nothing was written.')


def flash(path, offset, log, attempts=WRITE_ATTEMPTS, sleep=time.sleep):
    """Write and verify one image at a flash offset, retrying failed attempts."""
    if offset % 0x10000:
        raise ValueError('FPGA slot offset must be 64 KiB aligned.')
    stuck = 0
    for attempt in range(1, attempts + 1):
        log(f'FPGA write at 0x{offset:06x}, attempt {attempt} of {attempts}…')
        try:
            output = run(['--cable', 'gwu2x', '--write-flash', '--verify', '-o', hex(offset),
                          '--force-terminal-mode', str(path)], log, ATTEMPT_TIMEOUT)
        except (ProgrammerError, subprocess.TimeoutExpired) as exc:
            stuck = stuck + 1 if looks_stuck(getattr(exc, 'output', '')) else 0
            if stuck >= 2:
                raise ProgrammerStuck(STUCK_MESSAGE) from exc
            if attempt == attempts:
                raise ProgrammerError(f'FPGA write at 0x{offset:06x} did not succeed after {attempts} attempts. '
                                      'The device can still be reached over USB: run the installer again.') from exc
            log('Attempt failed. This is expected on the first tries; restarting the FPGA and retrying.')
            # A failed attempt can leave the flash interface unresponsive until the
            # FPGA reconfigures; restart it and let the MCU settle before retrying.
            try:
                reset(log)
            except (ProgrammerError, subprocess.TimeoutExpired):
                pass
            sleep(RETRY_SETTLE)
            continue
        # openFPGALoader exits non-zero when its read-back verification fails.
        return attempt


def reset(log):
    """Reboot the FPGA from flash slot 0 (this also restarts the MCU)."""
    run(['--cable', 'gwu2x', '--reset'], log)
