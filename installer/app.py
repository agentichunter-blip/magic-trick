import contextlib, datetime, io, json, math, os, queue, random, re, subprocess, sys, threading, time, webbrowser
from pathlib import Path
try:
    import tkinter as tk
    from tkinter import messagebox, filedialog, font as tkfont
except ImportError:   # command-line mode does not need a window
    tk = None
import core, fpga

DOCS = 'https://github.com/agentichunter-blip/magic-trick/blob/main/docs/recovery.md'
SOURCE = 'https://github.com/agentichunter-blip/magic-trick'
KOFI = 'https://ko-fi.com/ryansag'
ASSETS = Path(getattr(sys, '_MEIPASS', Path(__file__).parent)) / 'assets'

# Palette from the Magic Trick logo (same as the website).
BG, PANEL, PANEL_2, LINE = '#060a19', '#0d1430', '#141d3f', '#26305a'
INK, MUTED, FAINT = '#f4f7f9', '#9ea9c2', '#5f6b88'
ACCENT, BLUE, GREEN, AMBER, RED, LILAC = '#5bc4ff', '#2f67c0', '#5be38a', '#ffb95c', '#ec324e', '#b9a8ff'

# Steps the core reports (core.emit), with a short pixel label, what's happening in plain words,
# and a rough duration in minutes used for the progress bar and the time-left estimate.
STEPS = {
    'check': ('CHECK', 'Making sure your Chromatic is connected and the firmware files are intact.', 0.5),
    'backup': ('BACK UP', 'Saving a full copy of your MCU, so you can always go back. This is the longest step.', 6),
    'precopy': ('SAFETY COPY', 'Saving a copy of what is on the MCU right now, just in case.', 6),
    'mcu': ('MCU APP', 'Writing the Magic Trick app to the MCU.', 1.5),
    'fpga_chromagic': ('FPGA PREP', 'Putting the ChroMagic core in place so the FPGA can be written safely.', 2.5),
    'fpga_gg': ('GAME GEAR CORE', 'Writing the Game Gear core to the FPGA.', 2.5),
    'fpga_gb': ('GAME BOY CORE', 'Writing the Game Boy and Game Boy Color core to the FPGA.', 2.5),
    'table': ('SWITCH OVER', 'Switching the MCU over to the new firmware.', 0.3),
    'mcu_restore': ('RESTORE MCU', 'Putting your saved MCU flash back. This takes a few minutes.', 6),
    'confirm': ('RESTART', 'Restarting your Chromatic and checking that it starts up.', 1),
}
PLANS = {
    'install': ['check', 'backup', 'mcu', 'fpga_gg', 'fpga_gb', 'table', 'confirm'],
    'uninstall': ['check', 'backup', 'fpga_chromagic', 'table', 'confirm'],
    'restore': ['check', 'precopy', 'fpga_chromagic', 'mcu_restore', 'confirm'],
}
TITLES = {'install': 'INSTALLING', 'uninstall': 'UNINSTALLING', 'restore': 'RESTORING'}
SPINNER = '▖▘▝▗'


def load_pixel_font(path):
    """Make the bundled Unscii pixel font (public domain) available to Tk for this process only."""
    path = str(path)
    try:
        import ctypes, ctypes.util
        if sys.platform == 'win32':
            return ctypes.windll.gdi32.AddFontResourceExW(path, 0x10, 0) > 0   # FR_PRIVATE
        if sys.platform == 'darwin':
            cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library('CoreFoundation'))
            ct = ctypes.cdll.LoadLibrary(ctypes.util.find_library('CoreText'))
            cf.CFURLCreateFromFileSystemRepresentation.restype = ctypes.c_void_p
            cf.CFURLCreateFromFileSystemRepresentation.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_bool]
            ct.CTFontManagerRegisterFontsForURL.restype = ctypes.c_bool
            ct.CTFontManagerRegisterFontsForURL.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
            raw = path.encode()
            url = cf.CFURLCreateFromFileSystemRepresentation(None, raw, len(raw), False)
            return bool(ct.CTFontManagerRegisterFontsForURL(url, 1, None))   # process scope
        fc = ctypes.CDLL(ctypes.util.find_library('fontconfig') or 'libfontconfig.so.1')
        fc.FcConfigGetCurrent.restype = ctypes.c_void_p
        fc.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        return bool(fc.FcConfigAppFontAddFile(fc.FcConfigGetCurrent(), path.encode()))
    except Exception:
        return False


