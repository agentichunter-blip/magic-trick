import contextlib, datetime, io, queue, sys, threading, webbrowser
from pathlib import Path
try:
    import tkinter as tk
    from tkinter import ttk, messagebox, filedialog
except ImportError:   # command-line mode does not need a window
    tk = None
import core, fpga

DOCS = 'https://github.com/agentichunter-blip/magic-trick/blob/main/docs/recovery.md'


class App:
    def __init__(self):
        self.root = tk.Tk(); self.root.title('Magic Trick Installer ' + core.INSTALLER_VERSION)
        self.root.geometry('780x680'); self.root.minsize(700, 600)
        self.events = queue.Queue(); self.busy = False; self.history = []
        body = ttk.Frame(self.root, padding=28); body.pack(fill='both', expand=True)
        ttk.Label(body, text='MAGIC TRICK', font=('Helvetica', 28, 'bold')).pack(anchor='w')
        ttk.Label(body, text='1.0 beta for the ModRetro Chromatic · built on CursedToast\'s ChroMagic',
                  font=('Helvetica', 12)).pack(anchor='w', pady=(4, 18))
        ttk.Label(body, wraplength=700, text=(
            'Game Boy, Game Boy Color and Game Gear with cover art, gameplay previews, Game Genie cheats, '
            'save states and Wi-Fi transfers. Installs from stock or ChroMagic firmware, or updates an earlier '
            'Magic Trick install. Writes the MCU application, both FPGA cores and the MCU partition table. '
            'No games or media are included.')).pack(anchor='w')
        row = ttk.Frame(body); row.pack(fill='x', pady=16)
        self.port = tk.StringVar()
        self.ports = ttk.Combobox(row, textvariable=self.port, state='readonly', width=45); self.ports.pack(side='left')
        self.refresh_button = ttk.Button(row, text='Refresh USB ports', command=self.refresh); self.refresh_button.pack(side='right')
        self.consent = tk.BooleanVar()
        ttk.Label(body, wraplength=700, foreground='#b45309', text=(
            'Beta firmware, installed at your own risk. Not made or supported by ModRetro; it may void your '
            'warranty. Read "Installation & recovery" first, and keep the backup folder from your first install.'
        )).pack(anchor='w', pady=(0, 8))
        if sys.platform == 'win32':
            ttk.Label(body, wraplength=700, text=(
                'Windows: before your first install, install ModRetro\'s Chromatic Firmware Updater, run its USB '
                'driver setup once, then close it completely. Magic Trick uses the same programmer driver.'
            )).pack(anchor='w', pady=(0, 8))
        ttk.Checkbutton(body, text='I understand the risk, backed up my SD card, and will keep power and USB connected.',
                        variable=self.consent).pack(anchor='w')
        actions = ttk.Frame(body); actions.pack(anchor='w', pady=14)
        self.install_button = ttk.Button(actions, text='Back up & install / update', command=lambda: self.start('install'))
        self.install_button.pack(side='left')
        self.uninstall_button = ttk.Button(actions, text='Uninstall…', command=lambda: self.start('uninstall'))
        self.uninstall_button.pack(side='left', padx=(10, 0))
        self.restore_button = ttk.Button(actions, text='Restore backup…', command=self.start_restore)
        self.restore_button.pack(side='left', padx=(10, 0))
        self.output = tk.Text(body, height=14, wrap='word', state='disabled', font=('Courier', 10))
        self.output.pack(fill='both', expand=True)
        footer = ttk.Frame(body); footer.pack(fill='x', pady=(16, 0))
        for label, url in [('Installation & recovery', DOCS),
                           ('Source & credits', 'https://github.com/agentichunter-blip/magic-trick'),
                           ('Support on Ko-fi ↗', 'https://ko-fi.com/ryansag')]:
            ttk.Button(footer, text=label, command=lambda u=url: webbrowser.open(u)).pack(side='left', padx=(0, 8))
        ttk.Button(footer, text='Copy log', command=self.copy_log).pack(side='right')
        self.root.protocol('WM_DELETE_WINDOW', self.close); self.refresh(); self.root.after(100, self.poll)

    def refresh(self):
        values = core.device_ports(); self.ports['values'] = values
        self.port.set(values[0] if len(values) == 1 else '')

    def log(self, text):
        if text and text.strip():
            self.history.append(text.rstrip())
            self.events.put(('log', text.rstrip()))

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.output.get('1.0', 'end-1c'))

    def save_log(self, folder):
        """Keep a log on disk for every run, even one that stopped before the backup folder existed."""
        folder = Path(folder)
        target = folder / 'installer-log.txt' if folder.is_dir() else folder.parent / (folder.name + '-log.txt')
        try:
            target.write_text('\n'.join(self.history) + '\n', encoding='utf-8')
            self.log('Log saved: ' + str(target))
        except OSError:
            pass

    def set_busy(self, busy):
        self.busy = busy
        state = 'disabled' if busy else 'normal'
        for b in (self.install_button, self.uninstall_button, self.restore_button, self.refresh_button):
            b['state'] = state
        self.ports['state'] = 'disabled' if busy else 'readonly'

    def start(self, operation):
        if self.busy:
            return
        if not self.port.get() or not self.consent.get():
            messagebox.showinfo('Before you start', 'Select the Chromatic USB port and confirm your SD backup.'); return
        parent = filedialog.askdirectory(title='Choose where to keep your private device backup')
        if not parent:
            return
        service = core.cloud_service(parent)
        if service and not messagebox.askyesno('Magic Trick', (
                'This folder looks like it syncs to ' + service + '.\n\nThe backup holds your Chromatic\'s settings, '
                'including any saved Wi-Fi password, and would be uploaded there. A folder that stays on this '
                'computer (or a USB drive) is safer.\n\nUse this folder anyway?')):
            return
        if operation == 'install':
            question = ('Install Magic Trick ' + core.RELEASE + '?\n\nThe app backs up your MCU, then writes the MCU '
                        'application, both FPGA cores and the partition table. FPGA writes often need several '
                        'attempts and can take up to 15 minutes. Keep USB and power connected. Your SD card is not written.')
        else:
            question = ('Uninstall Magic Trick?\n\nThe app backs up your MCU, writes the ChroMagic 1.0.1 FPGA and '
                        'restores the standard partition layout, so the application you had before Magic Trick 1.0 '
                        'runs again. To return fully to stock afterwards, use ModRetro\'s official updater.')
        if not messagebox.askyesno('Magic Trick', question):
            return
        port = self.port.get()
        folder = Path(parent) / ('Magic-Trick-backup-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        self.run(lambda: getattr(core, operation)(port, folder, self.log), folder)

    def start_restore(self):
        """Recovery: put back the backup made before Magic Trick was first installed. Works without
        a USB serial port, because the FPGA (which provides it) is repaired first."""
        if self.busy:
            return
        if not self.consent.get():
            messagebox.showinfo('Before you start', 'Confirm your SD backup first.'); return
        chosen = filedialog.askopenfilename(
            title='Choose mcu-backup.bin from your FIRST Magic Trick install',
            filetypes=[('MCU backup', 'mcu-backup.bin'), ('All files', '*')])
        if not chosen:
            return
        try:
            _, app = core.check_restore_backup(chosen)
        except (OSError, ValueError) as exc:
            messagebox.showerror('Magic Trick', str(exc)); return
        if not messagebox.askyesno('Magic Trick', (
                'Restore this backup?\n\nUse this if install, update or uninstall can\'t finish, or the Chromatic '
                'no longer starts. The app writes the ChroMagic 1.0.1 FPGA, then puts back your saved MCU flash '
                '(application ' + app + ' and your settings). It takes 10–20 minutes. Keep USB and power '
                'connected. Your SD card is not written.\n\nTo return fully to stock afterwards, use ModRetro\'s '
                'official updater.')):
            return
        folder = Path(chosen).parent.parent / ('Magic-Trick-restore-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        self.run(lambda: core.restore(chosen, folder, self.log), folder)

    def run(self, operation, folder):
        self.set_busy(True)
        self.history = ['Magic Trick Installer ' + core.INSTALLER_VERSION + ' on ' + sys.platform,
                        'Started ' + datetime.datetime.now().isoformat(timespec='seconds')]

        def worker():
            try:
                class Sink(io.TextIOBase):
                    """Joins tool output fragments into whole lines for the log box."""
                    def __init__(sink):
                        sink.buffer = ''
                    def write(sink, text):
                        sink.buffer += text.replace('\r', '\n')
                        *lines, sink.buffer = sink.buffer.split('\n')
                        for line in lines:
                            if line.strip():
                                self.log(line)
                        return len(text)
                    def flush(sink):
                        pass
                    def finish(sink):
                        if sink.buffer.strip():
                            self.log(sink.buffer)
                        sink.buffer = ''
                out, err = Sink(), Sink()
                try:
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        operation()
                finally:
                    out.finish(); err.finish()
            except (Exception, SystemExit) as exc:
                self.log('STOPPED: ' + str(exc) + '\nDo not erase the device. Your private backup: ' + str(folder) +
                         '\nSee "Installation & recovery". If install, update and uninstall keep refusing, '
                         'use Restore backup with mcu-backup.bin from your first install.')
            finally:
                self.save_log(folder)
                self.events.put(('done', ''))
        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        while not self.events.empty():
            kind, text = self.events.get()
            if kind == 'done':
                self.set_busy(False)
            else:
                self.output['state'] = 'normal'; self.output.insert('end', text + '\n')
                self.output.see('end'); self.output['state'] = 'disabled'
        self.root.after(100, self.poll)

    def close(self):
        if self.busy:
            messagebox.showinfo('Operation in progress', 'Keep the installer open until it finishes.'); return
        self.root.destroy()


def self_test():
    core.check_images()
    assert core.known_running(core.TARGET)
    import esptool, serial   # noqa: F401  (bundled dependencies)
    import subprocess
    out = subprocess.run(core.esptool_command() + ['version'], capture_output=True, text=True, timeout=60).stdout
    assert 'esptool' in out, 'bundled esptool did not run: ' + out
    fpga.run(['--version'], print)
    print('Magic Trick installer ' + core.INSTALLER_VERSION + ' (firmware ' + core.RELEASE + '): bundled firmware and dependencies OK')


def run_esptool(args):
    """Child-process entry point used by core.run_tool in the packaged app."""
    import os, esptool
    sys.argv = ['esptool'] + args
    code = 0
    try:
        esptool._main()
    except SystemExit as exc:
        code = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
    except BaseException:
        import traceback
        traceback.print_exc()
        code = 1
    finally:
        sys.stdout.flush(); sys.stderr.flush()
        # Exit at once: closing the serial port during interpreter shutdown can hang on Windows.
        os._exit(code)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--esptool':
        run_esptool(sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == '--read-flash':
        # Child process used by core.read_flash: --read-flash PORT OFFSET SIZE OUT
        import os
        code = 0
        try:
            core.read_flash_session(sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16), sys.argv[5])
        except BaseException as exc:
            print('A fatal error occurred: ' + str(exc), flush=True)
            code = 2
        sys.stdout.flush()
        os._exit(code)
    elif '--self-test' in sys.argv:
        self_test()
    elif len(sys.argv) == 4 and sys.argv[1] in ('--install', '--uninstall'):
        # Command-line mode for hardware testing: app.py --install PORT BACKUP_DIR
        getattr(core, sys.argv[1][2:])(sys.argv[2], sys.argv[3], print)
    elif len(sys.argv) == 4 and sys.argv[1] == '--restore':
        # app.py --restore path/to/mcu-backup.bin NEW_FOLDER
        core.restore(sys.argv[2], sys.argv[3], print)
    else:
        App().root.mainloop()
