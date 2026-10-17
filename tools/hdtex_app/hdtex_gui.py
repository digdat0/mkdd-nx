"""MKDD-NX HD Textures: a window around tools/hdtex_pack.py for people who don't use a terminal.

Pick the Dolphin pack's GM4E01 folder and an output folder, press Convert, walk away: the
computer is kept awake until it is done, and the window then says where hd-textures.bin is
and where it goes on the SD card. Packaged as a single .exe / .app by tools/hdtex_app/build_app.py.

    py -3 tools/hdtex_app/hdtex_gui.py      (run from source)
"""
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

if not getattr(sys, "frozen", False):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import hdtex_pack  # noqa: E402

APP = "MKDD-NX HD Textures"
OUT_NAME = "hd-textures.bin"
PACK_URL = "https://sites.google.com/view/mkddhd/home-page"


class KeepAwake:
    """Stops the computer sleeping while a conversion runs (the screen may still turn off)."""

    def __init__(self):
        self.proc = None

    def __enter__(self):
        if os.name == "nt":
            import ctypes
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001)  # CONTINUOUS | SYSTEM_REQUIRED
        elif sys.platform == "darwin":
            try:
                self.proc = subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())])
            except OSError:
                pass
        return self

    def __exit__(self, *exc):
        if os.name == "nt":
            import ctypes
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
        elif self.proc:
            self.proc.terminate()


def fmt_time(s):
    s = int(s)
    return f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"


def open_folder(path):
    if os.name == "nt":
        os.startfile(path)
    elif sys.platform == "darwin":
        subprocess.Popen(["open", str(path)])
    else:
        subprocess.Popen(["xdg-open", str(path)])


