#!/usr/bin/env python3
"""MKDD-NX Tools: the companion apps in one window, a tab each. The Start tab has a card per tool;
Open (or the tool's own tab) shows it, made the first time, and each tab keeps its state while the
others are shown:

    HD Textures   tools/hdtex_app/hdtex_gui.py   the Dolphin HD texture pack -> hd-textures.bin
    Characters    tools/charpack.py              character mods -> MKDD-NX character folders
    Battle Routes tools/route_editor.py          the routes the computer karts drive in battles

    py -3 tools/mkddnx_tools.py                  the window (from source)
    MKDD-NX-Tools --cli ...                      tools/charpack.py's command line
    MKDD-NX-Tools --convert PACK OUT             tools/hdtex_app/hdtex_gui.py's headless check

Closing the window asks each open tool first (unsaved routes, a conversion running). Packaged as
one .exe / .dmg by tools/build_tools_app.py.
"""
import os
import sys
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import messagebox, ttk

HERE = Path(__file__).resolve().parent
if not getattr(sys, "frozen", False):
    sys.path[:0] = [str(HERE), str(HERE / "hdtex_app")]

APP = "MKDD-NX Tools"
WIKI = "https://github.com/digdat0/mkdd-nx/wiki/"
# (tab, what it does, wiki page, kind)
TOOLS = [
    ("HD Textures", "Convert the MKDD HD texture pack, made for Dolphin, into the one file MKDD-NX loads.",
     "HD-Textures-App", "hdtex"),
    ("Characters", "Turn downloaded character mods into characters for the select screen's extra pages: "
                   "checks each one and writes the folders for the SD card.", "Characters-App", "characters"),
    ("Battle Routes", "Edit the routes the computer karts drive in single player battles, on the game's arena maps.",
     "Route-Editor", "routes"),
]


def make_tool(kind, frame):
    """the tool in frame (has can_close()), None when it didn't open (Battle Routes' folders cancelled)"""
    if kind == "characters":
        import charpack
        return charpack.gui(frame)
    if kind == "hdtex":
        import hdtex_gui
        return hdtex_gui.App(frame)
    import route_editor
    return route_editor.open_editor(frame)


class Tools:
    def __init__(self, root):
        self.root = root
        root.title(APP)
        k = max(1.0, root.winfo_fpixels("1i") / 96)  # DPI aware: sized as on a 96 dpi screen
        root.geometry(f"{int(1000 * k)}x{int(700 * k)}")
        ttk.Style(root).configure("TNotebook.Tab", padding=(int(16 * k), int(6 * k)), font=("", 10, "bold"))
        self.book = ttk.Notebook(root)
        self.book.pack(fill="both", expand=True)
        self.tools, self.pages = {}, {}  # kind -> the tool, its tab's frame
        self.book.add(self.start_page(), text="Start")
        for title, _, _, kind in TOOLS:
            self.pages[kind] = ttk.Frame(self.book)
            self.book.add(self.pages[kind], text=title)
        self.book.bind("<<NotebookTabChanged>>", lambda e: self.shown())
        root.protocol("WM_DELETE_WINDOW", self.close)

    def start_page(self):
        f = ttk.Frame(self.book, padding=18)
        ttk.Label(f, text=APP, font=("", 16, "bold")).pack(anchor="w")
        ttk.Label(f, text="Companion apps for MKDD-NX, Mario Kart: Double Dash!! on Switch. Each one has a tab "
                          "above; switch between them at any time.").pack(anchor="w", pady=(2, 12))
        for title, text, page, kind in TOOLS:
            card = ttk.LabelFrame(f, text=title, padding=10)
            card.pack(fill="x", pady=5)
            card.columnconfigure(0, weight=1)
            ttk.Label(card, text=text, wraplength=560, justify="left").grid(row=0, column=0, rowspan=2, sticky="w")
            ttk.Button(card, text="Open", width=10, command=lambda k=kind: self.book.select(self.pages[k])).grid(row=0, column=1, padx=(12, 0))
            link = tk.Label(card, text="How to use", fg="#0066cc", cursor="hand2", font=("", 9, "underline"))
            link.grid(row=1, column=1, padx=(12, 0))
            link.bind("<Button-1>", lambda e, p=page: webbrowser.open(WIKI + p))
        ttk.Label(f, text="Your disc image is only read; nothing from it is saved.", foreground="#666").pack(anchor="w", pady=(10, 0))
        return f

    def shown(self):
        """a tool's tab shown for the first time: the tool made in it"""
        page = self.root.nametowidget(self.book.select())
        kind = next((k for k, p in self.pages.items() if p is page), None)
        if kind is None or kind in self.tools:
            return
        self.tools[kind] = None  # (one at a time: Battle Routes' dialogs)
        title = next(t[0] for t in TOOLS if t[3] == kind)
        try:
            tool = make_tool(kind, page)
        except Exception as e:  # a broken tool must not take the others with it
            tool = None
            messagebox.showerror(APP, f"{title} could not open: {e}")
        if tool is None:  # cancelled or broken: empty again, made anew when its tab is next shown
            for w in page.winfo_children():
                w.destroy()
            del self.tools[kind]
            self.book.select(0)
        else:
            self.tools[kind] = tool
            self.fit(page)

    def fit(self, page):
        """the window grown to what the tool in page asks for (never shrunk; at most the screen)"""
        self.root.update_idletasks()
        tabs = self.book.winfo_height() - page.winfo_height()
        w = max(self.root.winfo_width(), page.winfo_reqwidth() + 8)
        h = max(self.root.winfo_height(), page.winfo_reqheight() + tabs + 8)
        w, h = min(w, self.root.winfo_screenwidth() - 40), min(h, self.root.winfo_screenheight() - 80)
        if (w, h) != (self.root.winfo_width(), self.root.winfo_height()):
            self.root.geometry(f"{w}x{h}")

    def close(self):
        if all(t is None or t.can_close() for t in self.tools.values()):
            self.root.destroy()


def main():
    if "--cli" in sys.argv:
        import charpack
        sys.exit(charpack.cli(sys.argv[1:]))
    if len(sys.argv) == 4 and sys.argv[1] == "--convert":
        import hdtex_gui
        hdtex_gui.main()
        return
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
        except Exception:
            pass
    root = tk.Tk()
    Tools(root)
    root.mainloop()


if __name__ == "__main__":
    main()
