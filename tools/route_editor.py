"""Edit the battle routes of the computer's karts (the files tools/battle_routes.py writes), and the
map they are made on.

usage: py -3 tools/route_editor.py [<routes dir>] [--open KEYS] [--disc IMAGE] [--mod DIR] [--game-map]
(or Battle Routes in the MKDD-NX Tools app, tools/build_tools_app.py). Without <routes dir> it asks for
the folders, and remembers them (SETTINGS).

<routes dir>: the repository's routes (built into the .nro: rebuild after saving) or the SD card's
switch/MKDD-NX/routes (a file there wins over the built-in one). An arena with no <key>.txt there
opens its built-in route (BUILTIN); Save writes <key>.txt (the one before kept as <key>.bak) and
its map edits, map/<key>.txt. The game's map is drawn from your disc image (--disc, default the one
in the app folder) and a mod's (--mod), only read. How to use the window: the wiki's Route Editor
page and docs/BATTLE_ROUTES.md.
"""
import argparse, copy, glob, math, os, random, sys
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battle_routes as br

try:
    from PIL import Image, ImageDraw, ImageTk
except ImportError:
    sys.exit("the route editor needs Pillow: py -3 -m pip install pillow")

# the routes built into the .nro: the repository's routes/ (in the .exe, its own copy)
BUILTIN = os.path.join(getattr(sys, "_MEIPASS", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")), "routes")
SETTINGS = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~/Library/Application Support" if sys.platform == "darwin" else "~"),
                        "MKDD-NX", "route_editor.json")
NAMES = {"3a": "Cookie Land", "36": "Block City", "34": "Luigi's Mansion", "38": "Tilt-a-Kart", "35": "Nintendo GameCube", "3b": "Pipe Plaza"}
# MKDD Plus v3.3's arenas (its CourseNam1-4 name pictures), by route key; the others go by NAMES
PLUS_NAMES = {
    "34_f2ef4fad": "Wii Block Plaza", "35_565fb9cd": "N64 Big Donut", "36_524aad49": "DS Nintendo DS",
    "38_b6b9aa46": "3DS Sherbet Rink", "3a_0fefb882": "SNES Battle Course 1", "3b_df10b834": "GBA Battle Course 4",
    "34_dc6f7e5d": "SNES Battle Course 2", "35_6506cbc0": "Wii Funky Stadium", "36_53585fc1": "3DS Wuhu Town",
    "38_5009c6aa": "3DS Tart Top", "3a_0fe57e4c": "N64 Skyscraper", "3b_c6f94b4e": "GBA Battle Course 2",
    "34_26a8ec91": "N64 Double Deck", "35_b5ad7dd7": "GBA Battle Course 1", "36_a308ef2f": "Wii Delfino Pier",
    "38_e9cd7c72": "3DS Honeybee Hive", "3a_92fe685d": "DS Palm Shore", "3b_97dd873c": "SNES Battle Course 3",
    "34_0aa7c954": "DS Twilight House", "35_54971312": "SNES Battle Course 4", "36_6ad7734e": "N64 Block Fort",
    "38_1809e745": "Wii Chain Chomp Wheel", "3a_24b2990d": "GBA Battle Course 3", "3b_fb48db3e": "Wii Thwomp Desert",
}


def arena_name(key):
    return PLUS_NAMES.get(key) or NAMES.get(key[:2], "")
HIT = 8  # pixels: how near the mouse a point or line counts as under it
SHADE = [(58, 60, 68), (92, 98, 118), (128, 136, 160)]  # floors driven, the higher lighter
PAINTED = (96, 128, 168)  # floor added in the map edits
CELL_ALPHA = 190          # the cells over the game's map
BG = "#121216"
GAME = {"road": (46, 82, 56), "wall": (122, 48, 48), "out": (70, 44, 88), "pipe": (40, 140, 160)}
DISC_TYPES = (".iso", ".gcm", ".ciso", ".rvz")


def kind(t):
    """a collision type's kind (battle_routes.game_collision)"""
    hi = t >> 8
    return "road" if hi in br.ROAD else "wall" if hi == 0x02 else "pipe" if hi == 0x0D else "out"


class Editor:
    def __init__(self, root, routes, open_keys, disc, game_map=False, mod=None):
        self.root, self.routes, self.open_keys, self.disc, self.mod = root, routes, open_keys, disc, mod
        self.m, self.key, self.sel, self.undo = None, None, None, []
        self.nets, self.cur = [[]], 0  # the file's networks (the game picks one each battle), the one shown
        self.cover_key, self.uncovered, self.covered_share = None, set(), 0.0
        self.files, self.tris, self.road, self.game = [], [], {}, None
        self.added, self.removed = {}, {}  # the map edits: (ix, iz, height band) -> (ix, iz, height)
        self.routes_dirty = self.map_dirty = False
        self.drag = None  # ("point", part, index), ("junction", number), ("pan", x, y), ("paint", add)
        self.scale, self.ox, self.oz = 0.05, 0.0, 0.0
        self.base, self.base_key, self.version = None, None, 0
        self.floors, self.floor = [0.0], tk.StringVar(value="all")
        self.show_game, self.map_mode, self.brush = tk.BooleanVar(value=True), tk.BooleanVar(value=False), tk.IntVar(value=1)
        self.source = tk.StringVar(value="Game" if game_map else "Recordings")  # what the map is made from
        self.show_cover, self.net_name = tk.BooleanVar(value=False), tk.StringVar(value="1")
        self.embedded = root.winfo_toplevel() is not root  # MKDD-NX Tools' tab: the window is the app's
        if not self.embedded:
            root.title("MKDD-NX Battle Routes")
        bar = ttk.Frame(root)
        bar.pack(side=tk.TOP, fill=tk.X)
        files = os.listdir(routes) + (os.listdir(BUILTIN) if os.path.isdir(BUILTIN) else [])
        keys = sorted(set(br.recordings(routes)) | {f[:11] for f in files if f.endswith(".txt") and len(f) == 15}
                      | {os.path.basename(f)[:11] for f in glob.glob(os.path.join(routes, "map", "*.txt"))})
        names = [f"{k}  {arena_name(k)}" for k in keys]
        # the whole list open at once, no scrolling, as far as the screen's height allows (~20 px a row)
        rows = max(1, min(len(names), (self.root.winfo_screenheight() - 120) // 20))
        self.arena = ttk.Combobox(bar, values=names, state="readonly", height=rows,
                                  width=max([30] + [len(n) + 2 for n in names]))
        self.arena.pack(side=tk.LEFT, padx=4, pady=4)
        self.arena.bind("<<ComboboxSelected>>", lambda e: self.load(self.arena.get().split()[0]))
        ttk.Label(bar, text="Map from").pack(side=tk.LEFT, padx=(10, 2))
        src = ttk.Combobox(bar, textvariable=self.source, values=["Recordings", "Game"], state="readonly", width=11)
        src.pack(side=tk.LEFT)
        src.bind("<<ComboboxSelected>>", lambda e: self.set_source())
        ttk.Label(bar, text="Floor").pack(side=tk.LEFT, padx=(10, 2))
        self.floor_box = ttk.Combobox(bar, textvariable=self.floor, values=["all"], state="readonly", width=5)
        self.floor_box.pack(side=tk.LEFT)
        self.floor_box.bind("<<ComboboxSelected>>", lambda e: self.draw())
        ttk.Checkbutton(bar, text="Game map", variable=self.show_game, command=self.draw).pack(side=tk.LEFT, padx=(10, 2))
        ttk.Checkbutton(bar, text="Coverage", variable=self.show_cover, command=self.draw).pack(side=tk.LEFT, padx=(10, 2))
        ttk.Checkbutton(bar, text="Edit map", variable=self.map_mode, command=self.set_mode).pack(side=tk.LEFT, padx=(10, 2))
        self.map_bar = ttk.Frame(bar)
        self.map_bar.pack(side=tk.LEFT)
        ttk.Label(self.map_bar, text="Brush").pack(side=tk.LEFT, padx=(6, 2))
        ttk.Spinbox(self.map_bar, from_=1, to=5, width=3, textvariable=self.brush, state="readonly").pack(side=tk.LEFT)
        ttk.Button(self.map_bar, text="Fill from game", command=self.fill_from_game).pack(side=tk.LEFT, padx=4)
        for text, cmd in (("Save", self.save), ("Reload", lambda: self.key and self.load(self.key)), ("Make routes", self.make_routes)):
            ttk.Button(bar, text=text, command=cmd).pack(side=tk.RIGHT, padx=4)
        side = ttk.Frame(root)
        side.pack(side=tk.LEFT, fill=tk.Y)
        nb = ttk.Frame(side)
        nb.pack(fill=tk.X, padx=4, pady=(4, 2))
        ttk.Label(nb, text="Network").pack(side=tk.LEFT)
        self.net_box = ttk.Combobox(nb, textvariable=self.net_name, values=["1"], state="readonly", width=4)
        self.net_box.pack(side=tk.LEFT, padx=4)
        self.net_box.bind("<<ComboboxSelected>>", lambda e: self.show_net(int(self.net_name.get()) - 1))
        self.net_count = ttk.Label(nb, text="of 1")
        self.net_count.pack(side=tk.LEFT)
        nb2 = ttk.Frame(side)
        nb2.pack(fill=tk.X, padx=4, pady=(0, 6))
        ttk.Button(nb2, text="Add network", command=self.add_net).pack(side=tk.LEFT, expand=True, fill=tk.X)
        ttk.Button(nb2, text="Delete network", command=self.delete_net).pack(side=tk.LEFT, expand=True, fill=tk.X)
        self.cover_label = ttk.Label(side, text="")
        self.cover_label.pack(anchor=tk.W, padx=4)
        ttk.Label(side, text="Parts (from junction > to)").pack(anchor=tk.W, padx=4)
        self.listbox = tk.Listbox(side, width=26, height=30, exportselection=False)
        self.listbox.pack(fill=tk.Y, expand=True, padx=4)
        self.listbox.bind("<<ListboxSelect>>", self.pick)
        for text, cmd in (("New part", self.new_part), ("Copy part", self.copy_part), ("Delete part", self.delete_part)):
            ttk.Button(side, text=text, command=cmd).pack(fill=tk.X, padx=4, pady=2)
        self.legend = tk.Canvas(side, width=180, height=140, background=BG, highlightthickness=0)
        self.legend.pack(padx=4, pady=6)
        self.status = tk.StringVar(value="Pick an arena")
        ttk.Label(root, textvariable=self.status, anchor=tk.W).pack(side=tk.BOTTOM, fill=tk.X)
        self.canvas = tk.Canvas(root, width=900, height=800, background=BG, highlightthickness=0)
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        c = self.canvas
        c.bind("<ButtonPress-1>", self.press)
        c.bind("<B1-Motion>", self.motion)
        c.bind("<ButtonRelease-1>", self.release)
        rb, mb = ("2", "3") if sys.platform == "darwin" else ("3", "2")  # macOS Tk: right is button 2, middle 3
        c.bind(f"<ButtonPress-{rb}>", self.right)
        c.bind(f"<B{rb}-Motion>", self.right_motion)
        c.bind(f"<ButtonRelease-{rb}>", self.release)
        c.bind(f"<B{mb}-Motion>", self.pan)
        c.bind(f"<ButtonRelease-{mb}>", lambda e: setattr(self, "drag", None))
        c.bind(f"<ButtonPress-{mb}>", lambda e: setattr(self, "drag", ("pan", e.x, e.y)))
        if sys.platform == "darwin":  # Control-click is a right click on a Mac
            c.bind("<Control-ButtonPress-1>", self.right)
            c.bind("<Control-B1-Motion>", self.right_motion)
            c.bind("<Control-ButtonRelease-1>", self.release)
            root.winfo_toplevel().bind("<Command-z>", lambda e: self.root.winfo_ismapped() and self.do_undo())
        c.bind("<MouseWheel>", self.zoom)
        c.bind("<Motion>", self.hover)
        c.bind("<Configure>", lambda e: self.draw())
        root.winfo_toplevel().bind("<Control-z>", lambda e: self.root.winfo_ismapped() and self.do_undo())
        if not self.embedded:
            root.protocol("WM_DELETE_WINDOW", self.quit)
        self.draw_legend()
        self.set_mode()
        if len(keys) == 1:
            self.arena.current(0)
            self.load(keys[0])

    # --- the networks ---
    @property
    def parts(self):
        """the network shown: [[from, to, points]]"""
        return self.nets[self.cur]

    @parts.setter
    def parts(self, v):
        self.nets[self.cur] = v

    def show_net(self, i):
        self.cur, self.sel = max(0, min(i, len(self.nets) - 1)), None
        self.net_box["values"] = [str(k + 1) for k in range(len(self.nets))]
        self.net_name.set(str(self.cur + 1))
        self.net_count.config(text=f"of {len(self.nets)}" + ("  (the game picks one each battle)" if len(self.nets) > 1 else ""))
        self.relist()
        self.draw()

    def keep_nets(self):
        self.undo.append(("nets", (copy.deepcopy(self.nets), self.cur)))
        del self.undo[:-50]
        self.routes_dirty = True

    def generate(self):
        """a new network from the map (battle_routes.best_network): its parts, or None"""
        self.status.set("Making routes (this can take a minute)...")
        self.root.config(cursor="watch")
        self.root.update()
        try:
            best, tries = br.best_network(self.m, random.Random())
        finally:
            self.root.config(cursor="")
        if not best:
            self.status.set(f"No routes: {tries} tried; the map has too little floor away from the walls")
            return None
        return [[a, b, list(p)] for a, b, p, _ in sorted(best, key=lambda q: q[0] != 0)]

    def add_net(self):
        """another network, made from the map: the game picks one of the file's each battle"""
        if not self.m:
            return
        net = self.generate()
        if net is None:
            return
        self.keep_nets()
        if not self.nets[-1]:
            self.nets[-1] = net  # (an empty one: filled)
        else:
            self.nets.append(net)
        self.show_net(len(self.nets) - 1)
        self.status.set(f"Network {self.cur + 1} of {len(self.nets)}: {len({q[0] for q in net})} junctions, {len(net)} parts, "
                        f"covers {100 * self.cover()[1]:.0f}% (not saved)")

    def delete_net(self):
        if len(self.nets) < 2:
            self.status.set("The file needs one network at least: Make routes replaces it")
            return
        self.keep_nets()
        del self.nets[self.cur]
        self.show_net(self.cur)

    def cover(self):
        """the cells no route of the network shown comes near, and the share covered (cached)"""
        key = (self.version, repr(self.parts))
        if key != self.cover_key and self.m:
            got = set()
            for a, b, pts in self.parts:
                got |= br.cover_cells(self.m, pts)
            self.uncovered = set(self.m.main) - got
            self.covered_share = len(got) / max(1, len(self.m.main))
            self.cover_key = key
        return self.uncovered, self.covered_share

    # --- the arena ---
    def unsaved(self, doing):
        """True to go on (nothing unsaved, or the user says to drop it)"""
        what = [w for w, d in (("the route", self.routes_dirty), ("the map", self.map_dirty)) if d]
        return not what or messagebox.askyesno("Not saved", f"Changes to {' and '.join(what)} are not saved. {doing} anyway?")

    def can_close(self):
        return self.unsaved("Close")

    def quit(self):
        if self.can_close():
            self.root.destroy()

    def load(self, key):
        if self.key and not self.unsaved("Load"):
            self.arena.set(f"{self.key}  {arena_name(self.key)}")
            return
        self.status.set(f"Loading {key}...")
        self.root.update_idletasks()
        self.key, self.undo, self.sel = key, [], None
        self.files = br.recordings(self.routes, key).get(key, [])
        added, removed = br.read_map(self.routes, key)
        self.added = {self.band(c): c for c in added}
        self.removed = {self.band(c): c for c in removed}
        self.floor.set("all")
        self.tris = self.game_map(key)
        self.road = br.road_cells(self.tris) if self.tris else {}
        self.rebuild()
        path = os.path.join(self.routes, key + ".txt")
        if not os.path.exists(path):
            path = os.path.join(self.routes, "net", key + ".txt")
        if not os.path.exists(path):
            path = os.path.join(BUILTIN, key + ".txt")
        self.nets = [[[a, b, list(p)] for a, b, p in net] for net in br.read_networks(path)] if os.path.exists(path) else [[]]
        self.cur = 0
        self.routes_dirty = self.map_dirty = False
        self.fit()
        self.show_net(0)
        where = ("the built-in route" if os.path.dirname(path) == BUILTIN and os.path.abspath(self.routes) != os.path.abspath(BUILTIN)
                 else os.path.relpath(path, self.routes)) if self.parts else "no route yet"
        if len(self.nets) > 1:
            where += f", {len(self.nets)} networks"
        game = f"; the game's map from {self.game_from.get(key, '?')}" if self.tris else "; no game map (" + (
            ("not one of the game's arenas" + ("" if self.mod else ", a mod's: --mod")) if self.disc or self.mod else "no disc image: --disc") + ")"
        made = "; the map from the game's" if self.from_game() else "" if self.m else "; no recordings: no map"
        if self.source.get() == "Game" and not self.tris:
            made = "; no game map here, so the map is from the recordings" + ("" if self.m else ": none")
        self.status.set(f"{key}: {len(self.parts)} parts ({where})" + made + game)

    def game_map(self, key):
        if self.game is None:
            self.game, self.game_from = {}, {}
            for what, kw in (("disc", {"disc_path": self.disc}), ("mod", {"mod_dir": self.mod})):
                if not next(iter(kw.values())):
                    continue
                try:
                    got = br.game_collision(**kw)
                except Exception as e:  # (a damaged or different image or folder: the editor works without it)
                    messagebox.showwarning("Game map", f"Could not read the {what}: {e}")
                    continue
                for k, v in got.items():
                    if k not in self.game:
                        self.game[k] = v
                        self.game_from[k] = os.path.basename(next(iter(kw.values())).rstrip("/\\"))
        return (self.game or {}).get(key, [])

    def from_game(self):
        """the map is made from the game's map (Map from: Game, and there is one)"""
        return self.source.get() == "Game" and bool(self.tris)

    def set_source(self):
        if self.source.get() == "Game" and self.key and not self.tris:
            self.source.set("Recordings")
            messagebox.showinfo("Map from", "There is no game map for this arena (see the status bar when it loads).")
            return
        if self.from_game() and self.map_mode.get():
            self.map_mode.set(False)
            self.set_mode()
        if self.key:
            self.status.set("Making the map...")
            self.root.update_idletasks()
            self.rebuild()
            self.fit()
            self.draw()
            self.status.set("The map from the game's: its road and walls (your recordings give only the pipes)" if self.from_game()
                            else "The map from your recordings and map edits")

    def rebuild(self):
        """the map again from the recordings and the edits (or the game's map)"""
        if self.from_game():
            self.m = br.arena_map(self.files, self.key in self.open_keys, ((), ()), self.tris)[0]
        else:
            self.m = br.arena_map(self.files, self.key in self.open_keys, (list(self.added.values()), list(self.removed.values())))[0]
        fl = br.floors(self.m) if self.m else [0.0]
        if len(fl) != len(self.floors):
            self.floor.set("all")
        self.floors = fl
        self.floor_box["values"] = ["all"] + [str(i + 1) for i in range(len(fl))] if len(fl) > 1 else ["all"]
        self.version += 1

    def fit(self):
        pts = [(self.m.cells[c][0], self.m.cells[c][2]) for c in (self.m.main if self.from_game() else self.m.cells)] if self.m else [(p[0], p[2]) for q in self.parts for p in q[2]]
        if not pts:
            return
        x0, x1 = min(p[0] for p in pts) - br.CELL, max(p[0] for p in pts) + 2 * br.CELL
        z0, z1 = min(p[1] for p in pts) - br.CELL, max(p[1] for p in pts) + 2 * br.CELL
        w, h = max(self.canvas.winfo_width(), 400), max(self.canvas.winfo_height(), 400)
        self.scale = min(w / (x1 - x0), h / (z1 - z0))
        self.ox, self.oz = x0 - (w / self.scale - (x1 - x0)) / 2, z0 - (h / self.scale - (z1 - z0)) / 2

    def to_screen(self, x, z):
        return (x - self.ox) * self.scale, (z - self.oz) * self.scale

    def to_world(self, sx, sy):
        return sx / self.scale + self.ox, sy / self.scale + self.oz

    def on_floor(self, y):
        return min(range(len(self.floors)), key=lambda i: abs(self.floors[i] - y))

    def fsel(self):
        """the floor chosen (an index), or None: all"""
        return None if self.floor.get() == "all" or len(self.floors) < 2 else int(self.floor.get()) - 1

    def shown(self, p):
        """a route point is drawn (and can be picked): on the floor chosen"""
        f = self.fsel()
        return f is None or self.on_floor(p[1]) == f

    def height(self, x, z, y_old):
        """a point's height: the floor under it nearest the floor chosen (all: its old height)"""
        if not self.m:
            return y_old
        want = self.floors[self.fsel()] if self.fsel() is not None else y_old
        k = (math.floor(x / br.CELL), math.floor(z / br.CELL))
        ys = [self.m.cells[c][1] for c in self.cells_at(k)]
        return min(ys, key=lambda y: abs(y - want)) if ys else y_old

    # --- drawing ---
    def render_base(self, w, h):
        """the game's map and the cells (the floor), as one picture of the view"""
        img = Image.new("RGB", (w, h), (18, 18, 22))
        fsel = self.fsel()
        if (self.show_game.get() or self.from_game()) and self.tris:
            d = ImageDraw.Draw(img)
            for vs, t in sorted(self.tris, key=lambda q: sum(v[1] for v in q[0])):
                if fsel is not None and self.on_floor(sum(v[1] for v in vs) / 3) != fsel:
                    continue
                col = GAME[kind(t)]
                d.polygon([self.to_screen(v[0], v[2]) for v in vs], fill=col, outline=tuple(min(255, q + 28) for q in col))
        if self.m and not self.from_game():  # (the game's map: drawn alone)
            layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            d = ImageDraw.Draw(layer)
            s = self.scale * br.CELL
            drawn = set()
            for cell, v in sorted(self.m.cells.items(), key=lambda kv: kv[1][1]):
                f = self.on_floor(v[1])
                if fsel is not None and f != fsel:
                    continue
                drawn.add(cell[:2])
                x, z = self.to_screen(cell[0] * br.CELL, cell[1] * br.CELL)
                if x > w or z > h or x + s < 0 or z + s < 0:
                    continue
                col = PAINTED if cell in self.m.painted else SHADE[min(f, 2)]
                d.rectangle([x, z, x + s - 1, z + s - 1], fill=col + (CELL_ALPHA,))
            for gx, gz in self.m.floor & drawn:  # the outline: sides of the floor with a wall beyond
                x, z = self.to_screen(gx * br.CELL, gz * br.CELL)
                for (dx, dz), seg in (((1, 0), (x + s, z, x + s, z + s)), ((-1, 0), (x, z, x, z + s)), ((0, 1), (x, z + s, x + s, z + s)), ((0, -1), (x, z, x + s, z))):
                    if (gx + dx, gz + dz) in self.m.walls:
                        d.line(seg, fill=(232, 232, 232, 255), width=2)
            img = Image.alpha_composite(img.convert("RGBA"), layer)
        return ImageTk.PhotoImage(img)

    def draw(self):
        c = self.canvas
        c.delete("all")
        if not self.key:
            return
        w, h = max(c.winfo_width(), 1), max(c.winfo_height(), 1)
        key = (w, h, self.scale, self.ox, self.oz, self.fsel(), self.show_game.get(), self.from_game(), self.version)
        if key != self.base_key:
            self.base, self.base_key = self.render_base(w, h), key
        c.create_image(0, 0, image=self.base, anchor=tk.NW)
        if self.map_mode.get():
            return  # the map alone
        if self.show_cover.get() and self.m:  # what no route comes near (not worked out again mid-drag)
            unc = self.uncovered if self.drag and self.drag[0] in ("point", "junction") else self.cover()[0]
            s, f = self.scale * br.CELL, self.fsel()
            for cell in unc:
                v = self.m.cells[cell]
                if f is not None and self.on_floor(v[1]) != f:
                    continue
                x, z = self.to_screen(cell[0] * br.CELL, cell[1] * br.CELL)
                if -s < x < w and -s < z < h:
                    c.create_rectangle(x, z, x + s, z + s, fill="#d04040", stipple="gray50", width=0)
        for i, (a, b, pts) in enumerate(self.parts):
            mine = i == self.sel
            col = "#ffd24a" if mine else "#4aa8ff" if self.sel is None else "#3b5670"
            q = [self.to_screen(p[0], p[2]) for p in pts]
            for k in range(1, len(q)):
                if not self.shown(pts[k - 1]) and not self.shown(pts[k]):
                    continue
                jump = math.dist(pts[k - 1], pts[k]) > 2 * br.SPACING
                c.create_line(*q[k - 1], *q[k], fill=col, width=3 if mine else 2, dash=(6, 4) if jump else None,
                              arrow=tk.LAST if k == len(q) // 2 and not jump else None, arrowshape=(12, 14, 5))
            if mine:
                for k, (x, z) in enumerate(q):
                    if not self.shown(pts[k]):
                        continue
                    cl = br.clear_at(self.m, pts[k]) if self.m else 1e9
                    dot = "#ff4040" if cl < br.MIN_LOW else "#ffa030" if self.m and cl < self.m.want else "#ffffff"
                    c.create_oval(x - 4, z - 4, x + 4, z + 4, fill=dot, outline="#000")
        for j, p in self.junctions().items():
            if not self.shown(p):
                continue
            x, z = self.to_screen(p[0], p[2])
            c.create_oval(x - 10, z - 10, x + 10, z + 10, fill="#fafafa", outline="#111", width=2)
            c.create_text(x, z, text=str(j), fill="#111", font=("Segoe UI", 9, "bold"))

    def draw_legend(self):
        c = self.legend
        rows = [(SHADE[0], "floor driven"), (PAINTED, "floor you added"), (GAME["road"], "game: road"),
                (GAME["wall"], "game: wall"), (GAME["out"], "game: out of bounds"), (GAME["pipe"], "game: pipe")]
        for i, (col, text) in enumerate(rows):
            c.create_rectangle(6, 6 + i * 22, 22, 22 + i * 22, fill="#%02x%02x%02x" % col, outline="#888")
            c.create_text(30, 14 + i * 22, text=text, anchor=tk.W, fill="#ddd", font=("Segoe UI", 9))

    def junctions(self):
        """{junction: where (x, y, z)}: the first point of the parts leaving it"""
        out = {}
        for a, b, pts in self.parts:
            if pts:
                out.setdefault(a, pts[0])
        return out

    def relist(self):
        self.listbox.delete(0, tk.END)
        for a, b, pts in self.parts:
            self.listbox.insert(tk.END, f"{a} > {b}   ({len(pts)} points)")
        if self.sel is not None and self.sel < len(self.parts):
            self.listbox.selection_set(self.sel)
        self.cover_label.config(text=f"Covers {100 * self.cover()[1]:.0f}% of the arena" if self.m and self.parts else "")

    def pick(self, e):
        s = self.listbox.curselection()
        self.sel = s[0] if s else None
        self.draw()

    def set_mode(self):
        if self.map_mode.get() and self.from_game():
            self.map_mode.set(False)
            self.status.set("The game's map is not edited: set Map from to Recordings to edit your map")
        if self.map_mode.get():
            self.map_bar.pack(side=tk.LEFT)
            self.status.set("Edit map: drag to paint floor, right-drag to take it away. Pick a floor first on arenas with more than one.")
        else:
            self.map_bar.pack_forget()
        self.draw()

    # --- editing: undo ---
    def keep(self):
        self.undo.append(("parts", (self.cur, copy.deepcopy(self.parts))))
        del self.undo[:-50]
        self.routes_dirty = True

    def keep_map(self):
        self.undo.append(("map", (dict(self.added), dict(self.removed))))
        del self.undo[:-50]
        self.map_dirty = True

    def do_undo(self):
        if not self.undo:
            return
        what, state = self.undo.pop()
        if what == "parts":
            cur, parts = state
            if cur < len(self.nets):
                self.nets[cur] = parts
            sel = self.sel if cur == self.cur else None
            self.show_net(cur)
            self.sel = None if sel is None or sel >= len(self.parts) else sel
            self.relist()
        elif what == "nets":
            self.nets, cur = state
            self.show_net(cur)
        else:
            self.added, self.removed = state
            self.rebuild()
        self.draw()

    # --- editing: routes ---
    def point_at(self, sx, sy):
        """the selected part's point under the mouse (its index), or None"""
        if self.sel is None:
            return None
        best, bd = None, HIT
        for k, p in enumerate(self.parts[self.sel][2]):
            d = math.dist((sx, sy), self.to_screen(p[0], p[2]))
            if d <= bd and self.shown(p):
                best, bd = k, d
        return best

    def junction_at(self, sx, sy):
        for j, p in self.junctions().items():
            if self.shown(p) and math.dist((sx, sy), self.to_screen(p[0], p[2])) <= 11:
                return j
        return None

    def line_at(self, sx, sy):
        """the selected part's line under the mouse: the index of the point after it, or None"""
        if self.sel is None:
            return None
        pts = self.parts[self.sel][2]
        for k in range(1, len(pts)):
            if not self.shown(pts[k - 1]) and not self.shown(pts[k]):
                continue
            ax, az = self.to_screen(pts[k - 1][0], pts[k - 1][2])
            bx, bz = self.to_screen(pts[k][0], pts[k][2])
            l2 = (bx - ax) ** 2 + (bz - az) ** 2
            t = max(0.0, min(1.0, ((sx - ax) * (bx - ax) + (sy - az) * (bz - az)) / l2)) if l2 else 0.0
            if math.dist((sx, sy), (ax + (bx - ax) * t, az + (bz - az) * t)) <= HIT:
                return k
        return None

    def press(self, e):
        if not self.key:
            return
        if self.map_mode.get():
            self.paint_start(e, True)
            return
        k = self.point_at(e.x, e.y)
        j = self.junction_at(e.x, e.y) if k in (None, 0) else None
        if e.state & 0x1 and self.sel is not None:  # shift: a point at the end
            self.keep()
            x, z = self.to_world(e.x, e.y)
            pts = self.parts[self.sel][2]
            pts.append((x, self.height(x, z, pts[-1][1] if pts else self.floors[0]), z))
            self.drag = ("point", self.sel, len(pts) - 1)
        elif j is not None:
            self.keep()
            self.drag = ("junction", j)
        elif k is not None:
            self.keep()
            self.drag = ("point", self.sel, k)
        else:
            at = self.line_at(e.x, e.y)
            if at is not None:
                self.keep()
                pts = self.parts[self.sel][2]
                x, z = self.to_world(e.x, e.y)
                pts.insert(at, (x, self.height(x, z, (pts[at - 1][1] + pts[at][1]) / 2), z))
                self.drag = ("point", self.sel, at)
            else:  # another part's line: select it
                self.drag, was = None, self.sel
                for i in range(len(self.parts)):
                    self.sel = i
                    if self.line_at(e.x, e.y) is not None:
                        break
                else:
                    self.sel = was
        self.relist()
        self.draw()

    def motion(self, e):
        if not self.drag or self.drag[0] == "pan":
            return
        if self.drag[0] == "paint":
            self.paint(e)
            return
        x, z = self.to_world(e.x, e.y)
        if self.drag[0] == "point":
            _, i, k = self.drag
            p = self.parts[i][2][k]
            self.parts[i][2][k] = (x, self.height(x, z, p[1]), z)
            if k == 0:  # a junction: every part leaving it starts there
                self.move_junction(self.parts[i][0], x, z)
        else:
            self.move_junction(self.drag[1], x, z)
        self.draw()
        self.hover(e)

    def move_junction(self, j, x, z):
        for a, b, pts in self.parts:
            if a == j and pts:
                pts[0] = (x, self.height(x, z, pts[0][1]), z)

    def release(self, e):
        if self.drag and self.drag[0] == "paint":
            self.drag = None
            self.status.set("Rebuilding the map...")
            self.root.update_idletasks()
            self.rebuild()
            self.draw()
            self.status.set(f"Map: {len(self.added)} cells added, {len(self.removed)} taken away (not saved)")
        elif self.drag and self.drag[0] != "pan":
            self.relist()
        self.drag = None

    def right(self, e):
        if self.map_mode.get():
            if self.key:
                self.paint_start(e, False)
            return
        k = self.point_at(e.x, e.y)
        if k is None:
            self.drag = ("pan", e.x, e.y)
            return
        pts = self.parts[self.sel][2]
        if len(pts) <= 2:
            self.status.set("A part needs 2 points at least: delete the part instead")
            return
        self.keep()
        del pts[k]
        if k == 0:  # the junction moves to the next point
            self.move_junction(self.parts[self.sel][0], pts[0][0], pts[0][2])
        self.relist()
        self.draw()

    def right_motion(self, e):
        if self.drag and self.drag[0] == "paint":
            self.paint(e)
        else:
            self.pan(e)

    def pan(self, e):
        if self.drag and self.drag[0] == "pan":
            _, x, y = self.drag
            self.ox -= (e.x - x) / self.scale
            self.oz -= (e.y - y) / self.scale
            self.drag = ("pan", e.x, e.y)
            self.draw()

    def zoom(self, e):
        x, z = self.to_world(e.x, e.y)
        self.scale *= 1.2 if e.delta > 0 else 1 / 1.2
        self.ox, self.oz = x - e.x / self.scale, z - e.y / self.scale
        self.draw()

    def hover(self, e):
        if not self.key or (self.drag and self.drag[0] == "paint"):
            return
        x, z = self.to_world(e.x, e.y)
        msg = f"x {x:.0f}  z {z:.0f}"
        if self.m:
            cl = br.clear_at(self.m, (x, self.floors[self.fsel()] if self.fsel() is not None else None if self.from_game() else 0.0, z))
            msg += "  off the floor" if cl < 0 else f"  {cl:.0f} from a wall"
        under = self.game_under(x, z)
        if under:
            msg += "   |   game: " + ", ".join(under)
        if self.sel is not None and not self.map_mode.get():
            a, b, pts = self.parts[self.sel]
            msg += f"   |   part {a} > {b}: {len(pts)} points"
        self.status.set(msg)

    def game_under(self, x, z):
        """the kinds of the game's map under (x, z), top first (on the floor chosen)"""
        out = []
        for vs, t in self.tris:
            (ax, ay, az), (bx, by, bz), (cx, cy, cz) = vs
            if x < min(ax, bx, cx) or x > max(ax, bx, cx) or z < min(az, bz, cz) or z > max(az, bz, cz):
                continue
            det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
            if abs(det) < 1e-6:
                continue
            u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / det
            v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / det
            if u >= 0 and v >= 0 and u + v <= 1:
                y = u * ay + v * by + (1 - u - v) * cy
                if self.fsel() is None or self.on_floor(y) == self.fsel():
                    out.append((y, kind(t)))
        return [f"{k} ({y:.0f})" for y, k in sorted(out, reverse=True)[:3]]

    # --- editing: the map ---
    @staticmethod
    def band(c):
        return (c[0], c[1], round(c[2] / br.LAYER))

    def cells_at(self, k):
        return [k + (i,) for i in range(len(self.m.layers.get(k, ()))) if k + (i,) in self.m.cells] if self.m else []

    def paint_height(self, k):
        """the height floor painted at cell k gets: the game's road there on the floor chosen, else that
        floor's"""
        want = self.floors[self.fsel() or 0]
        ys = [y for y in self.road.get(k, ()) if abs(y - want) < 2 * br.LAYER]
        return min(ys, key=lambda y: abs(y - want)) if ys else want

    def paint_start(self, e, add):
        if not self.m:
            self.status.set("No map to edit: record a battle here first")
            return
        if add and self.fsel() is None and len(self.floors) > 1:
            self.status.set("Pick a floor first: this arena has more than one")
            return
        self.keep_map()
        self.drag = ("paint", add)
        self.paint(e)

    def paint(self, e):
        add = self.drag[1]
        x, z = self.to_world(e.x, e.y)
        n = self.brush.get()
        k0 = (math.floor(x / br.CELL) - (n - 1) // 2, math.floor(z / br.CELL) - (n - 1) // 2)
        s = self.scale * br.CELL
        for ix in range(k0[0], k0[0] + n):
            for iz in range(k0[1], k0[1] + n):
                if add:
                    self.paint_cell((ix, iz))
                else:
                    self.erase_cell((ix, iz))
                sx, sz = self.to_screen(ix * br.CELL, iz * br.CELL)
                self.canvas.create_rectangle(sx, sz, sx + s, sz + s, fill="#%02x%02x%02x" % PAINTED if add else BG, width=0)
        self.status.set(f"{'Painting' if add else 'Taking away'}: {len(self.added)} cells added, {len(self.removed)} taken away (not saved)")

    def paint_cell(self, k):
        y = self.paint_height(k)
        for b in [b for b, c in self.removed.items() if c[:2] == k and abs(c[2] - y) < br.LAYER]:
            del self.removed[b]
        if not any(abs(self.m.cells[c][1] - y) < br.LAYER for c in self.cells_at(k) if c not in self.m.painted):
            self.added[self.band(k + (y,))] = k + (y,)

    def erase_cell(self, k):
        f = self.fsel()
        near = lambda y: f is None or self.on_floor(y) == f
        for b in [b for b, c in self.added.items() if c[:2] == k and near(c[2])]:
            del self.added[b]
        ys = [self.m.cells[c][1] for c in self.cells_at(k) if c not in self.m.painted and near(self.m.cells[c][1])]
        for y in ys or [self.floors[f or 0]]:  # (none: kept from being filled in)
            self.removed[self.band(k + (y,))] = k + (y,)

    def fill_from_game(self):
        """floor added on all the game's road joined to the floor there is (on the floor chosen)"""
        if not self.key or not self.m:
            return
        if not self.road:
            messagebox.showinfo("Fill from game", "There is no game map for this arena (see the status bar when it loads).")
            return
        f = self.fsel()
        near = lambda y: f is None or self.on_floor(y) == f
        have, erased = {}, {}
        for c, v in self.m.cells.items():
            have.setdefault(c[:2], []).append(v[1])
        for c in self.removed.values():
            erased.setdefault(c[:2], []).append(c[2])
        # from the floor there is (one floor, a tilting one too: at any height), the game's road a step
        # up or down at most from road next to it
        lay = self.m.layer
        todo = [(c[:2], v[1], br.STEP_UP if lay < 1e6 else 1e9) for c, v in self.m.cells.items() if near(v[1])]
        seen, new = set(), []
        while todo:
            k, y, step = todo.pop()
            for dx, dz in br.RING8 + [(0, 0)]:
                n = (k[0] + dx, k[1] + dz)
                for h in self.road.get(n, ()):
                    b = (n, round(h / br.LAYER))
                    if b in seen or abs(h - y) >= step or not near(h) or any(abs(h - q) < lay for q in erased.get(n, ())):
                        continue
                    seen.add(b)
                    todo.append((n, h, br.STEP_UP))
                    if not any(abs(h - q) < lay for q in have.get(n, ())):
                        new.append(n + (h,))
        if not new:
            self.status.set("Fill from game: the game's road joined to this floor is all floor already")
            return
        if not messagebox.askyesno("Fill from game", f"Add {len(new)} cells of floor where the game has road joined to the floor there is"
                                   + ("" if f is None else f" (floor {f + 1})") + "? Ctrl+Z undoes it."):
            return
        self.keep_map()
        for c in new:
            self.added[self.band(c)] = c
        self.status.set("Rebuilding the map...")
        self.root.update_idletasks()
        self.rebuild()
        self.draw()
        self.status.set(f"Added {len(new)} cells of floor from the game's map (not saved)")

    def make_routes(self):
        if not self.m:
            return
        if self.parts and not messagebox.askyesno("Make routes", f"Replace network {self.cur + 1}'s {len(self.parts)} parts with new routes made from this map? Ctrl+Z undoes it."):
            return
        net = self.generate()
        if net is None:
            return
        self.keep()
        self.parts = net
        self.sel = None
        self.relist()
        self.draw()
        self.status.set(f"New routes for network {self.cur + 1}: {len({q[0] for q in net})} junctions, {len(net)} parts, "
                        f"covers {100 * self.cover()[1]:.0f}% of the arena (not saved)")

    # --- parts ---
    def new_part(self):
        if not self.key:
            return
        js = self.junctions()
        a = simpledialog.askinteger("New part", f"From junction (there now: {sorted(js) or 'none'}):", parent=self.root, minvalue=0, maxvalue=63)
        if a is None:
            return
        b = simpledialog.askinteger("New part", "To junction:", parent=self.root, minvalue=0, maxvalue=63)
        if b is None:
            return
        cx, cz = self.to_world(self.canvas.winfo_width() / 2, self.canvas.winfo_height() / 2)
        pa = (js[a][0], js[a][2]) if a in js else (cx, cz)
        pb = (js[b][0], js[b][2]) if b in js else (pa[0] + 2000, pa[1])
        n = max(2, round(math.dist(pa, pb) / br.SPACING))
        y = self.floors[self.fsel() or 0]
        pts = []
        for t in range(n):  # a straight line, its last point short of the next junction (as the tool's)
            x, z = pa[0] + (pb[0] - pa[0]) * t / n, pa[1] + (pb[1] - pa[1]) * t / n
            pts.append((x, self.height(x, z, y), z))
        self.keep()
        self.parts.append([a, b, pts])
        self.sel = len(self.parts) - 1
        self.relist()
        self.draw()
        self.status.set(f"New part {a} > {b}: drag its points into place" + ("" if b in js else f"; junction {b} needs a part leaving it too"))

    def copy_part(self):
        if self.sel is None:
            return
        self.keep()
        a, b, pts = self.parts[self.sel]
        self.parts.append([a, b, list(pts)])
        self.sel = len(self.parts) - 1
        self.relist()
        self.draw()
        self.status.set(f"Copied: a second part {a} > {b} on top of the first; drag its points away")

    def delete_part(self):
        if self.sel is None:
            return
        self.keep()
        del self.parts[self.sel]
        self.sel = None
        self.relist()
        self.draw()

    def save(self):
        if not self.key:
            return
        done = []
        if self.map_dirty:
            path = br.write_map(self.routes, self.key, list(self.added.values()), list(self.removed.values()))
            self.map_dirty = False
            done.append(f"the map ({os.path.relpath(path, self.routes)})")
        if self.routes_dirty:
            nets = [[(a, b, pts) for a, b, pts in net] for net in self.nets]
            for i, net in enumerate(nets):
                bad = br.check_routes(net)
                if bad:
                    self.show_net(i)
                    messagebox.showerror("Route not saved", f"The game would not use network {i + 1}: {bad}." + (" The map was saved." if done else ""))
                    return
            red = sum(1 for net in nets for _, _, pts in net for p in pts if self.m and br.clear_at(self.m, p) < br.MIN_LOW)
            if red and not messagebox.askyesno("Near the walls", f"{red} points are very near a wall (red). Save the route anyway?"):
                self.status.set("Saved " + " and ".join(done) if done else "Not saved")
                return
            out = br.write_routes(self.routes, self.key, nets, True, ", edited (tools/route_editor.py)")
            self.routes_dirty = False
            done.append(f"the route ({os.path.relpath(out, self.routes)}): the next battle in this arena uses it")
        self.status.set("Saved " + " and ".join(done) if done else "Nothing changed since the last save")


def find_disc(routes):
    """a disc image in the app folder (the routes folder's parent), or None"""
    app = os.path.dirname(os.path.abspath(routes))
    for f in sorted(os.listdir(app)):
        if f.lower().endswith(DISC_TYPES):
            return os.path.join(app, f)
    return None


DEFAULT_OPEN = "35_3439d285,38_db1d03f9,3b_2ad1d46b"  # the arenas with no holes (battle_routes.py's --open)


def ask_folders(root):
    """(routes, disc, mod) chosen in dialogs (the last ones offered first, kept in SETTINGS); routes None: cancelled"""
    from tkinter import filedialog
    import json
    try:
        last = json.load(open(SETTINGS, encoding="utf-8"))
    except (OSError, ValueError):
        last = {}
    ok = lambda k, test: last.get(k) if last.get(k) and test(last[k]) else None
    routes, disc, mod = ok("routes", os.path.isdir), ok("disc", os.path.isfile), ok("mod", lambda p: os.path.isdir(os.path.join(p, "files")))
    if routes and messagebox.askyesno("MKDD-NX Battle Routes", f"Routes folder: {routes}\nDisc image: {disc or 'none'}\nMKDD Plus folder: {mod or 'none'}\n\nUse these again?"):
        return routes, disc, mod
    messagebox.showinfo("MKDD-NX Battle Routes", "Pick the routes folder to edit: switch/MKDD-NX/routes on the SD card (create it if "
                        "there is none), or the repository's routes. Arenas without a file there start from the built-in route.")
    routes = filedialog.askdirectory(title="Routes folder", initialdir=routes or None)
    if not routes:
        return None, None, None
    disc = filedialog.askopenfilename(title="Your disc image, for the game's arena maps (Cancel: none)", initialdir=os.path.dirname(disc) if disc else None,
                                      filetypes=[("GameCube disc image", " ".join("*" + t for t in DISC_TYPES)), ("All files", "*.*")]) or find_disc(routes)
    mod = filedialog.askdirectory(title="MKDD Plus's folder (the one holding files), for its arena maps (Cancel: none)", initialdir=mod or None) or None
    if mod and not os.path.isdir(os.path.join(mod, "files")):
        messagebox.showwarning("MKDD Plus", f"No files folder in {mod}: its arenas' maps are not shown.")
        mod = None
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        json.dump({"routes": routes, "disc": disc, "mod": mod}, open(SETTINGS, "w", encoding="utf-8"))
    except OSError:
        pass  # (asked again next time)
    return routes, disc, mod


def open_editor(frame):
    """the editor in frame (MKDD-NX Tools' tab), the folders asked for as the app does; None when cancelled"""
    routes, disc, mod = ask_folders(frame)
    if not routes:
        return None
    return Editor(frame, routes, set(DEFAULT_OPEN.split(",")), disc or find_disc(routes), False, mod)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("routes", nargs="?")
    ap.add_argument("--open", default=DEFAULT_OPEN, type=lambda v: set(v.split(",")) - {""},
                    help="arenas (keys, commas between) with no holes, as battle_routes.py's --open")
    ap.add_argument("--disc", help="your disc image (.iso/.gcm/.ciso/.rvz), for the game's map; default the one in the app folder")
    ap.add_argument("--mod", help="a mod's folder (the one holding files/), for its arenas' maps: MKDD Plus's")
    ap.add_argument("--game-map", action="store_true", help="start with Map from: Game (the map made from the game's, not the recordings)")
    a = ap.parse_args()
    root = tk.Tk()
    if not a.routes:
        root.withdraw()
        a.routes, disc, mod = ask_folders(root)
        if not a.routes:
            return
        a.disc, a.mod = a.disc or disc, a.mod or mod
        root.deiconify()
    if not os.path.isdir(a.routes):
        sys.exit(f"no folder {a.routes}")
    if a.mod and not os.path.isdir(os.path.join(a.mod, "files")):
        sys.exit(f"no files folder in {a.mod}: --mod is the folder holding files/")
    Editor(root, a.routes, a.open, a.disc or find_disc(a.routes), a.game_map, a.mod)
    root.mainloop()


if __name__ == "__main__":
    main()
