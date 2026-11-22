import contextlib, datetime, io, json, math, os, queue, random, re, subprocess, sys, threading, time, webbrowser
from pathlib import Path
try:
    import tkinter as tk
    from tkinter import messagebox, filedialog, simpledialog, font as tkfont
except ImportError:   # command-line mode does not need a window
    tk = None
import core, fpga, library, cartridge, cart_backups
import cart_game_saves, cart_replacement

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
# The Cartridge tab stays hidden until cartridge support is announced.
CARTRIDGE = os.environ.get('MAGIC_TRICK_CARTRIDGE') != '0'   # the Cartridge tab (0 hides it)
CART_OPS = ('cart_read', 'cart_write', 'cart_restore', 'cart_save_snapshot')
STEPS.update({
    'cart_connect': ('CONNECTING', 'Opening the cartridge in your Chromatic.', 0.2),
    'cart_read_rom': ('READING ROM', 'Reading the full cartridge ROM from the Chromatic.', 1.8),
    'cart_read_save': ('READING SAVE', 'Reading the cartridge’s save memory.', 0.2),
    'cart_backup': ('SAVING BACKUP', 'Saving the cartridge backup on this computer.', 0.2),
    'cart_editor': ('OPENING EDITOR', 'Preparing the cartridge editor.', 0.1),
    'cart_check': ('CHECKING CART', 'Checking which ROM blocks match before writing.', 0.3),
    'cart_write': ('WRITING ROM', 'Erasing or programming the ROM blocks that need to change.', 2),
    'cart_verify': ('VERIFYING ROM', 'Checking the changed ROM blocks against the expected checksums.', 0.2),
    'cart_write_save': ('WRITING SAVE', 'Writing the save data to the cartridge’s save memory.', 0.2),
    'cart_release': ('CLOSING CONNECTION', 'Ending the current transfer session with the Chromatic.', 0.2),
    'cart_read_sd': ('READING SD SAVE', 'Reading the game’s save files from the microSD card.', 0.2),
    'cart_check_sd': ('CHECKING SD SAVE', 'Comparing the current SD card files with the save backup.', 0.2),
    'cart_clear_sd': ('CLEARING SD SAVE', 'Removing the backed-up save files so the new ROM starts fresh.', 0.2),
})
CART_WRITE_STEPS = ['cart_connect', 'cart_check', 'cart_write', 'cart_verify', 'cart_write_save', 'cart_release']
PLANS.update({'cart_read': ['cart_connect', 'cart_read_rom', 'cart_read_save', 'cart_release', 'cart_backup', 'cart_editor'],
              'cart_save_snapshot': ['cart_connect', 'cart_read_sd', 'cart_release', 'cart_backup', 'cart_editor'],
              'cart_write': CART_WRITE_STEPS, 'cart_restore': CART_WRITE_STEPS})
TITLES.update({'cart_read': 'READING CART', 'cart_write': 'WRITING CART', 'cart_restore': 'RESTORING CART',
               'cart_save_snapshot': 'BACKING UP SAVE'})
CART_ROUTES = [('wifi', 'WI-FI', 'Faster, no cable. Needs a Wi-Fi network (or the Chromatic\'s own hotspot).'),
               ('usb', 'USB', 'Plug in a USB cable. Slower; handy without Wi-Fi.')]


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
    """Older macOS file and folder pickers don't show their title, newer ones do: give macOS the text as the
    panel's message only, so it shows exactly once (otherwise the SD card picker that follows the games picker
    looks like the same one reopening, or the text appears twice)."""
    if sys.platform == 'darwin' and options.get('title'):
        options['message'] = options.pop('title')
    return options


def ask_directory(**options):
    return filedialog.askdirectory(**_picker_text(options))


def work_area_height():
    """Windows: the desktop's height above the taskbar, in the same pixels Tk uses; None elsewhere."""
    if sys.platform != 'win32':
        return None
    try:
        import ctypes
        from ctypes import wintypes
        rect = wintypes.RECT()
        if ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0):   # SPI_GETWORKAREA
            return rect.bottom - rect.top
    except Exception:
        pass
    return None