class App:
    def __init__(self, root):
        self.root = root
        self.pack = None
        self.out_dir = None
        self.worker = None
        self.cancel = threading.Event()
        self.events = queue.Queue()

        root.title(APP)
        root.resizable(False, False)
        f = ttk.Frame(root, padding=16)
        f.grid(sticky="nsew")
        f.columnconfigure(1, weight=1)

        intro = ttk.Frame(f)
        intro.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 12))
        ttk.Label(intro, text="Converts the MKDD HD texture pack (made for Dolphin) into one file for MKDD-NX on Switch.\n"
                  "Download the English pack from its site (Mega link there) and extract it first:",
                  wraplength=560, justify="left").pack(anchor="w")
        link = tk.Label(intro, text=PACK_URL, fg="#0066cc", cursor="hand2", font=("", 9, "underline"))
        link.pack(anchor="w", pady=(2, 0))
        link.bind("<Button-1>", lambda e: webbrowser.open(PACK_URL))

        ttk.Label(f, text="1. Texture pack", font=("", 10, "bold")).grid(row=1, column=0, sticky="w")
        self.pack_var = tk.StringVar(value="Choose the pack's GM4E01 folder")
        ttk.Label(f, textvariable=self.pack_var, wraplength=400, foreground="gray").grid(row=1, column=1, sticky="w", padx=8)
        self.pack_btn = ttk.Button(f, text="Choose...", command=self.choose_pack)
        self.pack_btn.grid(row=1, column=2)

        ttk.Label(f, text="2. Output folder", font=("", 10, "bold")).grid(row=2, column=0, sticky="w", pady=(8, 0))
        self.out_var = tk.StringVar(value="Choose where to save " + OUT_NAME + " (about 800 MB)")
        ttk.Label(f, textvariable=self.out_var, wraplength=400, foreground="gray").grid(row=2, column=1, sticky="w", padx=8, pady=(8, 0))
        self.out_btn = ttk.Button(f, text="Choose...", command=self.choose_out)
        self.out_btn.grid(row=2, column=2, pady=(8, 0))

        self.go = ttk.Button(f, text="3. Convert", command=self.start, state="disabled")
        self.go.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(16, 8), ipady=4)

        self.bar = ttk.Progressbar(f, length=560, mode="determinate")
        self.bar.grid(row=4, column=0, columnspan=3, sticky="ew")
        self.status = tk.StringVar(value="")
        ttk.Label(f, textvariable=self.status, wraplength=560, justify="left").grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 0))

        btns = ttk.Frame(f)
        btns.grid(row=6, column=0, columnspan=3, sticky="e", pady=(12, 0))
        self.open_btn = ttk.Button(btns, text="Open output folder", command=lambda: open_folder(self.out_dir))
        self.cancel_btn = ttk.Button(btns, text="Cancel", command=self.ask_cancel)

        root.protocol("WM_DELETE_WINDOW", self.close)

    def update_go(self):
        self.go.configure(state="normal" if self.pack and self.out_dir else "disabled")

    def choose_pack(self):
        d = filedialog.askdirectory(title="Choose the pack's GM4E01 folder")
        if not d:
            return
        self.root.configure(cursor="watch")
        self.root.update()
        try:
            pack = hdtex_pack.find_pack(d)
        finally:
            self.root.configure(cursor="")
        if not pack:
            messagebox.showerror(APP, "No HD textures found in that folder.\n\nChoose the GM4E01 folder of the "
                                 "extracted pack: it holds files named tex1_..._.png (Dolphin would load it "
                                 "from Load/Textures/GM4E01).")
            return
        self.pack = pack
        self.pack_var.set(str(pack))
        self.update_go()

    def choose_out(self):
        d = filedialog.askdirectory(title="Choose where to save " + OUT_NAME)
        if not d:
            return
        out = Path(d)
        if self.pack and (out == self.pack or self.pack in out.parents):
            messagebox.showerror(APP, "Choose an output folder outside the texture pack.")
            return
        self.out_dir = out
        self.out_var.set(str(out / OUT_NAME))
        self.update_go()

    def start(self):
        out = self.out_dir / OUT_NAME
        if out.exists() and not messagebox.askyesno(APP, f"{out} already exists. Replace it?"):
            return
        for w in (self.go, self.pack_btn, self.out_btn):
            w.configure(state="disabled")
        self.open_btn.pack_forget()
        self.cancel_btn.configure(state="normal")
        self.cancel_btn.pack(side="right")
        self.cancel.clear()
        self.bar.configure(value=0, maximum=1)
        self.status.set("Reading the texture pack...")
        self.worker = threading.Thread(target=self.run, args=(self.pack, out), daemon=True)
        self.worker.start()
        self.root.after(200, self.poll)

    def run(self, pack, out):
        def progress(done, total, size, el):
            self.events.put(("progress", done, total, size, el))
        try:
            with KeepAwake():
                r = hdtex_pack.convert(pack, out, progress=progress, cancel=self.cancel)
            self.events.put(("done", r))
        except hdtex_pack.PackError as e:
            self.events.put(("error", str(e)))
        except Exception as e:  # anything unexpected: still tell the user instead of hanging
            self.events.put(("error", f"{type(e).__name__}: {e}"))

    def poll(self):
        last = None
        try:
            while True:
                ev = self.events.get_nowait()
                if ev[0] == "progress":
                    last = ev
                else:
                    self.finish(ev)
                    return
        except queue.Empty:
            pass
        if last:
            _, done, total, size, el = last
            self.bar.configure(value=done, maximum=total)
            left = el / done * (total - done) if done else 0
            self.status.set(f"Converting: {done:,} of {total:,} textures, {size / 2**20:,.0f} MB written, "
                            f"{fmt_time(el)} elapsed, about {fmt_time(left)} left.\n"
                            "You can leave the computer: it stays awake until this is done.")
        self.root.after(200, self.poll)

    def finish(self, ev):
        self.cancel_btn.pack_forget()
        for w in (self.pack_btn, self.out_btn):
            w.configure(state="normal")
        self.update_go()
        if ev[0] == "done":
            r = ev[1]
            self.bar.configure(value=1, maximum=1)
            msg = (f"Done: {r['textures']:,} textures, {r['bytes'] / 2**20:,.0f} MB, in {fmt_time(r['seconds'])}.\n\n"
                   f"Saved: {r['out']}\n\n"
                   f"Copy {OUT_NAME} to the SD card, into sdmc:/switch/MKDD-NX/ next to MKDD-NX.nro, "
                   "then turn on HD TEXTURES in the game's - menu.")
            if r["failed"]:
                msg += f"\n\n{len(r['failed'])} damaged texture file(s) were skipped; the game shows those textures as normal."
            self.status.set(msg)
            self.open_btn.pack(side="right")
            self.root.bell()
            messagebox.showinfo(APP, msg)
        elif ev[1] == "cancelled":
            self.bar.configure(value=0)
            self.status.set("Cancelled. Nothing was saved.")
        else:
            self.bar.configure(value=0)
            self.status.set("Conversion failed: " + ev[1])
            messagebox.showerror(APP, "The conversion failed:\n\n" + ev[1])

    def ask_cancel(self):
        if self.worker and self.worker.is_alive() and messagebox.askyesno(APP, "Stop the conversion? The partial file is deleted."):
            self.cancel.set()
            self.status.set("Stopping...")
            self.cancel_btn.configure(state="disabled")

    def close(self):
        if self.worker and self.worker.is_alive():
            if not messagebox.askyesno(APP, "A conversion is running. Stop it and quit?"):
                return
            self.cancel.set()
            self.worker.join(timeout=30)
        self.root.destroy()


def main():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
        except Exception:
            pass
    if len(sys.argv) == 4 and sys.argv[1] == "--convert":  # headless check of a packaged app: exit code 0 = ok
        try:
            r = hdtex_pack.convert(hdtex_pack.find_pack(sys.argv[2]) or sys.argv[2], sys.argv[3])
        except Exception as e:
            sys.exit(f"failed: {e}")
        sys.exit(0 if r["textures"] else 1)
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
