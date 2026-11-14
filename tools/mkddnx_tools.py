#!/usr/bin/env python3
"""MKDD-NX Tools: the companion apps in one window, in the Windows launcher's look (win/launcher.c):
a header, a side bar with a page for each tool and the page shown. The Start page has a card per tool;
Open (or the tool's entry in the side bar) shows it, made the first time, and each page keeps its
state while the others are shown:

    HD Textures   tools/hdtex_app/hdtex_gui.py   the Dolphin HD texture pack -> hd-textures.bin
    Characters    tools/charpack.py              character mods -> MKDD-NX character folders
    Wii Characters tools/wiichar.py              a Mario Kart Wii driver (.szs) -> a character folder
    Wii Karts     tools/wiikart.py               a Mario Kart Wii kart (.szs) -> an extra kart folder
    Wii Batch     tools/wiibatch.py              many Mario Kart Wii drivers and karts at once
    Battle Routes tools/route_editor.py          the routes the computer karts drive in battles
    Coins         tools/coin_editor.py           the coins on each race course
    Languages     tools/langpack.py              a PAL disc's French, German, Italian, Spanish, a Japanese disc's Japanese -> mods/lang

    py -3 tools/mkddnx_tools.py                  the window (from source)
    MKDD-NX-Tools --cli ...                      tools/charpack.py's command line
    MKDD-NX-Tools --convert PACK OUT             tools/hdtex_app/hdtex_gui.py's headless check
    MKDD-NX-Tools --languages DISC OUT           tools/langpack.py's command line

Closing the window asks each open tool first (unsaved routes, a conversion running). Packaged as
one .exe / .dmg by tools/build_tools_app.py. The colours are tools/ui_theme.py's.
"""
import os
import sys
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
from pathlib import Path
from tkinter import messagebox, ttk

HERE = Path(__file__).resolve().parent
if not getattr(sys, "frozen", False):
    sys.path[:0] = [str(HERE), str(HERE / "hdtex_app")]
import ui_theme  # noqa: E402
import tools_update  # noqa: E402

APP = "MKDD-NX Tools"
WIKI = "https://github.com/digdat0/mkdd-nx/wiki/"
# (title, side bar name, what it does, wiki page, kind, icon: a Segoe MDL2 Assets glyph)
TOOLS = [
    ("HD Textures", "HD Textures", "Convert the MKDD HD texture pack, made for Dolphin, into the one file MKDD-NX loads.",
     "HD-Textures-App", "hdtex", ""),
    ("Export MKDD Characters", "Characters", "Turn downloaded character mods into characters for the select screen's extra "
     "pages: checks each one and writes the folders for the SD card.", "Characters-App", "characters", ""),
    ("Convert WiiMK Characters (beta)", "Wii Characters", "Convert a Mario Kart Wii driver (.szs) into a character: its "
     "model on the game's skeleton, with a preview. Drivers only, not karts.", "Wii-Characters", "wiichar", ""),
    ("Convert WiiMK Karts (beta)", "Wii Karts", "Convert a Mario Kart Wii kart (.szs) into an extra kart, added next to a "
     "Double Dash one and driving like it, with a preview. Karts only, not bikes.", "Wii-Karts", "wiikart", ""),
    ("Batch WiiMK (beta)", "Wii Batch", "Convert many Mario Kart Wii drivers and karts (.szs files or folders of mods) at "
     "once: a list like the Characters page's, with a preview of each, written as characters and extra karts.",
     "Wii-Characters", "wiibatch", ""),
    ("Battle Routes", "Battle Routes", "Edit the routes the computer karts drive in single player battles, on the game's "
     "arena maps.", "Route-Editor", "routes", ""),
    ("Coins", "Coins", "Place the coins on each race course (GAME > COINS), on the game's course maps, or start from the "
     "layout the game makes.", "Coins", "coins", ""),
    ("Languages", "Languages", "Copy the French, German, Italian and Spanish menus and texts from your European (PAL) disc "
     "image, or the Japanese ones from your Japanese disc image, to play in those languages (MODS > LANGUAGE).",
     "Languages", "langs", ""),
]
HOME_ICON = ""


def make_tool(kind, frame):
    """the tool in frame (has can_close()), None when it didn't open (Battle Routes' folders cancelled)"""
    if kind == "characters":
        import charpack
        return charpack.gui(frame)
    if kind == "wiichar":
        import wiichar
        return wiichar.gui(frame)
    if kind == "wiikart":
        import wiikart
        return wiikart.gui(frame)
    if kind == "wiibatch":
        import wiibatch
        return wiibatch.gui(frame)
    if kind == "langs":
        import langpack
        return langpack.gui(frame)
    if kind == "hdtex":
        import hdtex_gui
        return hdtex_gui.App(frame)
    if kind == "coins":
        import coin_editor
        return coin_editor.open_editor(frame)
    import route_editor
    return route_editor.open_editor(frame)


