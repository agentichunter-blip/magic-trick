import contextlib, datetime, io, queue, sys, threading, tkinter as tk, urllib.request, webbrowser
from pathlib import Path
from tkinter import ttk, messagebox, filedialog
import core, fpga, hashlib

class App:
    def __init__(self):
        self.root = tk.Tk(); self.root.title('Magic Trick Installer'); self.root.geometry('760x640'); self.root.minsize(680,580)
        self.events = queue.Queue(); self.busy = False
        body = ttk.Frame(self.root, padding=28); body.pack(fill='both', expand=True)
        ttk.Label(body,text='MAGIC TRICK',font=('Helvetica',28,'bold')).pack(anchor='w')
        ttk.Label(body,text='A ChroMagic mod for the ModRetro Chromatic',font=('Helvetica',12)).pack(anchor='w',pady=(4,20))
        ttk.Label(body,text='Install artwork, gameplay previews, cheats, and recent games.\nInstalls directly from stock or compatible ChroMagic firmware.\nInstalls both MCU and FPGA firmware. No games or media included.',wraplength=680).pack(anchor='w')
        row = ttk.Frame(body); row.pack(fill='x',pady=16)
        self.port = tk.StringVar(); self.ports = ttk.Combobox(row,textvariable=self.port,state='readonly',width=45); self.ports.pack(side='left')
        self.refresh_button = ttk.Button(row,text='Refresh USB ports',command=self.refresh); self.refresh_button.pack(side='right')
        self.consent = tk.BooleanVar()
        ttk.Checkbutton(body,text='I backed up my SD card and will keep power and USB connected.',variable=self.consent).pack(anchor='w')
        self.button = ttk.Button(body,text='Back up device & install Magic Trick',command=self.start); self.button.pack(anchor='w',pady=14)
        self.output = tk.Text(body,height=14,wrap='word',state='disabled',font=('Courier',10)); self.output.pack(fill='both',expand=True)
        footer = ttk.Frame(body); footer.pack(fill='x',pady=(16,0))
        for label,url in [('Installation & recovery','https://github.com/agentichunter-blip/magic-trick/blob/main/docs/install-release.md'),('Source & credits','https://github.com/agentichunter-blip/magic-trick'),('Support on Ko-fi ↗','https://ko-fi.com/ryansag')]:
            ttk.Button(footer,text=label,command=lambda u=url:webbrowser.open(u)).pack(side='left',padx=(0,8))
        self.root.protocol('WM_DELETE_WINDOW',self.close); self.refresh(); self.root.after(100,self.poll)
    def refresh(self):
        values = core.device_ports(); self.ports['values'] = values
        self.port.set(values[0] if len(values)==1 else '')
    def log(self,text): self.events.put(('log',text))
    def start(self):
        if self.busy: return
        if not self.port.get() or not self.consent.get():
            messagebox.showinfo('Before installing','Select the Chromatic USB port and confirm your SD backup.'); return
        parent = filedialog.askdirectory(title='Choose where to keep your private device backup')
        if not parent: return
        if not messagebox.askyesno('Install Magic Trick?','This will back up your MCU and replace its application. Keep USB and power connected. Both MCU and FPGA firmware will be updated. Your SD card will not be written. The backup covers MCU only, not FPGA. Continue?'): return
        port = self.port.get(); folder = Path(parent)/('Magic-Trick-backup-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
        self.busy = True; self.button['state']='disabled'; self.refresh_button['state']='disabled'; self.ports['state']='disabled'
        def worker():
            try:
                self.log('Downloading pinned art31 release…')
                with urllib.request.urlopen(core.URL,timeout=60) as response:
                    data=response.read(core.SIZE+1)
                core.validate_image(data)
                self.log('Downloading pinned ChroMagic FPGA firmware…')
                with urllib.request.urlopen(fpga.FPGA_URL,timeout=60) as response:
                    fpga_data=response.read(16*1024*1024+1)
                if hashlib.sha256(fpga_data).hexdigest()!=fpga.FPGA_HASH:
                    raise ValueError('FPGA download checksum mismatch.')
                import tempfile
                with tempfile.TemporaryDirectory() as tmp:
                    firmware=Path(tmp)/'mcu.bin'; firmware.write_bytes(data)
                    fpga_image=Path(tmp)/'fpga.fs'; fpga_image.write_bytes(fpga_data)
                    class Sink(io.TextIOBase):
                        def write(_,s):
                            if s.strip(): self.log(s.strip())
                            return len(s)
                        def flush(_): pass
                    with contextlib.redirect_stdout(Sink()), contextlib.redirect_stderr(Sink()):
                        core.install(port,firmware,folder,self.log,fpga_image)
                self.log('Private backup saved to '+str(folder))
            except (Exception,SystemExit) as exc:
                self.log('STOPPED: '+str(exc)+'\nDo not erase the device. See Installation & recovery. Backup folder: '+str(folder))
            finally: self.events.put(('done',''))
        threading.Thread(target=worker,daemon=True).start()
    def poll(self):
        while not self.events.empty():
            kind,text=self.events.get()
            if kind=='done':
                self.busy=False; self.button['state']='normal'; self.refresh_button['state']='normal'; self.ports['state']='readonly'
            else:
                self.output['state']='normal'; self.output.insert('end',text+'\n'); self.output.see('end'); self.output['state']='disabled'
        self.root.after(100,self.poll)
    def close(self):
        if self.busy: messagebox.showinfo('Installation in progress','Keep the installer open until the operation finishes.'); return
        self.root.destroy()

if __name__=='__main__':
    if '--self-test' in sys.argv:
        assert core.compatible({'mcu':'v0.13.4-art31','fpga':'18.38','chromatic':'1.0.1-art31 (4.2)'})
        import esptool, serial
        fpga.run(['--version'],print)
        print('Magic Trick installer dependencies OK')
    else:
        app=App(); app.root.mainloop()
