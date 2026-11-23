"""Prepare pinned, redistributable FPGA tools without requiring runtime installs."""
import hashlib, os, platform, shutil, subprocess, tarfile, urllib.request, zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'programmer'
WORK=ROOT/'.tool-build'
REV='c3a1232f8730dbde42453ab96d0275b29a384271'
def run(*args,**kwargs): subprocess.run([str(a) for a in args],check=True,**kwargs)
def fetch(name,url,sha):
    p=WORK/name
    if not p.exists():
        with urllib.request.urlopen(url,timeout=120) as r: p.write_bytes(r.read())
    if hashlib.sha256(p.read_bytes()).hexdigest()!=sha: raise ValueError('Tool checksum mismatch: '+name)
    return p

# Oldest macOS the Mac programmer must run on (the app's own Python and Tk need 11 on Apple silicon).
MAC_MIN='11.0' if platform.machine()=='arm64' else '10.15'

def mac_min(path):
    """The minimum macOS a Mach-O file says it needs, as a tuple."""
    out=subprocess.check_output(['otool','-l',str(path)],text=True).split()
    for key in ('minos','LC_VERSION_MIN_MACOSX'):
        if key in out:
            i=out.index(key)
            value=out[i+1] if key=='minos' else out[out.index('version',i)+1]
            return tuple(int(x) for x in value.split('.'))
    raise ValueError('No minimum macOS in '+str(path))

def main():
    WORK.mkdir(exist_ok=True)
    if OUT.exists(): shutil.rmtree(OUT)
    OUT.mkdir()
    system=platform.system()
    if system in ('Windows','Linux'):
        name,sha=(('ChroMagician-1.0.3-windows-x64.zip','e31a9ff09cd8460dea09496d1a79f2c0a7cc5f9fe5249726d081663338eb7337') if system=='Windows' else ('ChroMagician-1.0.3-linux-x64.tar.gz','7c2b46ec7c2efad62ed0f77959d71be071d81d9a1bc086241213558f009982cf'))
        p=fetch(name,'https://github.com/cursedtoast2/ChroMagician/releases/download/v1.0.3/'+name,sha)
        extracted=WORK/'extracted'; extracted.mkdir(exist_ok=True)
        if system=='Windows':
            with zipfile.ZipFile(p) as z: z.extractall(extracted)
        else:
            with tarfile.open(p) as t: t.extractall(extracted,filter='data')
        firmware=extracted/'ChroMagician'/'firmware'
        for folder in ['tools','licenses']:
            shutil.copytree(firmware/folder,OUT/folder)
        for p in (OUT/'tools').glob('esptool*'): p.unlink()
    elif system=='Darwin':
        srcarchive=fetch('openFPGALoader.tar.gz','https://codeload.github.com/trabucayre/openFPGALoader/tar.gz/'+REV,'cb697746deb32b24fd3253331296d4659c7a7f377bbecc92acb3904942e08c4b')
        usbarchive=fetch('libusb.tar.bz2','https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2','ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575')
        for p in [srcarchive,usbarchive]:
            with tarfile.open(p) as t: t.extractall(WORK,filter='data')
        usb=WORK/'libusb-1.0.27'; prefix=WORK/'deps'
        # Build for the oldest macOS the app runs on, not the build machine's: without this, a CI
        # runner on macOS 15 made a programmer that macOS 14 and older refuse to start.
        env=dict(os.environ,MACOSX_DEPLOYMENT_TARGET=MAC_MIN,PKG_CONFIG_PATH=str(prefix/'lib/pkgconfig'))
        run(usb/'configure','--prefix='+str(prefix),'--disable-static',cwd=usb,env=env)
        run('make','-j3',cwd=usb,env=env); run('make','install',cwd=usb,env=env)
        src=WORK/('openFPGALoader-'+REV)
        run('cmake','-S',src,'-B',WORK/'build','-DCMAKE_BUILD_TYPE=Release',
            '-DENABLE_CABLE_ALL=OFF','-DENABLE_GOWIN_GWU2X=ON','-DENABLE_USB_SCAN=ON',
            '-DENABLE_VENDORS_ALL=OFF','-DENABLE_GOWIN_SUPPORT=ON','-DCMAKE_OSX_DEPLOYMENT_TARGET='+MAC_MIN,env=env)
        run('cmake','--build',WORK/'build','--parallel','3')
        target=OUT/'tools'; target.mkdir()
        binary=target/'openFPGALoader'; shutil.copy2(WORK/'build/openFPGALoader',binary)
        dylib=target/'libusb-1.0.0.dylib'; shutil.copy2(prefix/'lib/libusb-1.0.0.dylib',dylib)
        deps=subprocess.check_output(['otool','-L',str(binary)],text=True)
        for line in deps.splitlines()[1:]:
            dep=line.strip().split(' (')[0]
            if 'libusb' in dep: run('install_name_tool','-change',dep,'@loader_path/libusb-1.0.0.dylib',binary)
            elif not dep.startswith(('/usr/lib/','/System/Library/')): raise ValueError('Unbundled dependency '+dep)
        run('install_name_tool','-id','@loader_path/libusb-1.0.0.dylib',dylib)
        for p in [dylib,binary]:
            run('codesign','--force','--sign','-',p)
            if mac_min(p)>tuple(map(int,MAC_MIN.split('.'))):
                raise ValueError('%s needs macOS %s; it must run on %s' % (p.name,'.'.join(map(str,mac_min(p))),MAC_MIN))
        (OUT/'licenses').mkdir(); (OUT/'sources').mkdir()
        shutil.copy2(src/'LICENSE',OUT/'licenses/openFPGALoader.txt')
        shutil.copy2(usb/'COPYING',OUT/'licenses/libusb.txt')
        for p in [srcarchive,usbarchive]: shutil.copy2(p,OUT/'sources'/p.name)
    else: raise ValueError('Unsupported build host')
    (OUT/'SOURCES.txt').write_text('openFPGALoader '+REV+' (unmodified), libusb and dependencies.\nLinux/Windows corresponding tool sources and build recipes: https://github.com/cursedtoast2/ChroMagician/releases/tag/v1.0.3\nMac corresponding source archives are included in sources/. Build recipe: installer/prepare_programmer.py.\n')
if __name__=='__main__': main()