class PixelButton(tk.Label if tk else object):
    """A flat, retro button that looks the same on every platform (native buttons ignore colours on macOS)."""
    def __init__(self, master, text, command, font, kind='primary', **kw):
        colours = {'primary': (ACCENT, BG, '#8fd8ff'), 'secondary': (PANEL_2, INK, LINE), 'danger': (RED, INK, '#ff6178')}
        self.base, self.ink, self.hover = colours[kind]
        super().__init__(master, text=text, font=font, bg=self.base, fg=self.ink, padx=14, pady=8,
                         cursor='hand2', takefocus=1, highlightthickness=2, highlightbackground=BG,
                         highlightcolor=INK, **kw)
        self.command, self.enabled = command, True
        self.bind('<Button-1>', lambda e: self.invoke())
        self.bind('<Return>', lambda e: self.invoke())
        self.bind('<space>', lambda e: self.invoke())
        self.bind('<Enter>', lambda e: self.enabled and self.configure(bg=self.hover))
        self.bind('<Leave>', lambda e: self.configure(bg=self.base if self.enabled else PANEL))

    def invoke(self):
        if self.enabled:
            self.command()

    def set_enabled(self, enabled):
        self.enabled = enabled
        self.configure(bg=self.base if enabled else PANEL, fg=self.ink if enabled else FAINT,
                       cursor='hand2' if enabled else 'arrow')


