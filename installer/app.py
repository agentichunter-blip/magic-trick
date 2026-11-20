import contextlib, datetime, io, json, math, os, queue, random, re, subprocess, sys, threading, time, webbrowser
from pathlib import Path
try:
    import tkinter as tk
    from tkinter import messagebox, filedialog, simpledialog, font as tkfont
except ImportError:   # command-line mode does not need a window
    tk = None
import core, fpga, library

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
    'fpga_lynx': ('ATARI LYNX CORE', 'Writing the Atari Lynx core to the FPGA.', 2.5),
    'fpga_gb': ('GAME BOY CORE', 'Writing the Game Boy and Game Boy Color core to the FPGA.', 2.5),
    'table': ('SWITCH OVER', 'Switching the MCU over to the new firmware.', 0.3),
    'mcu_restore': ('RESTORE MCU', 'Putting your saved MCU flash back. This takes a few minutes.', 6),
    'confirm': ('RESTART', 'Restarting your Chromatic and checking that it starts up.', 1),
    'lib_scan': ('READING CARD', 'Looking at what\'s already on your SD card.', 0.5),
    'lib_copy': ('COPYING GAMES', 'Copying your games into the right folders.', 3),
    'art_match': ('FINDING BOX ART', 'Matching your games to box art.', 1),
    'art_write': ('ADDING BOX ART', 'Downloading box art and adding it to your card.', 3),
    'vid_write': ('ADDING CLIPS', 'Shrinking your clips to fit the screen and adding them to your card.', 5),
    'lib_remove': ('REMOVING GAMES', 'Deleting the games you picked, with their box art and clips.', 1),
}
PLANS = {
    'install': ['check', 'backup', 'mcu', 'fpga_gg', 'fpga_lynx', 'fpga_gb', 'table', 'confirm'],
    'uninstall': ['check', 'backup', 'fpga_chromagic', 'table', 'confirm'],
    'restore': ['check', 'precopy', 'fpga_chromagic', 'mcu_restore', 'confirm'],
    'games': ['lib_scan', 'lib_copy'],
    'covers': ['lib_scan', 'art_match', 'art_write'],
    'previews': ['lib_scan', 'vid_write'],
    'remove': ['lib_scan', 'lib_remove'],
}
TITLES = {'install': 'INSTALLING', 'uninstall': 'UNINSTALLING', 'restore': 'RESTORING',
          'games': 'ADDING GAMES', 'covers': 'BOX ART', 'previews': 'ADDING CLIPS',
          'remove': 'REMOVING GAMES'}
LIBRARY = ('games', 'covers', 'previews', 'remove')
# How the library reaches the SD card: key, label, speed note.
ROUTES = [('folder', 'CARD READER', 'Fastest. Take the SD card out of the Chromatic and put it in this computer.'),
          ('wifi', 'WI-FI', 'No cable needed. On the Chromatic, open Settings, then Wi-Fi Files.'),
          ('usb', 'USB (SLOW)', 'Very slow. Fine for a few covers, but not for games or clips.')]
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


def _picker_text(options):
    """macOS file and folder pickers don't show their title, so put it in the panel's message too
    (otherwise the SD card picker that follows the games picker looks like the same one reopening)."""
    if sys.platform == 'darwin' and options.get('title'):
        options['message'] = options['title']
    return options


def ask_directory(**options):
    return filedialog.askdirectory(**_picker_text(options))


def ask_open_filenames(**options):
    return filedialog.askopenfilenames(**_picker_text(options))


