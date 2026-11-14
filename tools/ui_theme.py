"""The look the tools share: light by default (each tool run on its own), and MKDD-NX Tools' dark
theme (the Windows launcher's colours, win/launcher.c) once apply() has run. The tools read the
colours below when they build their widgets, so apply() comes first."""
import sys

# the launcher's colours
BG, PANEL, FIELD, TEXT_C = "#111217", "#1d1f27", "#15161c", "#eceef2"
DIM_C, GOLD, RED, RED_HOT, RED_DOWN = "#9296a5", "#ffc629", "#e42030", "#f63a48", "#b21422"
BTN, BTN_HOT, BTN_DOWN, OFF, LINE = "#30333f", "#3e4251", "#262832", "#484c5c", "#2c2f3a"

dark = False
DIM = "#666"                 # notes under a field
LINK = "#0066cc"
PREVIEW = ("#46505f", "#ddd")  # model previews: background, text
NOTES = {"ok": "#1a7f37", "info": "#555555", "warn": "#9a6700", "error": "#cf222e"}  # a mod's notes by level
FONT = "Segoe UI" if sys.platform == "win32" else "Helvetica"


def apply(root, k=1.0):
    """the dark theme on root's ttk widgets and on the plain Tk ones made after this"""
    global dark, DIM, LINK, PREVIEW, NOTES
    from tkinter import ttk
    dark, DIM, LINK, PREVIEW = True, DIM_C, "#6cb6ff", ("#262a35", DIM_C)
    NOTES = {"ok": "#57d27a", "info": "#b4b8c6", "warn": GOLD, "error": "#ff6b74"}
    pad = int(6 * k)
    s = ttk.Style(root)
    s.theme_use("clam")
    base = (FONT, 10)
    s.configure(".", background=PANEL, foreground=TEXT_C, fieldbackground=FIELD, bordercolor=LINE, lightcolor=PANEL,
                darkcolor=PANEL, troughcolor=FIELD, focuscolor=GOLD, selectbackground=RED, selectforeground="white",
                insertcolor=TEXT_C, font=base, arrowcolor=TEXT_C)
    s.map(".", foreground=[("disabled", OFF)])
    s.configure("TFrame", background=PANEL)
    s.configure("Side.TFrame", background=BG)
    s.configure("TLabel", background=PANEL, foreground=TEXT_C)
    s.configure("TButton", background=BTN, foreground=TEXT_C, bordercolor=BTN, lightcolor=BTN, darkcolor=BTN,
                padding=(int(12 * k), pad - 2), relief="flat", focusthickness=1)
    s.map("TButton", background=[("disabled", BTN_DOWN), ("pressed", BTN_DOWN), ("active", BTN_HOT)],
          bordercolor=[("focus", GOLD)], lightcolor=[("active", BTN_HOT)], darkcolor=[("active", BTN_HOT)],
          foreground=[("disabled", OFF)])
    s.configure("Accent.TButton", background=RED, bordercolor=RED, lightcolor=RED, darkcolor=RED, foreground="white",
                font=(FONT, 10, "bold"))
    s.map("Accent.TButton", background=[("disabled", BTN_DOWN), ("pressed", RED_DOWN), ("active", RED_HOT)],
          lightcolor=[("active", RED_HOT)], darkcolor=[("active", RED_HOT)])
    for w in ("TEntry", "TCombobox", "TSpinbox"):
        s.configure(w, fieldbackground=FIELD, foreground=TEXT_C, bordercolor=LINE, lightcolor=FIELD, darkcolor=FIELD,
                    background=BTN, padding=int(4 * k), arrowsize=int(13 * k))
        s.map(w, bordercolor=[("focus", GOLD)], fieldbackground=[("readonly", FIELD), ("disabled", BG)],
              foreground=[("disabled", OFF)], background=[("active", BTN_HOT)])
    s.configure("TCheckbutton", background=PANEL, foreground=TEXT_C, indicatorbackground=FIELD, indicatorforeground="white",
                indicatormargin=(0, 0, int(6 * k), 0), upperbordercolor=LINE, lowerbordercolor=LINE)
    s.map("TCheckbutton", indicatorbackground=[("selected", RED), ("active", BTN_HOT)], background=[("active", PANEL)])
    s.configure("TRadiobutton", background=PANEL, foreground=TEXT_C, indicatorbackground=FIELD, indicatorforeground="white")
    s.map("TRadiobutton", indicatorbackground=[("selected", RED), ("active", BTN_HOT)], background=[("active", PANEL)])
    s.configure("TLabelframe", background=PANEL, bordercolor=LINE, lightcolor=PANEL, darkcolor=PANEL, relief="solid")
    s.configure("TLabelframe.Label", background=PANEL, foreground=GOLD, font=(FONT, 10, "bold"))
    s.configure("Treeview", background=FIELD, fieldbackground=FIELD, foreground=TEXT_C, bordercolor=LINE,
                rowheight=int(24 * k))
    s.map("Treeview", background=[("selected", RED)], foreground=[("selected", "white")])
    s.configure("Treeview.Heading", background=BTN, foreground=TEXT_C, bordercolor=LINE, lightcolor=BTN, darkcolor=BTN,
                relief="flat", font=(FONT, 10, "bold"))
    s.map("Treeview.Heading", background=[("active", BTN_HOT)])
    s.configure("TProgressbar", background=RED, troughcolor=FIELD, bordercolor=FIELD, lightcolor=RED, darkcolor=RED,
                thickness=int(10 * k))
    for o in ("Vertical", "Horizontal"):
        s.configure(f"{o}.TScrollbar", background=BTN, troughcolor=FIELD, bordercolor=FIELD, lightcolor=BTN, darkcolor=BTN,
                    arrowcolor=DIM_C, gripcount=0)
        s.map(f"{o}.TScrollbar", background=[("active", BTN_HOT)])
        s.configure(f"{o}.TScale", background=TEXT_C, troughcolor=OFF, bordercolor=PANEL, lightcolor=TEXT_C, darkcolor=TEXT_C)
    s.configure("TPanedwindow", background=PANEL)
    s.configure("Sash", sashthickness=int(6 * k), background=LINE)
    s.configure("TNotebook", background=PANEL, bordercolor=LINE)
    s.configure("TNotebook.Tab", background=BTN, foreground=DIM_C, padding=(int(12 * k), int(4 * k)))
    s.map("TNotebook.Tab", background=[("selected", PANEL)], foreground=[("selected", TEXT_C)])
    s.configure("TSeparator", background=LINE)
    # plain Tk widgets (Text, Listbox, Canvas, the combobox's list) made after this
    for pat, val in (("*Background", PANEL), ("*Foreground", TEXT_C), ("*Text.Background", FIELD), ("*Text.Foreground", TEXT_C),
                     ("*Text.insertBackground", TEXT_C), ("*Text.selectBackground", RED), ("*Text.relief", "flat"),
                     ("*Text.highlightThickness", 1), ("*Text.highlightBackground", LINE), ("*Text.highlightColor", GOLD),
                     ("*Listbox.Background", FIELD), ("*Listbox.Foreground", TEXT_C), ("*Listbox.selectBackground", RED),
                     ("*Listbox.relief", "flat"), ("*Listbox.highlightThickness", 1), ("*Listbox.highlightBackground", LINE),
                     ("*TCombobox*Listbox.background", FIELD), ("*TCombobox*Listbox.foreground", TEXT_C),
                     ("*TCombobox*Listbox.selectBackground", RED), ("*TCombobox*Listbox.font", base),
                     ("*Menu.background", PANEL), ("*Menu.foreground", TEXT_C), ("*Menu.activeBackground", RED),
                     ("*Toplevel.Background", PANEL), ("*Canvas.highlightThickness", 0)):
        root.option_add(pat, val)
    root.configure(background=BG)