class App:
    def __init__(self):
        loaded = load_pixel_font(ASSETS / 'unscii-8.ttf')
        self.root = tk.Tk()
        self.root.title('Magic Trick Installer ' + core.INSTALLER_VERSION)
        self.root.geometry('900x700'); self.root.minsize(820, 660)
        self.root.configure(bg=BG)
        families = set(tkfont.families(self.root))
        pixel = 'Unscii' if loaded and 'Unscii' in families else ('Courier New' if 'Courier New' in families else 'Courier')
        body = tkfont.nametofont('TkDefaultFont').actual('family')
        self.f_pixel = (pixel, -16)
        self.f_pixel_big = (pixel, -32)
        self.f_pixel_huge = (pixel, -64)
        self.f_body = (body, 13)
        self.f_body_big = (body, 15)
        self.f_small = (body, 11)
        self.events = queue.Queue(); self.busy = False; self.history = []
        self.operation = None; self.folder = None; self.tick = 0
        core.EVENT_HOOK = lambda kind, data: self.events.put(('event', (kind, data)))

        self.build_header()
        self.screen = tk.Frame(self.root, bg=BG)
        self.screen.pack(fill='both', expand=True, padx=28, pady=(8, 0))
        self.build_footer()
        self.show_home()
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        self.root.after(100, self.poll)
        self.root.after(200, self.animate)

    # ---------- shared pieces ----------
    def label(self, master, text='', font=None, fg=INK, bg=None, wrap=0, **kw):
        return tk.Label(master, text=text, font=font or self.f_body, fg=fg, bg=bg or master['bg'],
                        wraplength=wrap, justify='left', anchor='w', **kw)

    def build_header(self):
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill='x', padx=28, pady=(18, 4))
        try:
            self.logo = tk.PhotoImage(file=str(ASSETS / 'logo-mark.png'))
            tk.Label(bar, image=self.logo, bg=BG).pack(side='left', padx=(0, 16))
        except tk.TclError:
            self.logo = None
        words = tk.Frame(bar, bg=BG); words.pack(side='left')
        self.label(words, 'MAGIC TRICK', self.f_pixel_big, ACCENT).pack(anchor='w')
        self.label(words, 'INSTALLER ' + core.INSTALLER_VERSION.upper(), self.f_pixel, MUTED).pack(anchor='w', pady=(6, 0))
        self.status_box = tk.Frame(words, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        self.status_box.pack(anchor='w', pady=(10, 0))
        self.status_dot = self.label(self.status_box, '■', self.f_pixel, AMBER, PANEL)
        self.status_dot.pack(side='left', padx=(10, 6), pady=5)
        self.status_text = self.label(self.status_box, 'LOOKING…', self.f_pixel, INK, PANEL)
        self.status_text.pack(side='left', padx=(0, 10), pady=5)

    def build_footer(self):
        bar = tk.Frame(self.root, bg=BG); bar.pack(fill='x', side='bottom', padx=28, pady=(6, 16))
        for text, url in [('Installation & recovery', DOCS), ('Source & credits', SOURCE), ('Support on Ko-fi', KOFI)]:
            link = self.label(bar, text, self.f_small, MUTED, cursor='hand2')
            link.pack(side='left', padx=(0, 18))
            link.bind('<Button-1>', lambda e, u=url: webbrowser.open(u))
            link.bind('<Enter>', lambda e, w=link: w.configure(fg=ACCENT))
            link.bind('<Leave>', lambda e, w=link: w.configure(fg=MUTED))
        copy = self.label(bar, 'Copy log', self.f_small, MUTED, cursor='hand2'); copy.pack(side='right')
        copy.bind('<Button-1>', lambda e: self.copy_log())

    def clear(self):
        for child in self.screen.winfo_children():
            child.destroy()

    # ---------- home ----------
    def show_home(self):
        self.clear(); self.mode = 'home'
        self.label(self.screen, 'Game Boy, Game Boy Color and Game Gear on one Chromatic. What would you like to do?',
                   self.f_body_big, INK, wrap=840).pack(anchor='w', pady=(4, 10))
        self.tiles = {}
        for key, title, text, kind in [
                ('install', 'INSTALL / UPDATE', 'Put Magic Trick 1.0 beta on your Chromatic, or update it. Works from '
                 'stock or ChroMagic firmware. About 15 minutes.', 'primary'),
                ('uninstall', 'UNINSTALL', 'Go back to the firmware you had before Magic Trick 1.0. ModRetro\'s '
                 'updater can take you to full stock afterwards.', 'secondary'),
                ('restore', 'RESTORE BACKUP', 'Something went wrong, or it won\'t start? Puts back the backup from your '
                 'first install, whatever state the Chromatic is in.', 'secondary')]:
            tile = tk.Frame(self.screen, bg=PANEL, highlightthickness=2,
                            highlightbackground=ACCENT if kind == 'primary' else LINE)
            tile.pack(fill='x', pady=4)
            words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=10)
            self.label(words, title, self.f_pixel, ACCENT if kind == 'primary' else INK).pack(anchor='w')
            self.label(words, text, self.f_body, MUTED, wrap=600).pack(anchor='w', pady=(5, 0))
            button = PixelButton(tile, 'START ▶', (lambda k=key: self.begin(k)), self.f_pixel, kind)
            button.pack(side='right', padx=16)
            self.tiles[key] = button

        note = tk.Frame(self.screen, bg=BG); note.pack(fill='x', pady=(10, 0))
        self.label(note, '! BETA: installed at your own risk. Magic Trick isn\'t made or supported by ModRetro and may '
                   'void your warranty. Read "Installation & recovery" first, and keep the backup folder from your '
                   'first install somewhere safe (not a cloud folder).', self.f_body, AMBER, wrap=840).pack(anchor='w')
        if sys.platform == 'win32':
            self.label(note, 'Windows: before your first install, install ModRetro\'s Chromatic Firmware Updater, run '
                       'its USB driver setup once, then close it completely.', self.f_body, MUTED,
                       wrap=840).pack(anchor='w', pady=(6, 0))
        self.consent = getattr(self, 'consent', False)
        self.check = self.label(self.screen, '', self.f_pixel, INK, cursor='hand2')
        self.check.pack(anchor='w', pady=(12, 0))
        self.check.bind('<Button-1>', lambda e: self.toggle_consent())
        self.draw_consent()
        self.refresh_device()

    def toggle_consent(self):
        self.consent = not self.consent
        self.draw_consent()

    def draw_consent(self):
        mark = '[X]' if self.consent else '[ ]'
        self.check.configure(text=mark + ' I\'VE BACKED UP MY SD CARD AND I ACCEPT THE RISK',
                             fg=GREEN if self.consent else INK)

    def refresh_device(self):
        """Home screen only: show whether a Chromatic is connected. Listing ports doesn't touch the device."""
        if self.mode != 'home':
            return
        try:
            ports = core.device_ports()
        except Exception:
            ports = []
        self.ports = ports
        if len(ports) == 1:
            dot, text = GREEN, 'CHROMATIC FOUND  ' + Path(ports[0]).name.upper()
        elif len(ports) > 1:
            dot, text = AMBER, 'UNPLUG ALL BUT ONE CHROMATIC'
        else:
            dot, text = AMBER, 'PLUG IN + SWITCH ON YOUR CHROMATIC'
        self.status_dot.configure(fg=dot); self.status_text.configure(text=text)
        for key in ('install', 'uninstall'):
            self.tiles[key].set_enabled(len(ports) == 1)
        self.tiles['restore'].set_enabled(len(ports) <= 1)
        if not getattr(self, 'refresh_pending', False):
            self.refresh_pending = True
            self.root.after(1500, self.refresh_again)

    def refresh_again(self):
        self.refresh_pending = False
        self.refresh_device()

    # ---------- starting an operation ----------
    def begin(self, operation):
        if self.busy:
            return
        if not self.consent:
            messagebox.showinfo('Before you start', 'Tick the box at the bottom first: you understand the risk, backed '
                                'up your SD card, and will keep the Chromatic on and plugged in.'); return
        if operation == 'restore':
            return self.begin_restore()
        if len(self.ports) != 1:
            messagebox.showinfo('Magic Trick', 'Connect one Chromatic and switch it on first.'); return
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
            question = ('Install Magic Trick ' + core.RELEASE + '?\n\nThe installer backs up your MCU first, then writes '
                        'Magic Trick. It takes about 15 minutes. Some steps retry a few times; that\'s normal. Keep '
                        'the Chromatic on and plugged in. Your SD card isn\'t touched.')
        else:
            question = ('Uninstall Magic Trick?\n\nThe installer backs up your MCU, puts back the ChroMagic FPGA and '
                        'switches back to the firmware you had before Magic Trick 1.0. About 10 minutes. To return '
                        'fully to stock afterwards, use ModRetro\'s official updater.')
        if not messagebox.askyesno('Magic Trick', question):
            return
        port = self.ports[0]
        folder = Path(parent) / ('Magic-Trick-backup-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        self.run(operation, lambda: getattr(core, operation)(port, folder, self.log), folder)

    def begin_restore(self):
        """Recovery: put back the backup made before Magic Trick was first installed. Works without
        a USB serial port, because the FPGA (which provides it) is repaired first."""
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
                'no longer starts. The installer writes the ChroMagic FPGA, then puts back your saved MCU flash '
                '(application ' + app + ' and your settings). About 15 minutes. Keep the Chromatic on and plugged '
                'in. Your SD card isn\'t touched.\n\nTo return fully to stock afterwards, use ModRetro\'s '
                'official updater.')):
            return
        folder = Path(chosen).parent.parent / ('Magic-Trick-restore-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        self.run('restore', lambda: core.restore(chosen, folder, self.log), folder)

    # ---------- running ----------
    def run(self, operation, work, folder):
        self.busy, self.operation, self.folder = True, operation, folder
        self.plan = list(PLANS[operation]); self.current = None; self.done_steps = set()
        self.step_started = self.started = time.monotonic(); self.step_fraction = None; self.note = ''
        self.history = ['Magic Trick Installer ' + core.INSTALLER_VERSION + ' on ' + sys.platform,
                        'Started ' + datetime.datetime.now().isoformat(timespec='seconds')]
        self.show_running()

        def worker():
            error = None
            try:
                class Sink(io.TextIOBase):
                    """Joins tool output fragments into whole lines for the log."""
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
                        work()
                finally:
                    out.finish(); err.finish()
            except (Exception, SystemExit) as exc:
                error = str(exc) or exc.__class__.__name__
                self.log('STOPPED: ' + error)
            finally:
                self.save_log(folder)
                self.events.put(('finished', error))
        threading.Thread(target=worker, daemon=True).start()

    def show_running(self):
        self.clear(); self.mode = 'running'
        self.status_dot.configure(fg=ACCENT); self.status_text.configure(text='WORKING… KEEP IT PLUGGED IN')
        top = tk.Frame(self.screen, bg=BG); top.pack(fill='x')
        self.run_title = self.label(top, TITLES[self.operation], self.f_pixel_big, INK)
        self.run_title.pack(side='left')
        main = tk.Frame(self.screen, bg=BG); main.pack(fill='x', pady=(14, 0))
        steps_box = tk.Frame(main, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        steps_box.pack(side='left', fill='y', ipadx=10, ipady=8)
        self.step_rows = tk.Frame(steps_box, bg=PANEL); self.step_rows.pack(padx=12, pady=8, anchor='n')
        right = tk.Frame(main, bg=BG); right.pack(side='left', fill='both', expand=True, padx=(20, 0))
        self.now_title = self.label(right, '', self.f_pixel_big, ACCENT); self.now_title.pack(anchor='w')
        self.now_text = self.label(right, '', self.f_body_big, INK, wrap=460); self.now_text.pack(anchor='w', pady=(10, 0))
        self.bar = tk.Canvas(right, height=34, bg=BG, highlightthickness=0); self.bar.pack(fill='x', pady=(18, 6))
        self.stats = self.label(right, '', self.f_pixel, MUTED); self.stats.pack(anchor='w')
        self.note_label = self.label(right, '', self.f_body, AMBER, wrap=460); self.note_label.pack(anchor='w', pady=(12, 0))
        self.label(right, 'Keep the Chromatic switched on and the USB cable plugged in until it says DONE.',
                   self.f_body, MUTED, wrap=460).pack(anchor='w', pady=(12, 0))
        self.details_toggle = self.label(self.screen, '▸ SHOW DETAILS', self.f_pixel, MUTED, cursor='hand2')
        self.details_toggle.pack(anchor='w', pady=(18, 4))
        self.details_toggle.bind('<Button-1>', lambda e: self.toggle_details())
        self.output = tk.Text(self.screen, height=10, wrap='word', bg=PANEL, fg=MUTED, insertbackground=INK,
                              relief='flat', font=('Courier', 10), highlightthickness=1, highlightbackground=LINE)
        self.output.insert('end', '\n'.join(self.history) + '\n'); self.output.configure(state='disabled')
        self.details_shown = False
        self.draw_steps(); self.draw_progress()

    def toggle_details(self):
        self.details_shown = not self.details_shown
        if self.details_shown:
            self.output.pack(fill='both', expand=True)
            self.details_toggle.configure(text='▾ HIDE DETAILS')
            self.output.see('end')
        else:
            self.output.pack_forget()
            self.details_toggle.configure(text='▸ SHOW DETAILS')

    def draw_steps(self):
        for child in self.step_rows.winfo_children():
            child.destroy()
        for key in self.plan:
            name = STEPS[key][0]
            if key == 'fpga_chromagic' and self.operation != 'install':
                name = 'CHROMAGIC CORE'
            if key == 'table' and self.operation == 'uninstall':
                name = 'SWITCH BACK'
            if key in self.done_steps:
                mark, colour = '■', GREEN
            elif key == self.current:
                mark, colour = '▶' if self.tick % 2 == 0 else ' ', ACCENT
            else:
                mark, colour = '□', FAINT
            self.label(self.step_rows, f'{mark} {name}', self.f_pixel, colour if key != self.current else INK,
                       PANEL).pack(anchor='w', pady=3)

    def progress(self):
        """Overall fraction and minutes left, from step durations and any measured progress in the current step."""
        weights = [STEPS[k][2] for k in self.plan]
        total = sum(weights) or 1
        done = sum(STEPS[k][2] for k in self.plan if k in self.done_steps)
        current = 0.0
        if self.current in STEPS and self.current not in self.done_steps:
            w = STEPS[self.current][2]
            if self.step_fraction is not None:
                f = self.step_fraction
            else:
                f = 1 - math.exp(-(time.monotonic() - self.step_started) / (w * 60))
            current = w * min(f, 0.97)
        fraction = min((done + current) / total, 0.99)
        return fraction, max(total - done - current, 0)

    def draw_progress(self):
        if self.mode != 'running':
            return
        fraction, left = self.progress()
        self.bar.delete('all')
        width = max(self.bar.winfo_width(), 400); blocks = 24; gap = 4
        size = (width - gap * (blocks - 1)) / blocks
        lit = fraction * blocks
        for i in range(blocks):
            x = i * (size + gap)
            if i < int(lit):
                colour = ACCENT
            elif i == int(lit):
                colour = BLUE if self.tick % 2 == 0 else PANEL_2
            else:
                colour = PANEL
            self.bar.create_rectangle(x, 4, x + size, 30, fill=colour, outline=LINE)
        elapsed = int(time.monotonic() - self.started)
        left_text = 'ABOUT %d MIN LEFT' % max(1, math.ceil(left)) if fraction < 0.98 else 'ALMOST THERE'
        self.stats.configure(text='%d%%   ·   %d:%02d   ·   %s' % (int(fraction * 100), elapsed // 60, elapsed % 60, left_text))
        key = self.current
        if key in STEPS:
            name, text, _ = STEPS[key]
            if key == 'fpga_chromagic' and self.operation != 'install':
                name, text = 'CHROMAGIC CORE', 'Writing the ChroMagic FPGA core.'
            if key == 'table' and self.operation == 'uninstall':
                name, text = 'SWITCH BACK', 'Switching the MCU back to the firmware you had before.'
            self.now_title.configure(text=name + ' ' + SPINNER[self.tick % 4])
            self.now_text.configure(text=text)
        self.note_label.configure(text=self.note)

    def on_line(self, line):
        """Turn tool output into progress and reassuring notes."""
        m = re.search(r'Read (\d+)% of the MCU flash', line)
        if m:
            self.step_fraction = int(m.group(1)) / 100; return
        m = re.search(r'Writing at 0x[0-9a-f]+\.\.\. \((\d+) ?%\)', line)
        if m and self.current in ('mcu', 'mcu_restore', 'table'):
            self.step_fraction = int(m.group(1)) / 100; return
        m = re.search(r'FPGA write at 0x[0-9a-f]+, attempt (\d+) of (\d+)', line)
        if m and int(m.group(1)) > 1:
            self.note = ('The FPGA write didn\'t verify on try %d, so it\'s trying again (%s of %s). This is normal: '
                         'it often takes a few tries.' % (int(m.group(1)) - 1, m.group(1), m.group(2)))
            self.step_started = time.monotonic(); return
        if re.search(r'Read at 0x[0-9a-f]+ failed', line):
            self.note = 'A few bytes got lost over USB, so that part is being read again. This is normal.'; return
        if 'MCU write at' in line and 'retrying' in line:
            self.note = 'The MCU write didn\'t verify, so it\'s being written again. That\'s safe.'; return
        if 'Waiting for the Chromatic to reconnect' in line:
            self.note = 'Waiting for the Chromatic to reconnect. The first time, Windows may set up drivers; this can take a minute or two.'

    # ---------- finished ----------
    def finished(self, error):
        self.busy = False
        stage = None
        try:
            stage = json.loads((Path(self.folder) / 'installation.json').read_text()).get('stage')
        except (OSError, ValueError, TypeError):
            pass
        self.clear(); self.mode = 'finished'
        box = tk.Frame(self.screen, bg=BG); box.pack(fill='both', expand=True)
        self.sparkles = tk.Canvas(box, height=90, bg=BG, highlightthickness=0); self.sparkles.pack(fill='x')
        folder = str(self.folder)
        if error is None and stage != 'complete_unconfirmed':
            word, colour = 'DONE!', GREEN
            lines = {'install': 'Magic Trick is installed. Turn your Chromatic off and on once, then enjoy.',
                     'uninstall': 'Magic Trick is uninstalled. Turn your Chromatic off and on. To go all the way back '
                                  'to stock, use ModRetro\'s updater next.',
                     'restore': 'Your backup is back on the Chromatic. Turn it off and on. To go all the way back to '
                                'stock, use ModRetro\'s updater next.'}[self.operation]
            extra = 'Your backup is in: ' + folder + '\nKeep it somewhere safe and private (not a cloud folder).'
            self.status_dot.configure(fg=GREEN); self.status_text.configure(text='ALL DONE')
        elif error is None:
            word, colour = 'ALMOST DONE', AMBER
            lines = ('Everything was written and checked, but the Chromatic didn\'t answer the final check. '
                     'Turn it off and on and make sure it starts.')
            extra = 'If it doesn\'t start, open the installer again and use Restore backup. Backup: ' + folder
            self.status_dot.configure(fg=AMBER); self.status_text.configure(text='CHECK YOUR CHROMATIC')
        else:
            word, colour = 'STOPPED', RED
            lines = 'It stopped before finishing: ' + error
            extra = ('Don\'t panic: your Chromatic can be recovered. Unplug the USB cable, wait 5 seconds, plug it '
                     'back in with the Chromatic on, then try again. If it keeps stopping, use Restore backup and '
                     'read "Installation & recovery". Your backup: ' + folder)
            self.status_dot.configure(fg=RED); self.status_text.configure(text='STOPPED')
        self.finish_colour = colour
        self.label(box, word, self.f_pixel_huge, colour).pack(anchor='w')
        self.label(box, lines, self.f_body_big, INK, wrap=820).pack(anchor='w', pady=(14, 0))
        self.label(box, extra, self.f_body, MUTED, wrap=820).pack(anchor='w', pady=(12, 0))
        row = tk.Frame(box, bg=BG); row.pack(anchor='w', pady=(22, 0))
        if Path(folder).is_dir():
            PixelButton(row, 'OPEN FOLDER', lambda: self.open_folder(folder), self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'COPY LOG', self.copy_log, self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        if error is not None:
            PixelButton(row, 'HELP', lambda: webbrowser.open(DOCS), self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'START OVER', self.show_home, self.f_pixel, 'primary').pack(side='left')

    def open_folder(self, folder):
        try:
            if sys.platform == 'win32':
                os.startfile(folder)
            elif sys.platform == 'darwin':
                subprocess.Popen(['open', folder])
            else:
                subprocess.Popen(['xdg-open', folder])
        except OSError:
            pass

    # ---------- plumbing ----------
    def log(self, text):
        if text and text.strip():
            self.history.append(text.rstrip())
            self.events.put(('log', text.rstrip()))

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append('\n'.join(self.history))

    def save_log(self, folder):
        """Keep a log on disk for every run, even one that stopped before the backup folder existed."""
        folder = Path(folder)
        target = folder / 'installer-log.txt' if folder.is_dir() else folder.parent / (folder.name + '-log.txt')
        try:
            target.write_text('\n'.join(self.history) + '\n', encoding='utf-8')
            self.log('Log saved: ' + str(target))
        except OSError:
            pass

    def poll(self):
        changed = False
        while not self.events.empty():
            kind, payload = self.events.get()
            if kind == 'log' and self.mode == 'running':
                self.output.configure(state='normal'); self.output.insert('end', payload + '\n')
                self.output.see('end'); self.output.configure(state='disabled')
                self.on_line(payload); changed = True
            elif kind == 'event':
                name, data = payload
                if name == 'plan':
                    self.plan = list(data['steps'])
                elif name == 'step':
                    if self.current and self.current != data['key']:
                        self.done_steps.add(self.current)
                    if data['key'] not in self.plan:
                        self.plan.append(data['key'])
                    self.current = data['key']; self.step_started = time.monotonic()
                    self.step_fraction = None; self.note = ''
                changed = True
            elif kind == 'finished':
                self.finished(payload)
        if changed and self.mode == 'running':
            self.draw_steps(); self.draw_progress()
        self.root.after(100, self.poll)

    def animate(self):
        """The little bits of life: blinking step marker, spinner, filling bar, end-screen sparkles."""
        self.tick += 1
        if self.mode == 'running':
            self.draw_steps(); self.draw_progress()
        elif self.mode == 'finished' and getattr(self, 'finish_colour', None) == GREEN:
            c = self.sparkles; c.delete('all')
            width = max(c.winfo_width(), 400)
            rng = random.Random(self.tick // 2)
            for _ in range(18):
                x, y, s = rng.randrange(0, width), rng.randrange(6, 80), rng.choice((4, 6, 8))
                c.create_rectangle(x, y, x + s, y + s, fill=rng.choice((ACCENT, GREEN, AMBER, LILAC, INK)), outline='')
        self.root.after(450, self.animate)

    def close(self):
        if self.busy:
            messagebox.showinfo('Still working', 'Keep the installer open until it says DONE or STOPPED.'); return
        self.root.destroy()


def self_test():
    core.check_images()
    assert core.known_running(core.TARGET)
    import esptool, serial   # noqa: F401  (bundled dependencies)
    out = subprocess.run(core.esptool_command() + ['version'], capture_output=True, text=True, timeout=60).stdout
    assert 'esptool' in out, 'bundled esptool did not run: ' + out
    assert (ASSETS / 'unscii-8.ttf').is_file() and (ASSETS / 'logo-mark.png').is_file(), 'installer artwork missing'
    fpga.run(['--version'], print)
    print('Magic Trick installer ' + core.INSTALLER_VERSION + ' (firmware ' + core.RELEASE + '): bundled firmware and dependencies OK')


def run_esptool(args):
    """Child-process entry point used by core.run_tool in the packaged app."""
    import esptool
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


def demo_work(speed=1.0, fail=False):
    """A pretend install for screenshots and UI checks: no device is touched."""
    pause = lambda s: time.sleep(s * speed)
    core.emit('plan', steps=['check', 'backup', 'mcu', 'fpga_chromagic', 'fpga_gg', 'fpga_gb', 'confirm'])
    core.emit('step', key='check'); print('Device versions: {"mcu": "v1.0.0-beta", "fpga": "50.1"}'); pause(2)
    core.emit('step', key='backup')
    for pct in range(0, 101, 4):
        print(f'Read {pct}% of the MCU flash.'); pause(0.4)
        if pct == 36:
            print('Read at 0x170000 failed (Corrupt data); reconnecting and retrying (attempt 1 of 6).')
    core.emit('step', key='mcu')
    for pct in range(0, 101, 5):
        print(f'Writing at 0x{0x260000 + pct * 0x3000:08x}... ({pct} %)'); pause(0.2)
    for key in ('fpga_chromagic', 'fpga_gg', 'fpga_gb'):
        core.emit('step', key=key)
        print('FPGA write at 0x000000, attempt 1 of 12…'); pause(3)
        if key == 'fpga_chromagic':
            print('Verification failed at 196609'); print('FPGA write at 0x000000, attempt 2 of 12…'); pause(3)
        if fail and key == 'fpga_gg':
            raise ValueError('The FPGA write did not finish. (demo)')
    core.emit('step', key='confirm'); pause(3)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--demo':
        import tempfile
        app = App(); app.consent = True
        folder = Path(tempfile.mkdtemp()) / 'Magic-Trick-backup-demo'
        folder.mkdir()
        speed = float(os.environ.get('DEMO_SPEED', '1'))
        app.root.after(800, lambda: app.run('install', lambda: demo_work(speed, '--fail' in sys.argv), folder))
        app.root.mainloop()
    elif len(sys.argv) > 1 and sys.argv[1] == '--esptool':
        run_esptool(sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == '--read-flash':
        # Child process used by core.read_flash: --read-flash PORT OFFSET SIZE OUT
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