def ask_open_filename(**options):
    return filedialog.askopenfilename(**_picker_text(options))


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
        for text, url in [('Installation & recovery', DOCS), ('Source & credits', SOURCE), ('Support my work', KOFI)]:
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
        self.label(self.screen, 'Game Boy, Game Boy Color, Game Gear and Atari Lynx on one Chromatic. What would you like to do?',
                   self.f_body_big, INK, wrap=840).pack(anchor='w', pady=(4, 8))
        note = tk.Frame(self.screen, bg=BG); note.pack(fill='x')
        self.label(note, '! BETA: installed at your own risk. Magic Trick isn\'t made or supported by ModRetro and may '
                   'void your warranty. Read "Installation & recovery" first, and keep the backup folder from your '
                   'first install somewhere safe (not a cloud folder).', self.f_body, AMBER, wrap=840).pack(anchor='w')
        if sys.platform == 'win32':
            self.label(note, 'Windows: before your first install, install ModRetro\'s Chromatic Firmware Updater, run '
                       'its USB driver setup once, then close it completely.', self.f_body, MUTED,
                       wrap=840).pack(anchor='w', pady=(6, 0))
        self.consent = getattr(self, 'consent', False)
        self.check = self.label(self.screen, '', self.f_pixel, INK, cursor='hand2')
        self.check.pack(anchor='w', pady=(10, 8))
        self.check.bind('<Button-1>', lambda e: self.toggle_consent())
        self.draw_consent()
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
        tile = tk.Frame(self.screen, bg=PANEL, highlightthickness=2, highlightbackground=LINE)
        tile.pack(fill='x', pady=(12, 4))
        words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=10)
        self.label(words, 'SD CARD LIBRARY', self.f_pixel, LILAC).pack(anchor='w')
        self.label(words, 'Add games, box art and gameplay clips to your SD card. Doesn\'t change the firmware, '
                   'and no backup is needed.', self.f_body, MUTED, wrap=600).pack(anchor='w', pady=(5, 0))
        PixelButton(tile, 'OPEN ▶', self.show_library, self.f_pixel, 'secondary').pack(side='right', padx=16)
        self.refresh_device()
        self.fit_window()

    # ---------- SD card library ----------
    def show_library(self):
        if self.busy:
            return
        self.clear(); self.mode = 'library'
        self.status_dot.configure(fg=LILAC); self.status_text.configure(text='SD CARD LIBRARY')
        self.route = getattr(self, 'route', 'folder')
        self.replace = getattr(self, 'replace', False)
        top = tk.Frame(self.screen, bg=BG); top.pack(fill='x', pady=(4, 6))
        self.label(top, 'SD CARD LIBRARY', self.f_pixel_big, LILAC).pack(side='left')
        PixelButton(top, '◀ BACK', self.show_home, self.f_pixel, 'secondary').pack(side='right')
        self.label(self.screen, 'Add games, box art and gameplay clips to your SD card. Everything goes in the '
                   'right folder with the right name. Nothing on the card is deleted unless you use REMOVE GAMES.',
                   self.f_body_big, INK, wrap=840).pack(anchor='w')
        self.label(self.screen, '1. HOW TO REACH YOUR SD CARD', self.f_pixel, ACCENT).pack(anchor='w', pady=(14, 4))
        self.route_rows = {}
        for key, name, note in ROUTES:
            row = tk.Frame(self.screen, bg=BG, cursor='hand2'); row.pack(fill='x', pady=2)
            mark = self.label(row, '', self.f_pixel, INK); mark.pack(side='left')
            text = self.label(row, note, self.f_body, AMBER if key == 'usb' else MUTED, wrap=520)
            text.pack(side='left', padx=(12, 0))
            for w in (row, mark, text):
                w.bind('<Button-1>', lambda e, k=key: self.pick_route(k))
            self.route_rows[key] = (mark, name)
        self.draw_routes()
        self.label(self.screen, '2. WHAT TO DO', self.f_pixel, ACCENT).pack(anchor='w', pady=(12, 4))
        for key, title, text, buttons in [
                ('games', 'ADD GAMES', 'Pick game files (.gb .gbc .gg .lnx) or a folder of them. Zip files are '
                 'fine. No need to sort them: each game is put in its system\'s folder for you.',
                 [('FILES ▶', 'files'), ('FOLDER ▶', 'folder')]),
                ('covers', 'GET BOX ART', 'Press START. It finds box art for every game on the card that has '
                 'none yet. ' + library.ART_CREDIT, [('START ▶', None)]),
                ('previews', 'ADD GAMEPLAY CLIPS', 'Pick video clips, 10 seconds or longer, or a folder of them. '
                 'Each clip needs the same name as its game, for example Tetris.mp4.',
                 [('FILES ▶', 'files'), ('FOLDER ▶', 'folder')]),
                ('remove', 'REMOVE GAMES', 'Pick games on the card to delete, with their box art and clips. Their '
                 'saves and save states stay on the card.', [('CHOOSE ▶', None)])]:
            tile = tk.Frame(self.screen, bg=PANEL, highlightthickness=2, highlightbackground=LINE)
            tile.pack(fill='x', pady=4)
            words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=8)
            self.label(words, title, self.f_pixel, INK).pack(anchor='w')
            self.label(words, text, self.f_body, MUTED, wrap=450).pack(anchor='w', pady=(3, 0))
            if key == 'previews':
                link = self.label(words, 'HOW DO I GET CLIPS? ▸', self.f_pixel, ACCENT, cursor='hand2')
                link.pack(anchor='w', pady=(4, 0))
                link.bind('<Button-1>', lambda e: self.clips_help())
            for label, pick in reversed(buttons):
                PixelButton(tile, label, (lambda k=key, p=pick: self.begin_library(k, p)), self.f_pixel,
                            'primary' if key == 'covers' else 'secondary').pack(side='right', padx=(0, 16))
        self.replace_box = self.label(self.screen, '', self.f_pixel, MUTED, cursor='hand2')
        self.replace_box.pack(anchor='w', pady=(8, 0))
        self.replace_box.bind('<Button-1>', lambda e: self.toggle_replace())
        self.draw_replace()
        self.fit_window()

    def clips_help(self):
        if messagebox.askyesno('Getting gameplay clips', (
                'Most people use EmuMovies, a website with gameplay clips for thousands of games. Magic Trick '
                'can\'t include them, so you download them yourself:\n\n'
                '1. Make an account at emumovies.com.\n'
                '2. Find the "Video Snaps (SQ)" pack for each system you play: Game Boy, Game Boy Color, '
                'Game Gear or Atari Lynx.\n'
                '3. Download every part of the pack (files ending in .part1.rar, .part2.rar and so on) into one '
                'folder.\n'
                '4. Open the first part to unzip it. Windows 11 can do this by itself. On a Mac, use a free app '
                'such as The Unarchiver.\n'
                '5. Come back here, press FOLDER next to ADD GAMEPLAY CLIPS, and pick the unzipped folder.\n\n'
                'The clips are already named like the games, so they match by themselves. Clips for games that '
                'aren\'t on your card are skipped. Your own recordings work too: 10 seconds or longer, named '
                'like the game.\n\nOpen emumovies.com now?')):
            webbrowser.open('https://emumovies.com')

    def pick_route(self, key):
        self.route = key
        self.draw_routes()

    def draw_routes(self):
        for key, (mark, name) in self.route_rows.items():
            on = key == self.route
            mark.configure(text=('(•) ' if on else '( ) ') + name.ljust(12), fg=GREEN if on else INK)

    def toggle_replace(self):
        self.replace = not self.replace
        self.draw_replace()

    def draw_replace(self):
        self.replace_box.configure(text=('[X]' if self.replace else '[ ]') + ' REPLACE BOX ART AND CLIPS ALREADY THERE', fg=GREEN if self.replace else MUTED)

    def begin_library(self, operation, pick):
        if self.busy:
            return
        sources = []
        if operation not in ('covers', 'remove'):
            if operation == 'games':
                kinds = [('Games', '*.gb *.gbc *.gg *.lnx *.lyx *.zip'), ('All files', '*')]
            else:
                kinds = [('Video clips', ' '.join('*' + e for e in sorted(library.VIDEO_EXTENSIONS))), ('All files', '*')]
            what = 'games' if operation == 'games' else 'gameplay clips'   # used in the picker titles
            if pick == 'folder':
                chosen = ask_directory(title='Choose the folder with your ' + what)
                sources = [chosen] if chosen else []
            else:
                sources = list(ask_open_filenames(title='Choose your ' + what, filetypes=kinds))
            if not sources:
                return
            if operation == 'previews' and not library.find_ffmpeg():
                messagebox.showinfo('Video converter missing', 'This installer is missing its video converter '
                                    '(FFmpeg). Download the installer again, or install FFmpeg on this computer.')
                return
        opener, folder = self.library_target(sources)
        if not opener:
            return
        replace = self.replace

        def work():
            target = opener()
            try:
                if operation == 'remove':
                    library.emit('step', key='lib_scan')
                    games = library.card_games(target)
                    if not games:
                        raise ValueError('There are no games on this card.')
                    chosen = self.ask_games(games)
                    self.library_result = (library.remove_games(target, chosen) if chosen else
                                           'Nothing was removed.')
                elif operation == 'games':
                    self.library_result = library.add_games(target, sources)
                elif operation == 'covers':
                    self.library_result = library.get_covers(target, replace)
                else:
                    self.library_result = library.add_previews(target, sources, replace)
                if replace and not target.can_replace:
                    self.library_result += ' (Wi-Fi can\'t replace files, so existing ones were kept.)'
            finally:
                target.close()
        self.library_result = ''
        self.run(operation, work, folder)

    def ask_games(self, games):
        """From the worker thread: show the game picker and wait for the user's choice ([] = cancel)."""
        answer = {'paths': [], 'done': threading.Event()}
        self.events.put(('pick', (games, answer)))
        answer['done'].wait()
        return answer['paths']

    def pick_games(self, games, answer):
        """Pick games to delete: a filter box and a multi-select list, then a confirmation."""
        rows = sorted(((library.SYSTEM_NAMES.get(system, system), Path(path).name, path)
                       for path, _, system in games), key=lambda r: (r[0], r[1].lower()))
        win = tk.Toplevel(self.root); win.title('Remove games'); win.configure(bg=BG); win.transient(self.root)
        self.label(win, 'Pick the games to delete. Click to select or unselect; you can pick several.',
                   self.f_body, INK, wrap=620).pack(anchor='w', padx=16, pady=(14, 6))
        search = tk.StringVar()
        entry = tk.Entry(win, textvariable=search, font=self.f_body, bg=PANEL, fg=INK, insertbackground=INK,
                         relief='flat', highlightthickness=2, highlightbackground=LINE, highlightcolor=ACCENT)
        entry.pack(fill='x', padx=16)
        frame = tk.Frame(win, bg=BG); frame.pack(fill='both', expand=True, padx=16, pady=8)
        box = tk.Listbox(frame, selectmode='multiple', activestyle='none', font=self.f_body, width=70, height=18,
                         bg=PANEL, fg=INK, selectbackground=RED, selectforeground=INK, relief='flat',
                         highlightthickness=0, exportselection=False)
        bar = tk.Scrollbar(frame, command=box.yview); box.configure(yscrollcommand=bar.set)
        box.pack(side='left', fill='both', expand=True); bar.pack(side='right', fill='y')
        count = self.label(win, '', self.f_pixel, MUTED); count.pack(anchor='w', padx=16)
        picked, shown = set(), []

        def refresh(*_):
            words = search.get().lower().split()
            shown[:] = [r for r in rows if all(w in (r[0] + ' ' + r[1]).lower() for w in words)]
            box.delete(0, 'end')
            for i, (system, name, path) in enumerate(shown):
                box.insert('end', '%-15s %s' % (system, name))
                if path in picked:
                    box.selection_set(i)
            count.configure(text='%d SELECTED' % len(picked), fg=RED if picked else MUTED)

        def toggled(_):
            visible = {r[2] for r in shown}
            chosen = {shown[i][2] for i in box.curselection()}
            picked.difference_update(visible - chosen); picked.update(chosen)
            count.configure(text='%d SELECTED' % len(picked), fg=RED if picked else MUTED)

        def finish(paths):
            answer['paths'] = paths; answer['done'].set()
            win.grab_release(); win.destroy()

        def delete():
            if not picked:
                return
            names = [r[1] for r in rows if r[2] in picked]
            listing = '\n'.join('• ' + n for n in names[:8]) + ('\n…and %d more' % (len(names) - 8) if len(names) > 8 else '')
            if messagebox.askyesno('Delete %s?' % library.plural(len(names), 'game'),
                                   'These will be deleted from the SD card, with their box art and clips:\n\n' +
                                   listing + '\n\nThis can\'t be undone. Saves and save states stay on the card.',
                                   icon='warning', parent=win):
                finish([r[2] for r in rows if r[2] in picked])

        search.trace_add('write', refresh); box.bind('<<ListboxSelect>>', toggled)
        row = tk.Frame(win, bg=BG); row.pack(anchor='e', padx=16, pady=(4, 14))
        PixelButton(row, 'CANCEL', lambda: finish([]), self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'DELETE ▶', delete, self.f_pixel, 'danger').pack(side='left')
        win.protocol('WM_DELETE_WINDOW', lambda: finish([]))
        refresh(); entry.focus_set(); win.grab_set()

    def library_target(self, sources=()):
        """Ask how to reach the card. Returns (a function that opens it, a local folder or None).
        sources: the games or clips just picked, so the card can't be mistaken for their folder."""
        if self.route == 'folder':
            where = {'initialdir': '/Volumes'} if sys.platform == 'darwin' and Path('/Volumes').is_dir() else {}
            chosen = ask_directory(title='Now choose your SD card (the card itself, not a folder on it)', **where)
            if not chosen:
                return None, None
            path = Path(chosen)
            root = path.parent if path.name.upper() == 'CHROMAGIC' else path
            for source in sources:
                picked = Path(source)
                picked = picked if picked.is_dir() else picked.parent
                if root.resolve() == picked.resolve() or picked.resolve() in root.resolve().parents:
                    messagebox.showinfo('That\'s not the SD card', 'That\'s the folder you picked your files from, '
                                        'not the SD card:\n\n' + str(root) + '\n\nStart again and, when asked for '
                                        'the SD card, choose the card itself (it\'s listed under Locations on a Mac, '
                                        'or as a drive on Windows).')
                    return None, None
            if not any(c.name.upper() == 'CHROMAGIC' for c in root.iterdir() if c.is_dir()) and \
                    not messagebox.askyesno('Is this the right card?', 'This doesn\'t look like a Chromatic SD card '
                                            '(there\'s no CHROMAGIC folder):\n\n' + str(root) + '\n\nChoose Yes only if '
                                            'this really is the card from your Chromatic.'):
                return None, None
            return (lambda: library.FolderTarget(root)), root
        if self.route == 'wifi':
            address = simpledialog.askstring('Wi-Fi: step 1 of 2', 'On the Chromatic, open Settings, then Wi-Fi '
                                             'Files.\n\nType the address it shows.\nIt looks like 192.168.1.23',
                                             parent=self.root)
            if not address:
                return None, None
            code = simpledialog.askstring('Wi-Fi: step 2 of 2', 'On the Chromatic, press A to see the WEB CODE.'
                                          '\n\nType its 6 numbers:', parent=self.root)
            if not code:
                return None, None
            return (lambda: library.WifiTarget(address, code)), None
        try:
            ports = core.device_ports()
        except Exception:
            ports = []
        if len(ports) != 1:
            messagebox.showinfo('Connect your Chromatic', 'Plug your Chromatic into this computer with a USB cable '
                                'and switch it on. Then try again.')
            return None, None
        if not messagebox.askyesno('USB is very slow', 'Copying over USB takes a long time:\n\n• one box art: '
                                   'about 8 seconds\n• one game: about 5 minutes\n• one gameplay clip: about 12 '
                                   'minutes\n\nFor more than a few files, use a card reader or Wi-Fi.\n\nBefore you '
                                   'go on, on the Chromatic:\n1. Turn Wi-Fi off.\n2. Turn USB TRANSFERS on.\n\n'
                                   'Continue over USB?'):
            return None, None
        port = ports[0]
        return (lambda: library.UsbTarget(port)), None

    def fit_window(self):
        """Grow the window to fit its content (high-DPI Windows needs more room), within the screen."""
        self.root.update_idletasks()
        want_w = max(self.root.winfo_width(), self.root.winfo_reqwidth())
        want_h = max(self.root.winfo_height(), self.root.winfo_reqheight())
        max_h = self.root.winfo_screenheight() - 80
        if want_h > self.root.winfo_height() or want_w > self.root.winfo_width():
            self.root.geometry('%dx%d' % (want_w, min(want_h, max_h)))
        self.root.minsize(min(want_w, self.root.winfo_screenwidth()), min(want_h, max_h))
        if sys.platform.startswith('linux'):
            # Tiling window managers squeeze a normal window into a tile that clips the lower screens.
            # A fixed size and the dialog window type make most of them (Hyprland, sway, i3) float it.
            self.root.resizable(False, False)
            try:
                self.root.wm_attributes('-type', 'dialog')
            except tk.TclError:
                pass
            # Without an explicit position some window managers put a floating window at 0,0,
            # under a desktop's top bar. Centre it instead.
            self.root.update_idletasks()
            w, h = self.root.winfo_width(), self.root.winfo_height()
            x = max(0, (self.root.winfo_screenwidth() - w) // 2)
            y = max(0, (self.root.winfo_screenheight() - h) // 2)
            self.root.geometry('%dx%d+%d+%d' % (w, h, x, y))

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
            dot, text = GREEN, 'CHROMATIC FOUND  ' + Path(ports[0]).name
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
        parent = ask_directory(title='Choose where to keep your private device backup')
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
        chosen = ask_open_filename(
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
                if operation not in LIBRARY:
                    self.save_log(folder)
                self.events.put(('finished', error))
        threading.Thread(target=worker, daemon=True).start()

    def show_running(self):
        self.clear(); self.mode = 'running'
        self.status_dot.configure(fg=ACCENT)
        self.status_text.configure(text='WORKING…' if self.operation in LIBRARY else 'WORKING… KEEP IT PLUGGED IN')
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
        reminder = {'folder': 'Leave the SD card in the reader until it says DONE.',
                    'wifi': 'Keep the Chromatic on Wi-Fi Files until it says DONE.'}.get(
            getattr(self, 'route', None) if self.operation in LIBRARY else None,
            'Keep the Chromatic switched on and the USB cable plugged in until it says DONE.')
        self.label(right, reminder, self.f_body, MUTED, wrap=460).pack(anchor='w', pady=(12, 0))
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
        left_text = 'ABOUT %d MIN LEFT' % max(1, math.ceil(left)) if fraction < 0.95 else 'ALMOST THERE'
        if self.current == 'confirm' and time.monotonic() - self.step_started > 75:
            left_text = 'CHECKING…'
        self.stats.configure(text='%d%% · %d:%02d · %s' % (int(fraction * 100), elapsed // 60, elapsed % 60, left_text))
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
        if line.startswith('Reading CHROMAGIC/'):
            self.note = 'Reading your SD card. Over Wi-Fi or USB this can take a minute.'; return
        m = re.search(r'^Progress: (\d+) of (\d+)', line)
        if m:
            self.step_fraction = int(m.group(1)) / max(int(m.group(2)), 1); return
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
        if 'hasn\'t confirmed its version yet' in line:
            self.note = ('Waiting for the Chromatic to answer. This can take a few minutes the first time; '
                         'everything has already been written and checked.')
            self.step_fraction = None
            return
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
        if self.operation in LIBRARY:
            word, colour = ('DONE!', GREEN) if error is None else ('STOPPED', RED)
            if error is None:
                lines = self.library_result
                extra = {'folder': 'Next: eject the SD card, put it back in the Chromatic and open BACKUPS.',
                         'wifi': 'Next: press B on the Chromatic to close Wi-Fi Files, then open BACKUPS.',
                         'usb': 'Next: turn USB TRANSFERS off on the Chromatic, then open BACKUPS.'}[self.route]
            else:
                lines = 'It stopped before finishing: ' + error
                extra = ('Nothing is lost: everything copied before it stopped is complete. Press BACK TO LIBRARY '
                         'and run it again to carry on. Files already on the card are skipped.')
            if self.operation == 'covers':
                extra += '\n' + library.ART_CREDIT
            extra += '\nCOPY LOG lists every file, and anything that wasn\'t found.'
            self.status_dot.configure(fg=colour); self.status_text.configure(text='ALL DONE' if error is None else 'STOPPED')
        elif error is None and stage != 'complete_unconfirmed':
            word, colour = 'DONE!', GREEN
            lines = {'install': 'Magic Trick is installed. Turn your Chromatic off and on once, then enjoy.',
                     'uninstall': 'Magic Trick is uninstalled. Turn your Chromatic off and on. To go all the way back '
                                  'to stock, use ModRetro\'s updater next.',
                     'restore': 'Your backup is back on the Chromatic. Turn it off and on. To go all the way back to '
                                'stock, use ModRetro\'s updater next.'}[self.operation]
            if self.operation == 'restore':
                extra = ('A copy of what was on the Chromatic before the restore is in: ' + folder +
                         '\nYour original backup stays where it was. Keep both private (not in a cloud folder).')
            else:
                extra = 'Your backup is in: ' + folder + '\nKeep it somewhere safe and private (not a cloud folder).'
            self.status_dot.configure(fg=GREEN); self.status_text.configure(text='ALL DONE')
        elif error is None:
            word, colour = 'ALMOST DONE', AMBER
            lines = ('Everything was written and verified. One last step: turn the Chromatic off and on, '
                     'wait for its menu, then press CHECK AGAIN.')
            extra = ('If it doesn\'t start, open the installer again and use Restore backup. Backup: ' + folder)
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
        if error is None and stage != 'complete_unconfirmed' and self.operation == 'install':
            support = self.label(box, 'Enjoying Magic Trick? It\'s free forever. If it made your Chromatic better, '
                                 'support my work: ko-fi.com/ryansag', self.f_body, ACCENT, wrap=820, cursor='hand2')
            support.pack(anchor='w', pady=(12, 0))
            support.bind('<Button-1>', lambda e: webbrowser.open(KOFI))
        self.recheck_note = self.label(box, '', self.f_body, AMBER, wrap=820)
        self.recheck_note.pack(anchor='w', pady=(8, 0))
        row = tk.Frame(box, bg=BG); row.pack(anchor='w', pady=(22, 0))
        if error is None and stage == 'complete_unconfirmed':
            PixelButton(row, 'CHECK AGAIN', self.check_again, self.f_pixel, 'primary').pack(side='left', padx=(0, 10))
        if self.folder and Path(folder).is_dir():
            PixelButton(row, 'OPEN FOLDER', lambda: self.open_folder(folder), self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'COPY LOG', self.copy_log, self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        if error is not None:
            PixelButton(row, 'HELP', lambda: webbrowser.open(DOCS), self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        if self.operation in LIBRARY:
            PixelButton(row, 'BACK TO LIBRARY', self.show_library, self.f_pixel, 'primary').pack(side='left')
            return
        PixelButton(row, 'START OVER', self.show_home, self.f_pixel,
                    'secondary' if error is None and stage == 'complete_unconfirmed' else 'primary').pack(side='left')

    def check_again(self):
        if self.busy:
            return
        self.busy = True
        self.recheck_note.configure(text='Asking the Chromatic… (up to a minute)', fg=MUTED)
        folder = self.folder

        def worker():
            try:
                ok, found = core.recheck(folder)
            except (OSError, ValueError) as exc:
                ok, found = False, str(exc)
            self.events.put(('recheck', (ok, found)))
        threading.Thread(target=worker, daemon=True).start()

    def rechecked(self, result):
        ok, found = result
        if ok:
            self.log('Confirmed after the power cycle: ' + json.dumps(found))
            self.save_log(self.folder)
            self.finished(None)
        elif isinstance(found, dict):
            self.recheck_note.configure(fg=AMBER, text='The Chromatic answered, but with other versions: MCU %s, FPGA %s. '
                                        'Try Uninstall or Restore backup again.' % (found.get('mcu'), found.get('fpga')))
        else:
            self.recheck_note.configure(fg=AMBER, text='No answer yet. Make sure it is switched on, plugged in and showing '
                                        'its menu, then press CHECK AGAIN.')

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
            elif kind == 'pick':
                self.pick_games(*payload)
            elif kind == 'finished':
                self.finished(payload)
            elif kind == 'recheck':
                self.busy = False
                if self.mode == 'finished':
                    self.rechecked(payload)
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
    import certifi   # noqa: F401  (cover downloads need its certificates in the packaged app)
    from PIL import Image
    png = io.BytesIO(); Image.new('RGB', (40, 30), (200, 30, 30)).save(png, 'PNG')
    assert len(library.cover_bytes(png.getvalue())) == library.COVER_BYTES, 'cover conversion failed'
    ffmpeg = library.find_ffmpeg()
    assert ffmpeg, 'bundled FFmpeg missing'
    out = subprocess.run([ffmpeg, '-hide_banner', '-version'], capture_output=True, text=True, timeout=60).stdout
    assert out.startswith('ffmpeg version'), 'bundled FFmpeg did not run: ' + out[:200]
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
    core.emit('plan', steps=['check', 'backup', 'mcu', 'fpga_chromagic', 'fpga_gg', 'fpga_lynx', 'fpga_gb', 'confirm'])
    core.emit('step', key='check'); print('Device versions: {"mcu": "v1.0.0-beta", "fpga": "50.1"}'); pause(2)
    core.emit('step', key='backup')
    for pct in range(0, 101, 4):
        print(f'Read {pct}% of the MCU flash.'); pause(0.4)
        if pct == 36:
            print('Read at 0x170000 failed (Corrupt data); reconnecting and retrying (attempt 1 of 6).')
    core.emit('step', key='mcu')
    for pct in range(0, 101, 5):
        print(f'Writing at 0x{0x260000 + pct * 0x3000:08x}... ({pct} %)'); pause(0.2)
    for key in ('fpga_chromagic', 'fpga_gg', 'fpga_lynx', 'fpga_gb'):
        core.emit('step', key=key)
        print('FPGA write at 0x000000, attempt 1 of 12…'); pause(3)
        if key == 'fpga_chromagic':
            print('Verification failed at 196609'); print('FPGA write at 0x000000, attempt 2 of 12…'); pause(3)
        if fail and key == 'fpga_gg':
            raise ValueError('The FPGA write did not finish. (demo)')
    core.emit('step', key='confirm'); pause(3)


if __name__ == '__main__':
    for stream in (sys.stdout, sys.stderr):   # a cp1252 console or redirect must not crash on device output
        try:
            stream.reconfigure(errors='replace')
        except (AttributeError, ValueError, OSError):
            pass
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
    elif len(sys.argv) == 3 and sys.argv[1] == '--status':
        # Read-only diagnostic: app.py --status PORT. Prints what the console says and the parsed versions.
        try:
            print('VERSIONS:', json.dumps(core.version(sys.argv[2], trace=lambda s: print(s, flush=True))))
        except ValueError as exc:
            print('NO VERSION:', exc)
    elif len(sys.argv) > 1 and sys.argv[1] == '--library':
        try:
            sys.exit(library.main(sys.argv[2:]))
        except (ValueError, OSError) as exc:
            print('ERROR: ' + str(exc), flush=True)
            sys.exit(1)
    elif '--self-test' in sys.argv:
        self_test()
    elif len(sys.argv) == 4 and sys.argv[1] in ('--install', '--uninstall', '--restore'):
        # Command-line mode for hardware testing: app.py --install|--uninstall PORT BACKUP_DIR,
        # or app.py --restore path/to/mcu-backup.bin NEW_FOLDER
        try:
            getattr(core, sys.argv[1][2:])(sys.argv[2], sys.argv[3], print)
        except (ValueError, OSError) as exc:
            print('ERROR: ' + str(exc), flush=True)
            sys.exit(1)
    else:
        App().root.mainloop()
