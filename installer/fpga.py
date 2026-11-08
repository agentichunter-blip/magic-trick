"""Bundled upstream openFPGALoader adapter; never uses a tool from PATH."""
import os, re, subprocess, sys
from pathlib import Path
FPGA_URL='https://github.com/cursedtoast2/ChroMagic/releases/download/v1.0.1/fpga.fs'
FPGA_HASH='79791d915060a9898c9be6adc7ab22f298cbea9619c6bb765b8667db62c337a2'
def tool():
    root=Path(getattr(sys,'_MEIPASS',Path(__file__).parent))/'programmer'
    name='openFPGALoader.exe' if sys.platform=='win32' else 'openFPGALoader'
    return root/'tools'/name

def run(args,log,timeout=120):
    program=tool()
    if not program.is_file(): raise ValueError('Bundled FPGA programmer is missing. Extract the complete download.')
    env=os.environ.copy()
    env['OPENFPGALOADER_SOJ_DIR']=str(program.parent/'share'/'openFPGALoader')
    result=subprocess.run([str(program),*args],capture_output=True,text=True,timeout=timeout,env=env,
                          creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
    log(result.stdout); log(result.stderr)
    if result.returncode: raise ValueError('FPGA programmer failed. Check USB drivers and the installation log; do not erase the device.')
    return result.stdout+result.stderr

def preflight(log):
    run(['--version'],log)
    scan=run(['--scan-usb'],log)
    if len(re.findall(r'^\s*\d+\s+\d+\s+0x33aa:0x0120\b',scan,re.M|re.I))!=1:
        raise ValueError('Connect exactly one Chromatic. Its programmer must be accessible. On Windows complete MRUpdater USB driver setup; on Linux check USB permissions.')
    found=run(['--cable','gwu2x','--detect'],log)
    if not re.search(r'GW5A-25|0x0*1281b\b',found,re.I):
        raise ValueError('Unsupported FPGA hardware. Nothing was written.')

def flash(path,log):
    run(['--cable','gwu2x','--write-flash','--verify','--reset','--force-terminal-mode',str(path)],log,600)