def documents_folder():
    """The Documents folder the file manager shows (on Windows it may be moved, e.g. into OneDrive)."""
    if sys.platform == 'win32':
        try:
            import ctypes
            from ctypes import wintypes

            class GUID(ctypes.Structure):
                _fields_ = [('Data1', wintypes.DWORD), ('Data2', wintypes.WORD), ('Data3', wintypes.WORD),
                            ('Data4', ctypes.c_ubyte * 8)]
            # FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}
            folder = GUID(0xFDD39AD0, 0x238F, 0x46AF, (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
            path = ctypes.c_wchar_p()
            if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(folder), 0, None, ctypes.byref(path)) == 0:
                try:
                    return Path(path.value)
                finally:
                    ctypes.windll.ole32.CoTaskMemFree(path)
        except Exception:
            pass
    return Path.home() / 'Documents'


CART_BACKUPS = documents_folder() / 'Magic Trick' / 'Cartridge backups'


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
        self.cart = None   # the cart read in the Cartridge tab
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
            tile.pack(fill='x', pady=3)
            words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=7)
            self.label(words, title, self.f_pixel, ACCENT if kind == 'primary' else INK).pack(anchor='w')
            self.label(words, text, self.f_body, MUTED, wrap=600).pack(anchor='w', pady=(3, 0))
            button = PixelButton(tile, 'START ▶', (lambda k=key: self.begin(k)), self.f_pixel, kind)
            button.pack(side='right', padx=16)
            self.tiles[key] = button
        tile = tk.Frame(self.screen, bg=PANEL, highlightthickness=2, highlightbackground=LINE)
        tile.pack(fill='x', pady=(8, 3))
        words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=7)
        self.label(words, 'SD CARD LIBRARY', self.f_pixel, LILAC).pack(anchor='w')
        self.label(words, 'Add games, box art and gameplay clips to your SD card. Doesn\'t change the firmware, '
                   'and no backup is needed.', self.f_body, MUTED, wrap=600).pack(anchor='w', pady=(3, 0))
        PixelButton(tile, 'OPEN ▶', self.show_library, self.f_pixel, 'secondary').pack(side='right', padx=16)
        if CARTRIDGE:
            tile = tk.Frame(self.screen, bg=PANEL, highlightthickness=2, highlightbackground=LINE)
            tile.pack(fill='x', pady=3)
            words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=7)
            self.label(words, 'CARTRIDGE', self.f_pixel, AMBER).pack(anchor='w')
            self.label(words, 'Back up a rewritable Game Boy cart (ModRetro carts, most flash carts) and put Game '
                       'Gear, Lynx and Game Boy games on it, with box art. No SD card needed to play them.', self.f_body, MUTED,
                       wrap=600).pack(anchor='w', pady=(3, 0))
            PixelButton(tile, 'OPEN ▶', self.show_cartridge, self.f_pixel, 'secondary').pack(side='right', padx=16)
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
            address = self.ask_text('Wi-Fi: step 1 of 2', 'On the Chromatic, open Settings, then Wi-Fi '
                                             'Files.\n\nType the address it shows.\nIt looks like 192.168.1.23',
                                             parent=self.root)
            if not address:
                return None, None
            code = self.ask_text('Wi-Fi: step 2 of 2', 'On the Chromatic, press A to see the WEB CODE.'
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

    def ask_text(self, title, prompt, parent=None):
        """A one-line question in the installer's own style (Tk's simpledialog fails on some Mac Tk builds)."""
        win = tk.Toplevel(self.root); win.title(title); win.configure(bg=BG); win.transient(self.root)
        win.resizable(False, False)
        self.label(win, prompt, self.f_body, INK, wrap=460).pack(anchor='w', padx=18, pady=(16, 8))
        value = tk.StringVar()
        entry = tk.Entry(win, textvariable=value, font=self.f_body_big, bg=PANEL, fg=INK, insertbackground=INK,
                         relief='flat', highlightthickness=2, highlightbackground=LINE, highlightcolor=ACCENT)
        entry.pack(fill='x', padx=18)
        answer = {'text': None}

        def finish(text):
            answer['text'] = text
            win.grab_release(); win.destroy()
        row = tk.Frame(win, bg=BG); row.pack(anchor='e', padx=18, pady=14)
        PixelButton(row, 'CANCEL', lambda: finish(None), self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'OK ▶', lambda: finish(value.get().strip() or None), self.f_pixel, 'primary').pack(side='left')
        entry.bind('<Return>', lambda e: finish(value.get().strip() or None))
        win.bind('<Escape>', lambda e: finish(None))
        win.protocol('WM_DELETE_WINDOW', lambda: finish(None))
        win.update_idletasks()
        x = self.root.winfo_rootx() + (self.root.winfo_width() - win.winfo_width()) // 2
        y = self.root.winfo_rooty() + 160
        win.geometry('+%d+%d' % (max(x, 0), max(y, 0)))
        entry.focus_set(); win.grab_set(); self.root.wait_window(win)
        return answer['text']

    def fit_window(self):
        """Grow the window to fit its content (high-DPI Windows needs more room), within the screen."""
        self.root.update_idletasks()
        want_w = max(self.root.winfo_width(), self.root.winfo_reqwidth())
        want_h = max(self.root.winfo_height(), self.root.winfo_reqheight())
        max_h = self.root.winfo_screenheight() - (150 if sys.platform == 'darwin' else 80)   # menu bar + Dock
        work = work_area_height()
        if work:   # Windows: above the taskbar, less the title bar (scaled for high-DPI screens)
            max_h = min(max_h, work - int(40 * self.root.winfo_fpixels('1i') / 96))
        if want_h > self.root.winfo_height() or want_w > self.root.winfo_width():
            self.root.geometry('%dx%d' % (want_w, min(want_h, max_h)))
        # A floor, not the content's size: the window can still be made smaller (screens keep their main
        # buttons at the bottom, so a short window clips the middle).
        self.root.minsize(min(820, want_w, self.root.winfo_screenwidth()), min(640, want_h, max_h))
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
        self.transfer_progress = None
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
                if operation not in LIBRARY + CART_OPS:
                    self.save_log(folder)
                self.events.put(('finished', error))
        threading.Thread(target=worker, daemon=True).start()

    def show_running(self):
        self.clear(); self.mode = 'running'
        self.status_dot.configure(fg=ACCENT)
        self.status_text.configure(text='WORKING…' if self.operation in LIBRARY else 'WORKING… KEEP IT PLUGGED IN')
        if self.operation in CART_OPS:
            self.status_text.configure(text='WORKING… LEAVE THE CART IN')
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
        if self.operation in CART_OPS:
            reminder = ('Keep Wi-Fi Files open on the Chromatic until this finishes.'
                        if getattr(self, 'cart_route', 'usb') == 'wifi' else
                        'Keep the Chromatic switched on and the USB cable plugged in until this finishes.')
        self.label(right, reminder, self.f_body, MUTED, wrap=460).pack(anchor='w', pady=(12, 0))
        self.details_toggle = self.label(self.screen, '▸ SHOW DETAILS', self.f_pixel, MUTED, cursor='hand2')
        self.details_toggle.pack(anchor='w', pady=(18, 4))
        self.details_toggle.bind('<Button-1>', lambda e: self.toggle_details())
        self.output = tk.Text(self.screen, height=10, wrap='word', bg=PANEL, fg=MUTED, insertbackground=INK,
                              relief='flat', font=('Courier', 10), highlightthickness=1, highlightbackground=LINE)
        self.output.insert('end', '\n'.join(self.history) + '\n'); self.output.configure(state='disabled')
        self.details_shown = False
        self.draw_steps(); self.draw_progress()
        self.fit_window()

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
        is_cart = self.operation in CART_OPS
        fraction, left = (self.step_fraction, None) if is_cart else self.progress()
        self.bar.delete('all')
        # The canvas can reach past the window's right edge when the window is narrower than the screen asks
        # for: draw only within what's visible.
        visible = self.root.winfo_width() - (self.bar.winfo_rootx() - self.root.winfo_rootx()) - 28
        width = max(min(self.bar.winfo_width(), visible), 240); blocks = 24; gap = 4
        size = (width - gap * (blocks - 1)) / blocks
        lit = int(fraction * blocks) if fraction is not None else 0
        pulse = self.tick * 2 % (blocks + 2) - 2 if fraction is None else None
        for i in range(blocks):
            x = i * (size + gap)
            if pulse is not None:
                colour = ACCENT if pulse <= i < pulse + 3 else PANEL
            elif i < lit:
                colour = ACCENT
            elif i == lit:
                colour = BLUE if self.tick % 2 == 0 else PANEL_2
            else:
                colour = PANEL
            self.bar.create_rectangle(x, 4, x + size, 30, fill=colour, outline=LINE)
        elapsed = int(time.monotonic() - self.started)
        clock = '%d:%02d' % divmod(elapsed, 60)
        if is_cart:
            phase = self.plan.index(self.current) + 1 if self.current in self.plan else 1
            stats = f'PHASE {phase}/{len(self.plan)} · {clock} ELAPSED'
            if self.transfer_progress is not None:
                done, total, units = self.transfer_progress
                stats += f'\n{done}/{total} {units}'
            self.stats.configure(text=stats)
        else:
            left_text = 'ABOUT %d MIN LEFT' % max(1, math.ceil(left)) if fraction < 0.95 else 'ALMOST THERE'
            if self.current == 'confirm' and time.monotonic() - self.step_started > 75:
                left_text = 'CHECKING…'
            self.stats.configure(text='%d%% · %s · %s' % (int(fraction * 100), clock, left_text))
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
        m = re.search(r'^Progress: (\d+) of (\d+)(?: (.*))?', line)
        if m:
            done, total = int(m.group(1)), int(m.group(2))
            self.step_fraction = done / max(total, 1)
            if self.operation in CART_OPS:
                self.transfer_progress = (done, total, m.group(3) or '')
            return
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

    # ---------- cartridge ----------
    def show_cartridge(self):
        if self.busy:
            return
        self.clear(); self.mode = 'cartridge'
        self.status_dot.configure(fg=AMBER); self.status_text.configure(text='CARTRIDGE')
        top = tk.Frame(self.screen, bg=BG); top.pack(fill='x', pady=(4, 6))
        self.label(top, 'CARTRIDGE', self.f_pixel_big, AMBER).pack(side='left')
        state = getattr(self, 'cart', None)
        # From the editor, BACK goes to the Cartridge start screen (connection, restore); from there, home.
        PixelButton(top, '◀ BACK', self.cart_forget if state else self.show_home, self.f_pixel,
                    'secondary').pack(side='right')
        PixelButton(top, 'BACKUP HISTORY', self.show_cart_backup_history, self.f_pixel,
                    'secondary').pack(side='right', padx=(0, 8))
        if state:
            return self.cart_editor(state)
        self.label(self.screen, 'Put Game Gear, Lynx and Game Boy games on a rewritable Game Boy cart: a ModRetro cart or '
                   'most flash carts. A Chromatic with Magic '
                   'Trick lists them under Games > Cartridge and plays them, even without an SD card. First the '
                   'whole cart is copied to this computer as a backup.', self.f_body_big, INK,
                   wrap=840).pack(anchor='w')
        self.cart_route = getattr(self, 'cart_route', 'wifi')
        self.cart_route_picked = getattr(self, 'cart_route_picked', False)
        self.label(self.screen, 'HOW TO REACH THE CHROMATIC', self.f_pixel, ACCENT).pack(anchor='w', pady=(14, 4))
        self.cart_route_rows = {}
        for key, name, note in CART_ROUTES:
            row = tk.Frame(self.screen, bg=BG, cursor='hand2'); row.pack(fill='x', pady=2)
            mark = self.label(row, '', self.f_pixel, INK); mark.pack(side='left')
            text = self.label(row, note, self.f_body, MUTED, wrap=560); text.pack(side='left', padx=(12, 0))
            for w in (row, mark, text):
                w.bind('<Button-1>', lambda e, k=key: self.pick_cart_route(k))
            self.cart_route_rows[key] = (mark, name)
        self.label(self.screen, 'BEFORE YOU START, ON THE CHROMATIC', self.f_pixel, ACCENT).pack(anchor='w', pady=(12, 4))
        self.cart_steps = self.label(self.screen, '', self.f_body, MUTED, wrap=820)
        self.cart_steps.pack(anchor='w')
        self.cart_route_poll = object()
        self.cart_route_auto(self.cart_route_poll)
        for title, text, button, command, kind in [
                ('READ CART', 'Back up the ROM and any save memory, then open the cartridge editor.',
                 'START ▶', self.begin_cart_read, 'primary'),
                ('RESTORE A CART BACKUP', 'Write a backup made here back onto the cart, exactly as it was.',
                 'CHOOSE ▶', self.begin_cart_restore, 'secondary')]:
            tile = tk.Frame(self.screen, bg=PANEL, highlightthickness=2,
                            highlightbackground=ACCENT if kind == 'primary' else LINE)
            tile.pack(fill='x', pady=(10, 0))
            words = tk.Frame(tile, bg=PANEL); words.pack(side='left', fill='x', expand=True, padx=16, pady=10)
            self.label(words, title, self.f_pixel, ACCENT if kind == 'primary' else INK).pack(anchor='w')
            self.label(words, text, self.f_body, MUTED, wrap=560).pack(anchor='w', pady=(5, 0))
            PixelButton(tile, button, command, self.f_pixel, kind).pack(side='right', padx=16)
        self.fit_window()

    def pick_cart_route(self, key):
        self.cart_route, self.cart_route_picked = key, True
        self.draw_cart_routes()

    def cart_route_auto(self, poll):
        """Until a route is picked by hand: USB when one Chromatic is plugged in, else Wi-Fi. Re-checked while the
        route screen shows, so plugging in the cable switches it."""
        if poll is not self.cart_route_poll or self.mode != 'cartridge' or getattr(self, 'cart', None):
            return   # another copy of this screen, or it's gone
        if not self.cart_route_picked and not self.busy:
            try:
                ports = core.device_ports()
            except Exception:
                ports = []
            self.cart_route = 'usb' if len(ports) == 1 else 'wifi'
        self.draw_cart_routes()
        self.root.after(1500, self.cart_route_auto, poll)

    def draw_cart_routes(self):
        for key, (mark, name) in self.cart_route_rows.items():
            mark.configure(text=('[■] ' if key == self.cart_route else '[ ] ') + name,
                           fg=ACCENT if key == self.cart_route else MUTED)
        last = ('3. Open Settings > Wi-Fi Files (USB TRANSFERS off). It shows an address and, after you press A, a '
                'WEB CODE: you\'ll type both here.' if self.cart_route == 'wifi' else
                '3. Press Menu and open the System tab: set WI-FI off and USB TRANSFERS on. Plug it into this '
                'computer with a USB cable.')
        self.cart_steps.configure(text='1. Put the cart in and switch the Chromatic on.\n2. Let it start the cart\'s '
                                  'Game Boy game or open the menu. Only the Game Boy side reaches the cart: if the '
                                  'cart starts a Game Gear or Lynx game, switch off and on holding B to stay in the '
                                  'menu.\n' + last)

    def cart_connection(self):
        """On the UI thread: what cart_open needs, or None. USB: the port. Wi-Fi: the address and WEB CODE
        (asked once; the pairing is kept while Wi-Fi Files stays open)."""
        if getattr(self, 'cart_route', 'wifi') == 'wifi':
            if getattr(self, 'cart_wifi', None):
                return ('wifi', None)
            address = self.ask_text('Wi-Fi: step 1 of 2', 'On the Chromatic, in a Game Boy game, open '
                                             'Settings, then Wi-Fi Files.\n\nType the address it shows.\nIt looks '
                                             'like 192.168.1.23', parent=self.root)
            if not address:
                return None
            code = self.ask_text('Wi-Fi: step 2 of 2', 'On the Chromatic, press A to see the WEB CODE.'
                                          '\n\nType its 6 numbers:', parent=self.root)
            return ('wifi', (address, code)) if code else None
        try:
            ports = core.device_ports()
        except Exception:
            ports = []
        if len(ports) != 1:
            messagebox.showinfo('Connect your Chromatic', 'Plug your Chromatic into this computer with a USB cable, '
                                'switch it on (hold B as it starts if the cart starts a Game Gear or Lynx game), then press '
                                'Menu and open the System tab: set WI-FI off and USB TRANSFERS on. Then try again.')
            return None
        return ('usb', ports[0])

    def cart_open(self, connection):
        """From the worker: (close function, cart access)."""
        kind, detail = connection
        if kind == 'wifi':
            try:
                if detail:
                    self.cart_wifi = library.WifiTarget(*detail)
                return (lambda: None), cartridge.CartWifi(self.cart_wifi)
            except Exception as exc:
                self.cart_wifi = None   # ask again next time
                if '(401)' in str(exc):
                    raise ValueError('The Chromatic didn\'t accept the pairing. Wi-Fi Files may have restarted: '
                                     'try again and type the WEB CODE it shows now.') from None
                raise
        usb = library.UsbTarget(detail)
        try:
            return usb.close, cartridge.CartUsb(usb)
        except Exception as exc:
            usb.close()
            if 'INVALID_STATE' in str(exc):
                raise ValueError(cartridge.GB_SIDE_ONLY)
            if 'NOT_SUPPORTED' in str(exc):
                raise ValueError('This cart\'s flash chip isn\'t one Magic Trick knows how to write. Nothing was '
                                 'changed.')
            if 'NOT_FOUND' in str(exc):
                raise ValueError('No cart answered. Check it\'s pushed all the way in, then try again.')
            raise

    def show_cart_backup_history(self):
        try:
            snapshots = cart_backups.list_save_snapshots(CART_BACKUPS)
        except OSError as exc:
            messagebox.showerror('Backup History', str(exc), parent=self.root)
            return

        win = tk.Toplevel(self.root)
        win.title('Backup History')
        win.configure(bg=BG)
        win.transient(self.root)
        if snapshots:
            frame = tk.Frame(win, bg=BG)
            frame.pack(fill='both', expand=True, padx=16, pady=(14, 4))
            box = tk.Listbox(frame, selectmode='browse', activestyle='none', font=self.f_body, width=68,
                             height=12, bg=PANEL, fg=INK, selectbackground=BLUE, selectforeground=INK,
                             relief='flat', highlightthickness=0, exportselection=False)
            vertical = tk.Scrollbar(frame, command=box.yview)
            horizontal = tk.Scrollbar(frame, orient='horizontal', command=box.xview)
            box.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
            horizontal.pack(side='bottom', fill='x')
            vertical.pack(side='right', fill='y')
            box.pack(side='left', fill='both', expand=True)
            for entry in snapshots:
                stamp = datetime.datetime.fromtimestamp(entry.modified_ns / 1_000_000_000)
                box.insert('end', '%s  ·  %s  ·  %s' % (stamp.strftime('%Y-%m-%d %H:%M:%S'),
                                                       entry.source, entry.path.name))
            box.selection_set(0)
            box.focus_set()
        else:
            self.label(win, 'No save snapshots yet.', self.f_body, MUTED).pack(anchor='w', padx=16, pady=(14, 6))

        def export():
            selection = box.curselection()
            if not selection:
                return
            snapshot = snapshots[selection[0]]
            chosen = filedialog.asksaveasfilename(**_picker_text({
                'title': 'Export this save snapshot', 'parent': win, 'initialfile': snapshot.path.name,
                'defaultextension': '.sav', 'filetypes': [('Game save', '*.sav')],
            }))
            if not chosen:
                return
            try:
                cart_backups.export_save(snapshot, chosen)
            except FileExistsError:
                messagebox.showerror('Export save', 'That file already exists. Choose another name.', parent=win)
            except OSError as exc:
                messagebox.showerror('Export save', str(exc), parent=win)
            else:
                messagebox.showinfo('Save exported', str(chosen), parent=win)

        actions = tk.Frame(win, bg=BG)
        actions.pack(fill='x', padx=16, pady=(6, 14))
        PixelButton(actions, 'CLOSE', win.destroy, self.f_pixel, 'secondary').pack(side='right')
        if snapshots:
            PixelButton(actions, 'EXPORT SAVE…', export, self.f_pixel).pack(side='right', padx=(0, 8))

    def begin_cart_read(self):
        if self.busy:
            return
        port = self.cart_connection()
        if not port:
            return

        def work():
            library.emit('step', key='cart_connect')
            finish, cart = self.cart_open(port)
            try:
                if not cart.save_size:
                    library.emit('plan', steps=[key for key in PLANS['cart_read'] if key != 'cart_read_save'])
                library.emit('step', key='cart_read_rom')
                image = cart.read(lambda done, total: library.progress(done // 1024, total // 1024, 'KiB read'))
                save = b''
                if cart.save_size:
                    library.emit('step', key='cart_read_save')
                    save = cart.read_save()
                library.emit('step', key='cart_release')
            finally:
                try:
                    cart.close()
                finally:
                    finish()
            library.emit('step', key='cart_backup')
            info = cartridge.parse(image)
            title = info.own_title or ('Magic-Trick-cart' if info.dedicated else 'cart')
            path = cart_backups.create_snapshot(CART_BACKUPS, image, save, title=title)
            print('Backup saved: %s%s' % (path, ' (+ %d KB save memory)' % (len(save) // 1024) if save else ''))
            library.emit('step', key='cart_editor')
            self.cart_loaded = {'image': image, 'info': info, 'backup': path, 'port': port, 'save': save,
                                'games': list(info.games), 'mode': self.cart_mode_of(info), 'own_art': info.own_art}
        self.cart_loaded = None
        self.cart_result = ''
        self.run('cart_read', work, None)

    @staticmethod
    def cart_mode_of(info):
        return 'keep' if info.own_title and not info.dedicated else 'magic'

    def cart_finished(self, error):
        if self.operation == 'cart_save_snapshot' and error is None:
            self.history.append('DONE: game save backup')
            return self.cart_finish_save_replacement()
        # A closing line, so a copied log says how it ended and how long it took.
        took = int(time.monotonic() - getattr(self, 'started', time.monotonic()))
        self.history.append('%s after %d:%02d: %s' % ('DONE' if error is None else 'STOPPED', took // 60, took % 60,
                                                     (self.cart_result or 'cart read') if error is None else error))
        if error is None and self.operation == 'cart_read' and self.cart_loaded:
            self.busy = True
            self.draw_steps()
            self.draw_progress()
            self.root.after_idle(self.open_loaded_cart)
            return
        self.clear(); self.mode = 'finished'; self.finish_colour = None
        box = tk.Frame(self.screen, bg=BG); box.pack(fill='both', expand=True)
        if error is None:
            word, colour = 'DONE!', GREEN
            lines = self.cart_result
            extra = ('On the Chromatic, press B to leave Wi-Fi Files, then open Games > Cartridge.'
                     if getattr(self, 'cart_route', 'usb') == 'wifi' else
                     'On the Chromatic, press Menu, turn off USB TRANSFERS in the System tab, then open Games > Cartridge.')
        else:
            word, colour = 'STOPPED', RED
            phase = STEPS[self.current][0].lower() if self.current in STEPS else 'starting'
            lines = f'Stopped while {phase}. {error}'
            extra = ('Read the cartridge again to see what is on it, or restore a backup. Backups are in ' +
                     str(CART_BACKUPS))
        self.status_dot.configure(fg=colour); self.status_text.configure(text='ALL DONE' if error is None else 'STOPPED')
        self.label(box, word, self.f_pixel_huge, colour).pack(anchor='w')
        self.label(box, lines, self.f_body_big, INK, wrap=820).pack(anchor='w', pady=(14, 0))
        self.label(box, extra, self.f_body, MUTED, wrap=820).pack(anchor='w', pady=(12, 0))
        row = tk.Frame(box, bg=BG); row.pack(anchor='w', pady=(22, 0))
        if CART_BACKUPS.is_dir():
            PixelButton(row, 'OPEN BACKUPS', lambda: self.open_folder(str(CART_BACKUPS)), self.f_pixel,
                        'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'COPY LOG', self.copy_log, self.f_pixel, 'secondary').pack(side='left', padx=(0, 10))
        PixelButton(row, 'BACK TO CARTRIDGE', self.show_cartridge, self.f_pixel, 'primary').pack(side='left')

    def open_loaded_cart(self):
        self.cart = self.cart_loaded
        self.busy = False
        self.show_cartridge()
        self.status_dot.configure(fg=GREEN)
        self.status_text.configure(text='BACKUP SAVED')

    # The editor: the cart's games, how full it gets, and WRITE.
    def cart_editor(self, state):
        info = state['info']
        size = '%g MB' % (info.capacity / 1048576)
        what = info.own_title or ('Magic Trick cart' if info.dedicated else 'no game of its own')
        head = tk.Frame(self.screen, bg=BG); head.pack(fill='x')
        self.label(head, '%s CART · %s' % (size, what.upper()), self.f_pixel, INK).pack(side='left')
        link = self.label(head, 'BACKUP FOLDER ▸', self.f_pixel, GREEN, cursor='hand2'); link.pack(side='right')
        link.bind('<Button-1>', lambda e: self.open_folder(str(state['backup'].parent)))
        row = tk.Frame(self.screen, bg=BG); row.pack(fill='x', side='bottom', pady=(6, 0))
        self.cart_write_button = PixelButton(row, 'WRITE TO CART ▶', self.begin_cart_write, self.f_pixel, 'primary')
        self.cart_write_button.pack(side='right')
        PixelButton(row, 'READ AGAIN', self.begin_cart_read, self.f_pixel, 'secondary').pack(side='right', padx=(0, 10))
        PixelButton(row, 'RESTORE BACKUP…', self.begin_cart_restore, self.f_pixel,
                    'secondary').pack(side='right', padx=(0, 10))
        self.label(self.screen, 'WHAT KIND OF CART', self.f_pixel, ACCENT).pack(anchor='w', pady=(8, 2))
        self.cart_modes = {}
        own = info.own_title
        options = ([('keep', 'KEEP IT', 'Keep %s first: it still plays on any Game Boy.' % own)] if own else []) + [
            ('replace', 'OWN GAME', 'Put a Game Boy game of your choice first%s, so any Game Boy plays it. More '
             'games can still go on for the Chromatic.' % (' instead of %s' % own if own else '')),
            ('magic', 'MAGIC TRICK CART', 'Only games for the Chromatic, behind a title card. With one game, it '
             'starts straight away.')]
        for key, name, text in options:
            row = tk.Frame(self.screen, bg=BG, cursor='hand2'); row.pack(fill='x', pady=1)
            mark = self.label(row, '', self.f_pixel, INK, width=20); mark.pack(side='left', anchor='n')
            note = self.label(row, text, self.f_body, MUTED, wrap=560); note.pack(side='left', padx=(8, 0))
            for w in (row, mark, note):
                w.bind('<Button-1>', lambda e, k=key: self.cart_set_mode(k))
            self.cart_modes[key] = (mark, name)
        self.cart_own_row = tk.Frame(self.screen, bg=BG)
        pick = tk.Frame(self.cart_own_row, bg=BG); pick.pack(fill='x')
        PixelButton(pick, 'CHOOSE GAME…', self.cart_pick_own, self.f_pixel, 'secondary').pack(side='left')
        self.cart_own_label = self.label(pick, '', self.f_body, INK, wrap=560)
        self.cart_own_label.pack(side='left', padx=(12, 0))
        clear = tk.Frame(self.cart_own_row, bg=BG, cursor='hand2'); clear.pack(fill='x', pady=(4, 0))
        self.cart_save_box = self.label(clear, '', self.f_pixel, INK, width=18)
        self.cart_save_box.pack(side='left', anchor='n')
        self.cart_clear_note = clear_note = self.label(clear, '', self.f_body, MUTED, wrap=520)
        clear_note.pack(side='left', padx=(8, 0))
        for w in (clear, self.cart_save_box, clear_note):
            w.bind('<Button-1>', lambda e: self.cart_toggle_clear())
        self.cart_games_title = self.label(self.screen, 'GAMES ON THE CART', self.f_pixel, ACCENT)
        self.cart_games_title.pack(anchor='w', pady=(8, 2))
        frame = tk.Frame(self.screen, bg=BG); frame.pack(fill='x')
        mono = 'Menlo' if sys.platform == 'darwin' else ('Consolas' if sys.platform == 'win32' else 'DejaVu Sans Mono')
        self.cart_list = tk.Listbox(frame, selectmode='extended', activestyle='none', font=(mono, 12), height=4,
                                    bg=PANEL, fg=INK, selectbackground=BLUE, selectforeground=INK, relief='flat',
                                    highlightthickness=0, exportselection=False)
        bar = tk.Scrollbar(frame, command=self.cart_list.yview); self.cart_list.configure(yscrollcommand=bar.set)
        self.cart_list.pack(side='left', fill='x', expand=True); bar.pack(side='right', fill='y')
        buttons = tk.Frame(self.screen, bg=BG); buttons.pack(fill='x', pady=(6, 0))
        for text, command in [('ADD GAMES…', self.cart_add_games), ('REPLACE ROM…', self.cart_replace_rom),
                              ('REMOVE', self.cart_remove),
                              ('FIND ART', self.cart_find_art), ('PICK ART…', self.cart_pick_art),
                              ('NO ART', self.cart_clear_art)]:
            PixelButton(buttons, text, command, self.f_pixel, 'secondary').pack(side='left', padx=(0, 8))
        self.cart_note = self.label(self.screen, '', self.f_body, MUTED, wrap=820)
        self.cart_note.pack(anchor='w', pady=(4, 0))
        self.label(self.screen, 'CART SPACE', self.f_pixel, ACCENT).pack(anchor='w', pady=(6, 2))
        self.cart_bar = tk.Canvas(self.screen, height=26, bg=BG, highlightthickness=0); self.cart_bar.pack(fill='x')
        legend = tk.Frame(self.screen, bg=BG); legend.pack(fill='x')
        self.cart_bar_text = self.label(legend, '', self.f_body, MUTED); self.cart_bar_text.pack(side='left')
        self.cart_legend = []
        for text, colour in [('BOX ART', LILAC), ('GAMES', ACCENT), (info.own_title or 'CART PROGRAM', FAINT)]:
            name = self.label(legend, text, self.f_body, MUTED); name.pack(side='right', padx=(0, 10))
            self.label(legend, '■', self.f_pixel, colour).pack(side='right', padx=(0, 4))
            self.cart_legend.append(name)
        self.label(self.screen, 'SAVES & STATES WITHOUT AN SD CARD', self.f_pixel, ACCENT).pack(anchor='w', pady=(6, 2))
        self.store_bar = tk.Canvas(self.screen, height=26, bg=BG, highlightthickness=0); self.store_bar.pack(fill='x')
        self.store_bar_text = self.label(self.screen, '', self.f_body, MUTED, wrap=820); self.store_bar_text.pack(anchor='w')
        for canvas in (self.cart_bar, self.store_bar):
            canvas.bind('<Configure>', lambda e: self.draw_cart())
        self.screen.after(50, self.draw_cart)
        self.fit_window()

    def cart_forget(self):
        self.cart = None
        self.show_cartridge()

    def cart_set_mode(self, mode):
        self.cart['mode'] = mode
        if mode == 'replace' and not self.cart.get('own_rom'):
            self.cart_pick_own()
        self.draw_cart()
        self.cart_drop_first_copies()

    def cart_pick_own(self):
        path = ask_open_filename(title='Choose the Game Boy or Game Boy Color game to put first on the cart',
                                 filetypes=[('Game Boy games', '*.gb *.gbc *.zip'), ('All files', '*')])
        if not path:
            return
        found = [(n, load) for n, load, _ in library.collect_games([path]) if n.lower().endswith(('.gb', '.gbc'))]
        if not found:
            messagebox.showinfo('Not a Game Boy game', 'Choose a .gb or .gbc file (or a zip with one in it).')
            return
        name, load = found[0]
        try:
            image, title, notes = cartridge.own_game(load(), self.cart_save_size())
        except cartridge.CartError as exc:
            messagebox.showinfo('That game can\'t go first on the cart', str(exc))
            return
        self.cart['own_rom'] = {'name': name, 'image': image, 'title': title, 'notes': notes}
        self.cart['own_art'] = None   # the old game's box art doesn't fit the new one
        self.cart['mode'] = 'replace'
        self.cart.setdefault('clear_save', True)
        self.draw_cart()
        self.cart_drop_first_copies()

    def cart_first(self):
        """The Game Boy game that goes first on the cart (KEEP IT or OWN GAME), or None."""
        state = self.cart
        if state['mode'] == 'replace':
            return (state.get('own_rom') or {}).get('image')
        if state['mode'] == 'keep':
            return state['image'][:state['info'].own_size]
        return None

    def cart_is_first(self, data):
        first = self.cart_first()
        return bool(first) and 0x150 <= len(data) <= len(first) and first[:len(data)] == data

    def cart_drop_first_copies(self):
        """An added game that is the cart's own game would be on the cart twice (and listed twice)."""
        copies = [g for g in self.cart['games'] if self.cart_is_first(g.data)]
        if copies:
            self.cart['games'] = [g for g in self.cart['games'] if g not in copies]
            self.draw_cart()
            messagebox.showinfo('Already the cart\'s own game', '%s is the game that goes first on the cart, so it '
                                'was taken out of the added games (it would be on the cart twice).' %
                                ', '.join(g.name for g in copies))

    def cart_toggle_clear(self):
        self.cart['clear_save'] = not self.cart.get('clear_save', True)
        self.draw_cart()

    def cart_save_size(self):
        return len(self.cart.get('save') or b'') or 32768

    def cart_target(self, state=None):
        """(image or None, error text): the cart as it would be written."""
        if state is None:
            state = self.cart
        info = state['info']
        try:
            if state['mode'] == 'magic':
                if not state['games']:
                    return None, 'Add at least one game.'
                return cartridge.build(info.capacity, state['games'], dedicated=True), ''
            if state['mode'] == 'replace':
                own = state.get('own_rom')
                if not own:
                    return None, 'Choose the Game Boy game to put first on the cart (CHOOSE GAME…).'
                return cartridge.build(info.capacity, state['games'], own_image=own['image'],
                                       own_art=state.get('own_art')), ''
            return cartridge.build(info.capacity, state['games'], own_image=state['image'],
                                   own_art=state.get('own_art')), ''
        except cartridge.CartFull as exc:
            return None, str(exc)
        except cartridge.CartError as exc:
            return None, str(exc)

    def draw_segments(self, canvas, parts, total, colours):
        canvas.delete('all')
        width = max(canvas.winfo_width(), 400)
        x, n = 0, 0
        for label, kind, size in parts:
            w = width * size / max(total, 1)
            if w <= 0:
                continue
            colour = colours.get(kind, PANEL)
            if kind == 'game':
                colour, n = (ACCENT, BLUE)[n % 2], n + 1   # tell neighbouring games apart
            canvas.create_rectangle(x, 2, min(x + w, width), 24, fill=colour, outline=BG)
            x += w
        if x < width:
            canvas.create_rectangle(x, 2, width, 24, fill=PANEL, outline=LINE)

    def draw_cart(self):
        if self.mode != 'cartridge' or not getattr(self, 'cart', None):
            return
        state = self.cart
        info = state['info']
        mode = state['mode']
        dedicated = mode == 'magic'
        for key, (mark, name) in getattr(self, 'cart_modes', {}).items():
            mark.configure(text=('[■] ' if key == mode else '[ ] ') + name, fg=ACCENT if key == mode else MUTED)
        own = state.get('own_rom')
        if mode == 'replace':
            self.cart_own_row.pack(fill='x', padx=(32, 0), pady=(2, 4), after=self.cart_modes['replace'][0].master)
            self.cart_own_label.configure(text=('%s (%s)' % (own['title'], own['name'])) if own else 'No game chosen yet.')
            on = state.get('clear_save', True)
            self.cart_save_box.configure(text=('[■] ' if on else '[ ] ') + 'BLANK SAVE', fg=ACCENT if on else MUTED)
            old = info.own_title or 'the old game'
            new = own['title'] if own else 'the new game'
            self.cart_clear_note.configure(
                text=('The cart\'s save chip still holds %s\'s save. Wipe it so %s starts with no save (the old one '
                      'is in your backup).' % (old, new)) if on else
                     ('Keep the save chip as it is: %s will see %s\'s save data, which it may read as damaged.' %
                      (new, old)))
        else:
            self.cart_own_row.pack_forget()
        self.cart_games_title.configure(text='MORE GAMES (FOR THE CHROMATIC)' if mode == 'replace' else
                                        'GAMES ON THE CART' if mode == 'magic' else 'ADDED GAMES (FOR THE CHROMATIC)')
        self.cart_list.delete(0, 'end')
        # The game that goes first stays on the cart: show it as a fixed first row.
        first = ((own['title'], len(own['image'])) if mode == 'replace' and own else
                 (info.own_title, info.own_size) if mode == 'keep' else None)
        self.cart_list_offset = 1 if first else 0
        if first:
            self.cart_list.insert('end', '%-10s %7s  %s  %s' % ('Game Boy', '%d KB' % (first[1] // 1024),
                                                              'ART' if state.get('own_art') else '   ',
                                                              '%s  (first on the cart, stays)' % first[0]))
            self.cart_list.itemconfig(0, fg=ACCENT)
        for g in state['games']:
            self.cart_list.insert('end', '%-10s %7s  %s  %s' % (cartridge.SYSTEM_NAMES[g.system],
                                                                '%d KB' % (len(g.data) // 1024),
                                                                'ART' if g.art else '   ', g.name))
        own_size = len(own['image']) if mode == 'replace' and own else info.own_size
        self.cart_legend[2].configure(text='TITLE CARD' if dedicated else (own['title'] if mode == 'replace' and own
                                                                          else info.own_title or 'CART PROGRAM'))
        parts, used = cartridge.usage(info.capacity, state['games'], own=own_size, dedicated=dedicated,
                                      own_art=state.get('own_art'))
        colours = {'own': FAINT, 'stub': FAINT, 'directory': LINE, 'game': ACCENT, 'art': LILAC}
        self.draw_segments(self.cart_bar, parts[:-1], info.capacity, colours)
        free = info.capacity - used
        self.cart_bar_text.configure(
            text='%d%% full · %.2f MB of %g MB used · %s' % (round(100 * used / info.capacity), used / 1048576,
                                                             info.capacity / 1048576,
                                                             '%d KB free' % (free // 1024) if free >= 0 else
                                                             '%d KB too much' % (-free // 1024)),
            fg=RED if free < 0 else MUTED)
        store_parts, store = cartridge.store_usage(state['games'])
        self.draw_segments(self.store_bar, [(n, 'save', b) for n, b in store_parts], cartridge.STORE_BYTES,
                           {'save': AMBER if store > cartridge.STORE_BYTES else GREEN})
        text = ('Up to %.2f of %.2f MB if every game saves and uses all 3 states%s' %
                (store / 1048576, cartridge.STORE_BYTES / 1048576,
                 ': without an SD card the newest saves could fail.' if store > cartridge.STORE_BYTES else
                 '. With an SD card in, saves go on the card.'))
        self.store_bar_text.configure(text=text, fg=AMBER if store > cartridge.STORE_BYTES else MUTED)
        target, problem = self.cart_target()
        changed = target is not None and target != state['image']
        if target is not None:
            steps = cartridge.plan(state['image'], target)
            moved = cartridge.plan_bytes(steps)
            note = ('Nothing to write yet.' if not changed else
                    'This write programs %d KiB. %s' % (moved // 1024, 'With one game, the Chromatic starts '
                                                               'straight into it.' if dedicated and
                                                               len(state['games']) == 1 else ''))
        else:
            note = problem
        warn = mode == 'replace' and own and own['notes']
        if warn and target is not None:
            note = ' '.join(own['notes']) + ' ' + note
        self.cart_note.configure(text=note, fg=RED if target is None else (AMBER if warn else MUTED))
        self.cart_write_button.set_enabled(changed)

    def cart_selected(self):
        offset = getattr(self, 'cart_list_offset', 0)
        return [self.cart['games'][i - offset] for i in self.cart_list.curselection() if i >= offset]

    def cart_add_games(self):
        paths = list(ask_open_filenames(title='Choose games for the cart', filetypes=[
            ('Games', '*.gb *.gbc *.gg *.lnx *.lyx *.zip'), ('All files', '*')]))
        if not paths:
            return
        have = {g.name.lower() for g in self.cart['games']}
        added, skipped, first = 0, [], []
        for name, load, _ in library.collect_games(paths):
            name = cartridge.short_name(name)
            if name.lower() in have:
                skipped.append(name)
                continue
            if Path(name).suffix.lower() in cartridge.SYSTEMS:
                data = load()
                if self.cart_is_first(data):
                    first.append(name)
                    continue
                self.cart['games'].append(cartridge.Game(name, data))
                have.add(name.lower()); added += 1
        self.draw_cart()
        if skipped:
            messagebox.showinfo('Already on the cart', 'These are already in the list:\n\n' + '\n'.join(skipped[:10]))
        if first:
            messagebox.showinfo('Already the cart\'s own game', '%s is the game that goes first on the cart, so it '
                                'isn\'t added again.' % ', '.join(first))

    def cart_own_selected(self):
        return getattr(self, 'cart_list_offset', 0) == 1 and 0 in self.cart_list.curselection()

    def cart_replace_rom(self):
        if self.busy:
            return
        chosen = self.cart_selected()
        own = self.cart_own_selected()
        if len(chosen) + own != 1:
            messagebox.showinfo('Replace ROM', 'Select one game on the cart first.')
            return
        path = ask_open_filename(title='Choose the replacement ROM', filetypes=[
            ('Games', '*.gb *.gbc *.gg *.lnx *.lyx'), ('All files', '*')])
        if not path:
            return
        path = Path(path)
        state = self.cart
        draft = dict(state)
        try:
            data = path.read_bytes()
            if own:
                draft['own_rom'] = cart_replacement.replace_own_game(state['info'], state['image'], path.name,
                                                                   data, self.cart_save_size())
                draft['mode'] = 'replace'
            else:
                game = chosen[0]
                replacement = cart_replacement.replace_game(game, path.name, data)
                draft['games'] = [replacement if g is game else g for g in state['games']]
                if self.cart_is_first(data):
                    raise cartridge.CartError('This ROM is already the first game on the cart.')
        except (OSError, cartridge.CartError) as exc:
            messagebox.showerror('Could not replace ROM', str(exc))
            return
        target, problem = self.cart_target(draft)
        if target is None:
            messagebox.showerror('Could not replace ROM', problem)
            return
        if own:
            save_file = state['backup'].with_suffix('.sav')
            if save_file.is_file():
                keep = messagebox.askyesnocancel('Carry over cartridge save?',
                    'The first game’s cartridge save is backed up at\n%s\n\nKeep the cartridge save for this build? '
                    'Choose No to clear the cartridge’s save memory or Cancel to leave the draft alone. '
                    'Leave the game and let the Chromatic finish saving before writing.' % save_file)
            else:
                keep = True
            if keep is not None:
                state.update(own_rom=draft['own_rom'], mode='replace', clear_save=not keep)
                self.draw_cart()
            return
        self.cart_back_up_replacement(game, replacement)

    def cart_back_up_replacement(self, game, replacement):
        original = next((g for g in self.cart['info'].games if g.name == game.name), None)
        if original is None or original.data == replacement.data:
            self.cart_stage_replacement(game, replacement)
            return
        on_sd = messagebox.askyesnocancel('Where is the save for ' + game.name + '?',
            'Choose Yes to back up the game’s files from the SD card in your Chromatic. '
            'Choose No if the game uses the Chromatic’s internal storage. Leave the game and enter the menu first.')
        if on_sd is None:
            return
        if not on_sd:
            if messagebox.askokcancel('Keep the internal save?',
                    'This firmware gives the installer access to SD cards and cartridge save memory. '
                    'The console’s internal game save can stay under the existing game name. '
                    'Keep that save in place and queue the new ROM?', icon='warning'):
                self.cart_stage_replacement(game, replacement)
            return
        port = self.cart_connection()
        if not port:
            return
        names = self.cart_save_names()
        self.cart_save_replacement = None

        def work():
            library.emit('step', key='cart_connect')
            target = self.cart_save_target(port)
            try:
                library.emit('step', key='cart_read_sd')
                files = cart_game_saves.snapshot_game_saves(target, game.name, game_names=names)
                library.emit('step', key='cart_release')
            finally:
                target.close()
            library.emit('step', key='cart_backup')
            backup = cart_backups.create_game_snapshot(CART_BACKUPS, game.name,
                                                      {Path(p).name: data for p, data in files.items()})
            print('Save backup: %s' % backup if backup else 'No save files found for ' + game.name)
            self.cart_save_replacement = game, replacement, files, backup
            library.emit('step', key='cart_editor')

        self.cart_result = ''
        self.run('cart_save_snapshot', work, None)

    def cart_save_target(self, port):
        route, detail = port
        if route == 'usb':
            return library.UsbTarget(detail)
        if detail:
            self.cart_wifi = library.WifiTarget(*detail)
        return self.cart_wifi

    def cart_save_names(self):
        state = self.cart
        names = {g.name for g in state['info'].games + state['games']}
        if state['info'].own_title:
            first = cart_replacement.own_save_entry(state['image'])
            if not first.startswith('.'):
                names.add(first)
        return names

    def cart_finish_save_replacement(self):
        game, replacement, files, backup = self.cart_save_replacement
        self.cart_save_replacement = None
        self.show_cartridge()
        if files:
            keep = messagebox.askyesnocancel('Carry over the save?',
                'Backed up this game’s save files at\n%s\n\nChoose Yes to keep the save in place, No to clear '
                'the backed-up saves and states after the ROM is written, or Cancel to discard the replacement. '
                'Check that the new build '
                'supports the old save format before loading it. Old save states may be incompatible.' % backup)
            if keep is None:
                return
        else:
            keep = True
            messagebox.showinfo('No existing save', 'No save files for %s were found on the SD card.' % game.name)
        self.cart_stage_replacement(game, replacement, clear_files=None if keep else files, backup=backup)

    def cart_stage_replacement(self, game, replacement, clear_files=None, backup=None):
        state = self.cart
        i = next(i for i, current in enumerate(state['games']) if current is game)
        state['games'][i] = replacement
        resets = state.setdefault('game_save_resets', {})
        if clear_files:
            resets[game.name] = (clear_files, backup)
        else:
            resets.pop(game.name, None)
        self.draw_cart()
        self.cart_list.selection_set(i + self.cart_list_offset)
        self.cart_list.see(i + self.cart_list_offset)

    def cart_own_for_art(self):
        """(name, ROM) of the game that goes first, for a box art lookup, or None."""
        state, info = self.cart, self.cart['info']
        if state['mode'] == 'replace' and state.get('own_rom'):
            return state['own_rom']['name'], state['own_rom']['image']
        if state['mode'] == 'keep' and info.own_title:
            data = state['image'][:info.own_size]
            return info.own_title + ('.gbc' if data[0x143] == 0xc0 else '.gb'), data
        return None

    def cart_remove(self):
        chosen = self.cart_selected()
        if self.cart_own_selected() and not chosen:
            messagebox.showinfo('Remove games', 'The game that goes first stays on the cart. To change it, choose '
                                'OWN GAME or MAGIC TRICK CART above.')
            return
        if not chosen:
            messagebox.showinfo('Remove games', 'Click a game in the list first (Shift or Ctrl/Cmd for several).')
            return
        self.cart['games'] = [g for g in self.cart['games'] if g not in chosen]
        resets = self.cart.get('game_save_resets', {})
        for game in chosen:
            resets.pop(game.name, None)
        self.draw_cart()

    def cart_pick_art(self):
        chosen = self.cart_selected()
        own = self.cart_own_selected() and not chosen and self.cart_own_for_art()
        if len(chosen) != 1 and not own:
            messagebox.showinfo('Pick box art', 'Click one game in the list first.')
            return
        path = ask_open_filename(title='Box art for ' + (own[0] if own else chosen[0].name),
                                 filetypes=[('Images', '*.png *.jpg *.jpeg *.bmp *.gif *.webp'), ('All files', '*')])
        if not path:
            return
        try:
            art = library.cover_bytes(Path(path).read_bytes())
            if own:
                self.cart['own_art'] = art
            else:
                chosen[0].art = art
        except Exception as exc:
            messagebox.showinfo('That image didn\'t work', 'It couldn\'t be read as an image: %s' % exc)
            return
        self.draw_cart()

    def cart_clear_art(self):
        if self.cart_own_selected():
            self.cart['own_art'] = None
        for g in self.cart_selected():
            g.art = None
        self.draw_cart()

    def cart_find_art(self):
        chosen = self.cart_selected()
        own_picked = self.cart_own_selected()
        todo = [g for g in (chosen or ([] if own_picked else self.cart['games'])) if not g.art]
        own = self.cart_own_for_art() if (own_picked or not chosen) and not self.cart.get('own_art') else None
        if own:   # the cart's own game too, so the menu shows it with a cover
            todo.append(cartridge.Game(own[0], own[1]))
        if not todo:
            messagebox.showinfo('Box art', 'Every game in the list already has box art.')
            return
        self.cart_note.configure(text='Looking for box art for %s…' % library.plural(len(todo), 'game'), fg=ACCENT)
        found = {}

        def work():
            try:
                found.update(library.covers_for([(g.name, g.data) for g in todo], log=lambda *_: None))
            except Exception as exc:
                found['_error'] = str(exc)
            self.events.put(('cart_art', None))

        def done():
            for g in todo:
                if g.name in found:
                    if own and g.name == own[0] and g not in self.cart['games']:
                        self.cart['own_art'] = found[g.name]
                    else:
                        g.art = found[g.name]
            self.draw_cart()
            missing = [g.name for g in todo if g.name not in found]
            if '_error' in found:
                messagebox.showinfo('Box art', 'Couldn\'t reach the box art catalogue: %s' % found['_error'])
            elif missing:
                messagebox.showinfo('Box art', 'No box art was found for:\n\n' + '\n'.join(missing[:10]) +
                                    '\n\nUse PICK ART… to choose an image yourself.')
        self.cart_art_done = done
        threading.Thread(target=work, daemon=True).start()

    def begin_cart_write(self):
        if self.busy:
            return
        target, problem = self.cart_target()
        if target is None:
            return
        state = self.cart
        steps = cartridge.plan(state['image'], target)
        moved = cartridge.plan_bytes(steps)
        erases = sum(step[0] == 'erase' for step in steps)
        before = {g.name for g in state['info'].games}
        after = {g.name for g in state['games']}
        lines = ['Add: ' + n for n in sorted(after - before)] + ['Remove: ' + n for n in sorted(before - after)]
        replaced = cart_replacement.changed_games(state['info'].games, state['games'])
        reset_choices = state.get('game_save_resets', {})
        game_resets = [(name, *reset_choices[name]) for name in replaced if name in reset_choices]
        lines += ['Replace ROM: ' + name + (' (start fresh using its save backup)' if name in reset_choices else '')
                  for name in replaced]
        own = state.get('own_rom')
        if state['mode'] == 'magic' and state['info'].own_title:
            lines.insert(0, 'Replace %s with the Magic Trick title card' % state['info'].own_title)
        clear = state['mode'] == 'replace' and state.get('clear_save', True)
        replace_own = state['mode'] == 'replace' and own is not None
        if replace_own:
            lines.insert(0, 'Put %s first%s' % (own['title'], ' (instead of %s)' % state['info'].own_title
                                                 if state['info'].own_title else ''))
            if clear:
                lines.insert(1, 'Wipe the cart\'s save chip (%s\'s save is in your backup)' % (state['info'].own_title or 'the old'))
        listing = '\n'.join('• ' + l for l in lines[:10]) + ('\n…and %d more' % (len(lines) - 10) if len(lines) > 10 else '')
        if not messagebox.askyesno('Write to the cart?', (listing + '\n\n' if listing else '') +
                                   'This erases %d flash blocks and programs %d KiB. Your backup is saved in:\n%s\n\n'
                                   'Keep the Chromatic on and connected until it says DONE. Continue?' %
                                   (erases, moved // 1024, state['backup'].parent), icon='warning'):
            return
        port = self.cart_connection()
        if not port:
            return
        save = b'\xff' * self.cart_save_size() if clear else None
        self.cart_write_run('cart_write', state['image'], target, port, check=True, save=save,
                            backup_own=replace_own, game_resets=game_resets)

    def begin_cart_restore(self):
        if self.busy:
            return
        path = ask_open_filename(title='Choose a cart backup', initialdir=str(CART_BACKUPS) if CART_BACKUPS.is_dir() else None,
                                 filetypes=[('Cart backups', '*.bin'), ('All files', '*')])
        if not path:
            return
        image = Path(path).read_bytes()
        info = cartridge.parse(image)
        what = info.own_title or ('a Magic Trick cart' if info.dedicated else 'a cart')
        save_path = Path(path).with_suffix('.sav')
        save = save_path.read_bytes() if save_path.is_file() else None
        if not messagebox.askyesno('Restore this backup?', '%s\n\n%g MB, %s, %s%s.\n\nEverything on the cart is '
                                   'replaced by this backup. Continue?' % (Path(path).name, len(image) / 1048576, what,
                                   library.plural(len(info.games), 'added game'),
                                   ', and its %d KB save memory' % (len(save) // 1024) if save else ''), icon='warning'):
            return
        port = self.cart_connection()
        if not port:
            return
        self.cart_write_run('cart_restore', None, image, port, check=False, save=save)

    @staticmethod
    def cart_summary(info, operation, rewritten, save_written):
        """The DONE screen's sentence: what is on the cart now."""
        games = library.plural(len(info.games), 'game')
        if info.dedicated:
            what = 'is a Magic Trick cart with %s%s' % (games, ', which starts at power-on' if len(info.games) == 1
                                                        else '')
        elif info.own_title:
            what = 'has ' + info.own_title + (' plus %s for the Chromatic' % games if info.games else
                                              ' and no added games')
        else:
            what = 'has %s for the Chromatic' % games
        if operation == 'cart_restore':
            text = ('The ROM already matched the backup. ' if not rewritten else
                    'Restored the ROM from the backup. ') + 'The cart %s.' % what
            return text + (' Its save was restored.' if save_written else '')
        now = 'is now ' + what[3:] if what.startswith('is ') else 'now ' + what
        text = 'ROM written. ' if rewritten else 'The ROM was already up to date. '
        return text + 'The cart %s.' % now + (' Its save was cleared.' if save_written else '')

    def cart_check_game_resets(self, port, game_resets, clear=False):
        files = self.cart_save_target(port)
        try:
            for name, expected, backup in game_resets:
                if clear:
                    try:
                        cart_game_saves.clear_game_saves(files, expected, game_name=name)
                    except (OSError, ValueError, library.UsbError, library.WifiError) as exc:
                        raise ValueError('The ROM was written, but starting fresh for %s stopped. '
                                         '%s Save backup: %s' % (name, exc, backup)) from exc
                elif cart_game_saves.snapshot_game_saves(files, name, game_names=self.cart_save_names()) != expected:
                    raise ValueError('The SD card save for %s changed after backup. Use REPLACE ROM again '
                                     'to back up the latest files before starting fresh.' % name)
        finally:
            files.close()

    def cart_write_run(self, operation, current, target, port, check, save=None, backup_own=False, game_resets=()):
        phases = list(CART_WRITE_STEPS)
        if backup_own:
            phases[2:2] = ['cart_read_save', 'cart_backup']
        if game_resets:
            phases = ['cart_check_sd', *phases, 'cart_clear_sd']

        def work():
            if phases != CART_WRITE_STEPS:
                library.emit('plan', steps=phases)
            if game_resets:
                library.emit('step', key='cart_check_sd')
                self.cart_check_game_resets(port, game_resets)
            library.emit('step', key='cart_connect')
            finish, cart = self.cart_open(port)
            write_save = save is not None and bool(cart.save_size)
            try:
                if cart.capacity != len(target):
                    raise ValueError('This cart holds %g MB, but the image is %g MB.' %
                                     (cart.capacity / 1048576, len(target) / 1048576))
                blocks = range(0, len(target), cartridge.ERASE_BLOCK)
                library.emit('step', key='cart_check')
                if check:
                    # A new full-cart backup pairs the cached image with fresh save memory, so verify all
                    # ROM blocks for that backup. Routine writes check their touched blocks and block 0.
                    steps = cartridge.plan(current, target)
                    touched = [s[1] for s in steps if s[0] != 'keep']
                    checked_blocks = blocks if backup_own else sorted(set([0] + touched))
                    if not cart.matches(current, checked_blocks):
                        raise ValueError('The cart isn\'t the one that was read (or it changed since). Nothing '
                                         'was written: press BACK TO CARTRIDGE, then READ AGAIN.')
                else:
                    # A restore: compare every block's checksum and rewrite only the ones that differ.
                    differ = [o for o in blocks if cart.crc(o, cartridge.ERASE_BLOCK) !=
                              cartridge.crc32(target[o:o + cartridge.ERASE_BLOCK])]
                    steps = [('erase', o, [o + b for b in range(0, cartridge.ERASE_BLOCK, cartridge.BLOCK)
                                           if target[o + b:o + b + cartridge.BLOCK] != b'\xff' * cartridge.BLOCK])
                             for o in differ]
                    touched = differ
                skipped = set()
                if not touched:
                    skipped.update(('cart_write', 'cart_verify'))
                if not write_save:
                    skipped.add('cart_write_save')
                if backup_own and not cart.save_size:
                    skipped.update(('cart_read_save', 'cart_backup'))
                if skipped:
                    library.emit('plan', steps=[key for key in phases if key not in skipped])
                if backup_own and cart.save_size:
                    library.emit('step', key='cart_read_save')
                    fresh_save = cart.read_save()
                    if len(fresh_save) != cart.save_size:
                        raise ValueError('The cartridge returned an incomplete save backup. The ROM was not written.')
                    library.emit('step', key='cart_backup')
                    backup = cart_backups.create_snapshot(CART_BACKUPS, current, fresh_save,
                                                           title=cartridge.parse(current).own_title)
                    print('Backup saved: %s' % backup)
                    self.cart.update(backup=backup, save=fresh_save)
                if touched:
                    library.emit('step', key='cart_write')
                    total, done = max(1, cartridge.plan_bytes(steps)), 0
                    shown = lambda d, t: library.progress(d // 1024, t // 1024, 'KiB written')
                    for step in steps:
                        if step[0] == 'keep':
                            continue
                        if step[0] == 'erase':
                            print('Erasing 64 KiB at 0x%x' % step[1])
                            cart.erase(step[1])
                        done = cart.program_blocks(target, step[2], shown, done, total)
                    library.emit('step', key='cart_verify')
                    if not cart.matches(target, touched):
                        raise ValueError('The cart doesn\'t match what was written. Write it again, or restore your '
                                         'backup.')
                if write_save:
                    library.emit('step', key='cart_write_save')
                    print('Writing %d KiB of save memory' % (cart.save_size // 1024))
                    saved_bytes = bytes(save[:cart.save_size]).ljust(cart.save_size, b'\xff')
                    cart.write_save(saved_bytes)
                library.emit('step', key='cart_release')
            finally:
                try:
                    games = cart.close()
                finally:
                    finish()
            info = cartridge.parse(target)
            self.cart_result = self.cart_summary(info, operation, bool(touched), write_save)
            if save is not None and not write_save:
                self.cart_result += ' The Chromatic did not report any cartridge save memory.'
            if self.cart:
                self.cart.update({'image': target, 'info': info, 'games': list(info.games),
                                  'mode': self.cart_mode_of(info), 'own_rom': None, 'own_art': info.own_art})
                self.cart.pop('game_save_resets', None)
                if write_save:
                    self.cart['save'] = saved_bytes
            print('The Chromatic reads %s games on the cart.' % games)
            if game_resets:
                library.emit('step', key='cart_clear_sd')
                try:
                    self.cart_check_game_resets(port, game_resets, clear=True)
                except (OSError, ValueError, library.UsbError, library.WifiError) as exc:
                    raise ValueError('ROM verified on the cartridge. SD save cleanup stopped. %s' % exc) from exc
                self.cart_result += ' The selected SD saves were backed up and cleared.'
        self.cart_result = ''
        self.run(operation, work, None)

    # ---------- finished ----------
    def finished(self, error):
        self.busy = False
        if self.operation in CART_OPS:
            return self.cart_finished(error)
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
            line = text.rstrip()
            # Progress lines drive the bar; the saved/copied log keeps one per 10% (and the last).
            m = re.match(r'Progress: (\d+) of (\d+)', line)
            if m:
                done, total = int(m.group(1)), max(int(m.group(2)), 1)
                step = done * 10 // total
                if step != getattr(self, 'progress_logged', None) or done >= total:
                    self.progress_logged = step
                    self.history.append(line)
            else:
                self.history.append(line)
            self.events.put(('log', line))

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
                    self.transfer_progress = None
                changed = True
            elif kind == 'pick':
                self.pick_games(*payload)
            elif kind == 'finished':
                self.finished(payload)
            elif kind == 'cart_art':
                if getattr(self, 'cart_art_done', None):
                    self.cart_art_done()
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