class Tools:
    """the window: a header, a side bar (Start, then the tools) and the page shown"""

    def __init__(self, root):
        self.root = root
        root.title(APP)
        self.k = k = max(1.0, root.winfo_fpixels("1i") / 96)  # DPI aware: sized as on a 96 dpi screen
        ui_theme.apply(root, k)
        root.geometry(f"{int(1180 * k)}x{int(760 * k)}")
        root.minsize(int(900 * k), int(560 * k))
        self.icons = sys.platform == "win32" and "Segoe MDL2 Assets" in tkfont.families(root)
        self.header = tk.Canvas(root, height=int(56 * k), background=ui_theme.BG, highlightthickness=0)
        self.header.pack(side="top", fill="x")
        self.header.bind("<Configure>", lambda e: self.draw_header())
        body = tk.Frame(root, background=ui_theme.BG)
        body.pack(side="top", fill="both", expand=True)
        self.side = tk.Frame(body, background=ui_theme.BG, width=int(220 * k))
        self.side.pack(side="left", fill="y", padx=(int(12 * k), 0), pady=int(10 * k))
        self.side.pack_propagate(False)
        # the page sits on a panel with rounded corners (a canvas behind the page's frame)
        self.panel = tk.Canvas(body, background=ui_theme.BG, highlightthickness=0)
        self.panel.pack(side="left", fill="both", expand=True, padx=int(10 * k), pady=int(10 * k))
        self.inner = ttk.Frame(self.panel)
        self.inner_id = self.panel.create_window(0, 0, window=self.inner, anchor="nw")
        self.panel.bind("<Configure>", lambda e: self.draw_panel())
        self.tools, self.pages, self.navs = {}, {}, {}  # kind -> the tool, its page's frame, its side bar entry
        self.cur = None
        self.pages["start"] = self.start_page()
        self.nav_item("start", "Start", HOME_ICON)
        tk.Frame(self.side, background=ui_theme.LINE, height=1).pack(fill="x", pady=int(8 * k), padx=int(10 * k))
        tk.Label(self.side, text="TOOLS", background=ui_theme.BG, foreground=ui_theme.DIM_C,
                 font=(ui_theme.FONT, 8, "bold"), anchor="w").pack(fill="x", padx=int(16 * k), pady=(0, int(4 * k)))
        for title, nav, _, _, kind, icon in TOOLS:
            self.pages[kind] = ttk.Frame(self.inner)
            self.nav_item(kind, nav, icon, "BETA" if "(beta)" in title else "")
        tk.Frame(self.side, background=ui_theme.LINE, height=1).pack(fill="x", pady=int(8 * k), padx=int(10 * k))
        self.pages["updates"] = ttk.Frame(self.inner, padding=int(14 * k))
        self.nav_item("updates", "Updates", "")
        self.updates = tools_update.UpdatePanel(self.pages["updates"], root, k, self.can_close,
                                                lambda new: self.navs["updates"][3].configure(text="Update!" if new else "Updates"))
        tk.Label(self.side, text="Your disc image is only read;\nnothing from it is saved.", background=ui_theme.BG,
                 foreground=ui_theme.DIM_C, font=(ui_theme.FONT, 8), justify="left", anchor="w").pack(
            side="bottom", fill="x", padx=int(16 * k), pady=int(6 * k))
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.select("start")

    # --- the look ---
    def draw_header(self):
        c, k = self.header, self.k
        c.delete("all")
        w, hh, sq = c.winfo_width(), int(46 * k), max(4, int(5 * k))
        for x in range(0, w, 3):  # red to deep red, as the launcher's
            a = x * 256 // max(1, w)
            c.create_rectangle(x, 0, x + 3, hh, width=0,
                               fill="#%02x%02x%02x" % (214 - a * 104 // 256, 20 - a * 12 // 256, 40 - a * 12 // 256))
        for row in range(2):  # the checkered strip
            for col in range(w // sq + 1):
                c.create_rectangle(col * sq, hh + row * sq, col * sq + sq, hh + row * sq + sq, width=0,
                                   fill="#121214" if (row + col) & 1 else "#f0f0f0")
        title = ("Segoe UI Black" if sys.platform == "win32" else ui_theme.FONT, 19, "italic")
        x = int(24 * k)
        c.create_text(x + 2, hh // 2 + 2, text="MKDD-NX", anchor="w", font=title, fill="#6e0816")
        t = c.create_text(x, hh // 2, text="MKDD-NX", anchor="w", font=title, fill="white")
        x2 = c.bbox(t)[2] + int(14 * k)
        c.create_text(x2, hh // 2 + int(2 * k), text="TOOLS", anchor="w", font=(ui_theme.FONT, 13, "bold"), fill=ui_theme.GOLD)
        c.create_text(w - int(24 * k), hh // 2, text="Companion apps for Mario Kart: Double Dash!!", anchor="e",
                      font=(ui_theme.FONT, 10), fill="#ffdede")

    def draw_panel(self):
        c, k = self.panel, self.k
        w, h, r = c.winfo_width(), c.winfo_height(), int(16 * k)
        c.delete("bg")
        for x0, y0 in ((0, 0), (w - 2 * r, 0), (0, h - 2 * r), (w - 2 * r, h - 2 * r)):
            c.create_oval(x0, y0, x0 + 2 * r, y0 + 2 * r, fill=ui_theme.PANEL, width=0, tags="bg")
        c.create_rectangle(r, 0, w - r, h, fill=ui_theme.PANEL, width=0, tags="bg")
        c.create_rectangle(0, r, w, h - r, fill=ui_theme.PANEL, width=0, tags="bg")
        c.tag_lower("bg")
        pad = int(8 * k)
        c.coords(self.inner_id, pad, pad)
        c.itemconfigure(self.inner_id, width=max(1, w - 2 * pad), height=max(1, h - 2 * pad))

    def nav_item(self, kind, text, icon, tag=""):
        k, bg = self.k, ui_theme.BG
        f = tk.Frame(self.side, background=bg, height=int(42 * k), cursor="hand2")
        f.pack(fill="x", pady=1)
        f.pack_propagate(False)
        bar = tk.Frame(f, background=bg, width=int(4 * k))
        bar.pack(side="left", fill="y", pady=int(9 * k))
        ic = tk.Label(f, text=icon if self.icons else "●", background=bg, foreground=ui_theme.DIM_C, width=2,
                      font=("Segoe MDL2 Assets", 13) if self.icons else (ui_theme.FONT, 8))
        ic.pack(side="left", padx=(int(10 * k), int(6 * k)))
        lb = tk.Label(f, text=text, background=bg, foreground=ui_theme.DIM_C, font=(ui_theme.FONT, 11, "bold"), anchor="w")
        lb.pack(side="left", fill="x")
        parts = [f, ic, lb]
        if tag:
            tg = tk.Label(f, text=tag, background=bg, foreground=ui_theme.GOLD, font=(ui_theme.FONT, 7, "bold"))
            tg.pack(side="right", padx=int(10 * k))
            parts.append(tg)
        self.navs[kind] = (parts, bar, ic, lb)
        for w in parts:
            w.bind("<Button-1>", lambda e, kd=kind: self.select(kd))
            w.bind("<Enter>", lambda e, kd=kind: self.paint_nav(kd, True))
            w.bind("<Leave>", lambda e, kd=kind: self.paint_nav(kd, False))

    def paint_nav(self, kind, hot=False):
        parts, bar, ic, lb = self.navs[kind]
        sel = kind == self.cur
        bg = ui_theme.PANEL if sel else "#1a1c23" if hot else ui_theme.BG
        for w in parts:
            w.configure(background=bg)
        bar.configure(background=ui_theme.RED if sel else bg)
        ic.configure(foreground=ui_theme.GOLD if sel else ui_theme.TEXT_C if hot else ui_theme.DIM_C)
        lb.configure(foreground=ui_theme.TEXT_C if sel or hot else ui_theme.DIM_C)

    def start_page(self):
        k = self.k
        f = ttk.Frame(self.inner, padding=int(14 * k))
        ttk.Label(f, text="Welcome", font=(ui_theme.FONT, 18, "bold")).pack(anchor="w")
        ttk.Label(f, text="Pick a tool on the left, or open one below. Each keeps its work while you look at the others.",
                  foreground=ui_theme.DIM).pack(anchor="w", pady=(2, int(12 * k)))
        grid = ttk.Frame(f)
        grid.pack(fill="both", expand=True)
        for c in range(2):
            grid.columnconfigure(c, weight=1, uniform="card")
        for i, (title, _, text, page, kind, icon) in enumerate(TOOLS):
            bg = ui_theme.FIELD
            card = tk.Frame(grid, background=bg, highlightthickness=1, highlightbackground=ui_theme.LINE)
            card.grid(row=i // 2, column=i % 2, sticky="nsew", padx=int(5 * k), pady=int(5 * k))
            card.columnconfigure(1, weight=1)
            tk.Label(card, text=icon if self.icons else "●", background=bg, foreground=ui_theme.GOLD,
                     font=("Segoe MDL2 Assets", 18) if self.icons else (ui_theme.FONT, 10)).grid(
                row=0, column=0, rowspan=2, sticky="n", padx=(int(14 * k), int(10 * k)), pady=int(14 * k))
            tk.Label(card, text=title, background=bg, foreground=ui_theme.TEXT_C, anchor="w",
                     font=(ui_theme.FONT, 11, "bold")).grid(row=0, column=1, sticky="ew", pady=(int(12 * k), 0))
            desc = tk.Label(card, text=text, background=bg, foreground=ui_theme.DIM_C, anchor="nw", justify="left",
                            wraplength=int(330 * k), font=(ui_theme.FONT, 9))
            desc.grid(row=1, column=1, sticky="new", padx=(0, int(12 * k)), pady=(int(2 * k), int(12 * k)))
            desc.bind("<Configure>", lambda e, d=desc: d.configure(wraplength=max(100, e.width - 4)))
            btns = tk.Frame(card, background=bg)
            btns.grid(row=0, column=2, rowspan=2, sticky="ne", padx=(0, int(12 * k)), pady=int(12 * k))
            ttk.Button(btns, text="Open", style="Accent.TButton", command=lambda kd=kind: self.select(kd)).pack(fill="x")
            if not page:  # (no wiki page)
                continue
            link = tk.Label(btns, text="How to use", background=bg, foreground=ui_theme.LINK, cursor="hand2",
                            font=(ui_theme.FONT, 9, "underline"))
            link.pack(pady=(int(6 * k), 0))
            link.bind("<Button-1>", lambda e, p=page: webbrowser.open(WIKI + p))
        return f

    # --- the pages ---
    def select(self, kind):
        if kind == self.cur:
            return
        old, self.cur = self.cur, kind
        if old:
            self.pages[old].pack_forget()
            self.paint_nav(old)
        self.pages[kind].pack(fill="both", expand=True)
        self.paint_nav(kind)
        if kind not in ("start", "updates") and kind not in self.tools:
            self.root.after_idle(lambda: self.shown(kind))

    def shown(self, kind):
        """a tool's page shown for the first time: the tool made in it"""
        if kind in self.tools or kind != self.cur:
            return
        page = self.pages[kind]
        self.tools[kind] = None  # (one at a time: Battle Routes' dialogs)
        title = next(t[0] for t in TOOLS if t[4] == kind)
        self.root.configure(cursor="watch")
        self.root.update_idletasks()
        try:
            tool = make_tool(kind, page)
        except Exception as e:  # a broken tool must not take the others with it
            tool = None
            messagebox.showerror(APP, f"{title} could not open: {e}")
        finally:
            self.root.configure(cursor="")
        if tool is None:  # cancelled or broken: empty again, made anew when its page is next shown
            for w in page.winfo_children():
                w.destroy()
            del self.tools[kind]
            self.select("start")
        else:
            self.tools[kind] = tool
            self.fit(page)

    def fit(self, page):
        """the window grown to what the tool in page asks for (never shrunk; at most the screen)"""
        self.root.update_idletasks()
        extra_w = self.root.winfo_width() - self.inner.winfo_width()
        extra_h = self.root.winfo_height() - self.inner.winfo_height()
        w = max(self.root.winfo_width(), page.winfo_reqwidth() + extra_w)
        h = max(self.root.winfo_height(), page.winfo_reqheight() + extra_h)
        w, h = min(w, self.root.winfo_screenwidth() - 40), min(h, self.root.winfo_screenheight() - 80)
        if (w, h) != (self.root.winfo_width(), self.root.winfo_height()):
            self.root.geometry(f"{w}x{h}")

    def can_close(self):
        return all(t is None or t.can_close() for t in self.tools.values())

    def close(self):
        if self.can_close():
            self.root.destroy()


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--run-py":  # (a script run by the frozen app)
        import runpy
        if sys.stdout is None:  # the windowed .exe: the pipe its caller reads
            try:
                sys.stdout = sys.stderr = open(1, "w", buffering=1, encoding="utf-8", errors="replace", closefd=False)
            except OSError:
                pass
        script = os.path.abspath(sys.argv[2])
        sys.argv = [script, *sys.argv[3:]]
        sys.path.insert(0, os.path.dirname(script))
        runpy.run_path(script, run_name="__main__")
        sys.exit(0)
    if "--cli" in sys.argv:
        import charpack
        sys.exit(charpack.cli(sys.argv[1:]))
    if len(sys.argv) == 4 and sys.argv[1] == "--languages":
        import langpack
        sys.exit(langpack.main(sys.argv[2:]))
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
    # the window is gone: end now, not when a conversion or a preview still running in the background ends
    for f in (sys.stdout, sys.stderr):  # (None in the windowed .exe)
        if f:
            f.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
