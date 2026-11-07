"""Edit a race course's coins: the layout file runtime/coins.c reads in place of the one it makes,
coins/<course ID>_<BOL hash>.txt in the app folder (switch/MKDD-NX/coins on the SD card, the PC
build's folder on Windows). The key is the battle routes' one, so a mod's course has its own file.

usage: py -3 tools/coin_editor.py [<coins dir>] [--disc IMAGE] [--mod DIR]
(or Coins in the MKDD-NX Tools app). The window picks its folders (Browse), remembered for next time.

The course is drawn from above from your disc image (only read): the road grey, off-road green,
water blue, the computer karts' line, the item boxes and the start. Generate puts down the layout the
game makes (the same rules, from the course's own collision); then:
    left click        a coin, or a row of them (Row) along the karts' line there
    left drag         move a coin
    right click       remove a coin; right or middle drag: move the view; wheel: zoom
    Ctrl+Z            undo
Save writes <key>.txt (the one before kept as <key>.bak). A course with no file gets the made layout;
Remove file deletes it (asked first). A saved file with no coins: no coins on that course.
"""
import argparse, json, math, os, re, struct, sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battle_routes as br

TITLE = "MKDD-NX Coins"
SETTINGS = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~/Library/Application Support" if sys.platform == "darwin" else "~"),
                        "MKDD-NX", "coin_editor.json")
DISC_TYPES = (".iso", ".gcm", ".ciso", ".rvz")
BG = "#121216"
NAMES = {0x21: "Baby Park", 0x22: "Peach Beach", 0x23: "Daisy Cruiser", 0x24: "Luigi Circuit", 0x25: "Mario Circuit",
         0x26: "Yoshi Circuit", 0x28: "Mushroom Bridge", 0x29: "Mushroom City", 0x2a: "Waluigi Stadium",
         0x2b: "Wario Colosseum", 0x2c: "Dino Dino Jungle", 0x2d: "DK Mountain", 0x2f: "Bowser's Castle",
         0x31: "Rainbow Road", 0x32: "Dry Dry Desert", 0x33: "Sherbet Land"}

# runtime/coins.c's rules (keep them the same)
COIN_MAX, COIN_ROWS, SPACE_MIN, SPACE_MAX = 384, 16, 2000.0, 9000.0
ROW, GAP, SIDE, SIDE_MAX = 4, 170.0, 0.45, 700.0
PROBE, BELOW, ABOVE = 150.0, 400.0, 150.0
START, BOX, STEEP, LONG, JOIN, ROW_MIN = 2500.0, 450.0, 0.45, 4.0, 3.0, 3
ROAD = {1, 4, 6, 7, 8, 11, 17}  # coin_road: the attributes a coin goes on (17 water)
WATER = 17
OFF_ROAD = {0, 3, 12, 19}
WALLS = {2, 18}

MAP_CELLS = 900  # the map's longer side at most this many cells (one image pixel each)


def course_name(key, arc):
    cid = int(key[:2], 16)
    name = NAMES.get(cid, f"course {cid:#x}")
    base = os.path.basename(arc or "")
    return f"{name} ({base})" if base else name



class Course:
    """a course's map: a grid of cells, each with its floors (height, attribute) from the collision,
    and its BOL's line, item boxes and start"""

    def __init__(self, key, tris, bol):
        self.key, self.hash = key, int(key[3:], 16)
        xs = [v[0] for vs, _ in tris for v in vs]
        zs = [v[2] for vs, _ in tris for v in vs]
        self.x0, self.z0 = min(xs) - 500, min(zs) - 500
        span = max(max(xs) - self.x0, max(zs) - self.z0) + 500
        self.g = max(30.0, span / MAP_CELLS)
        self.w = int((max(xs) + 500 - self.x0) / self.g) + 1
        self.h = int((max(zs) + 500 - self.z0) / self.g) + 1
        self.cells = {}  # (ix, iz) -> [(y, attr)]
        for vs, t in tris:
            at = t >> 8
            if at in WALLS:
                continue
            (ax, ay, az), (bx, by, bz), (cx, cy, cz) = vs
            ux, uy, uz, vx, vy, vz = bx - ax, by - ay, bz - az, cx - ax, cy - ay, cz - az
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            nl = math.sqrt(nx * nx + ny * ny + nz * nz)
            if nl < 1e-6 or abs(ny) < 0.35 * nl:
                continue  # a wall, or a slope no kart drives
            det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
            if abs(det) < 1e-6:
                continue
            g = self.g
            for ix in range(int((min(ax, bx, cx) - self.x0) / g), int((max(ax, bx, cx) - self.x0) / g) + 1):
                px = self.x0 + (ix + 0.5) * g
                for iz in range(int((min(az, bz, cz) - self.z0) / g), int((max(az, bz, cz) - self.z0) / g) + 1):
                    pz = self.z0 + (iz + 0.5) * g
                    u = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / det
                    v = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / det
                    if u >= -0.01 and v >= -0.01 and u + v <= 1.01:
                        self.cells.setdefault((ix, iz), []).append((u * ay + v * by + (1 - u - v) * cy, at))
        for fl in self.cells.values():
            fl.sort(reverse=True)
        self.read_bol(bol)

    def read_bol(self, bol):
        u16 = lambda o: struct.unpack_from(">H", bol, o)[0]
        u32 = lambda o: struct.unpack_from(">I", bol, o)[0]
        self.cl, self.boxes, self.start = [], [], None
        n, clo = u16(0x1a), u32(0x44)
        if 2 <= n <= 1024 and clo >= 0x7c and clo + 0x20 * n <= len(bol):
            for i in range(n):
                p = clo + 0x20 * i
                x, y, z = struct.unpack_from(">fff", bol, p)
                grp = struct.unpack_from(">h", bol, p + 0xe)[0]
                sc = struct.unpack_from(">f", bol, p + 0x10)[0]
                self.cl.append(((x, y, z), grp, sc if math.isfinite(sc) and sc >= 0 else 0.0))
        nobj, obo = u16(0x1e), u32(0x54)
        for i in range(nobj):
            o = obo + 0x40 * i
            if o + 0x40 > len(bol):
                break
            if u16(o + 0x24) == 1:
                self.boxes.append(struct.unpack_from(">fff", bol, o))
        so = u32(0x58)
        if bol[0x3b] > 0 and so + 12 <= len(bol):
            self.start = struct.unpack_from(">fff", bol, so)

    def floors(self, x, z):
        return self.cells.get((int((x - self.x0) / self.g), int((z - self.z0) / self.g)), [])

    def ground(self, p):
        """coins.c's ground_at: the ground searched from PROBE above p, (height, attribute) or None"""
        for y, at in self.floors(p[0], p[2]):
            if y <= p[1] + PROBE:
                return (y, at) if p[1] - BELOW < y < p[1] + ABOVE else None
        return None

    def floor_near(self, x, z, y=None, top=True):
        """(height, attribute) at x, z: the floor nearest y, else the top (or bottom) one"""
        fl = self.floors(x, z)
        if not fl:
            return None
        if y is not None:
            return min(fl, key=lambda f: abs(f[0] - y))
        return fl[0] if top else fl[-1]

    def steps(self):
        pts = [p for p, _, _ in self.cl]
        n = len(pts)
        d = lambda a, b: math.dist(a, b)
        lens = [d(pts[i], pts[i + 1]) for i in range(n - 1)]
        usual = 0.0
        for L in lens:
            if sum(1 for q in lens if q < L) == len(lens) // 2:
                usual = L
                break
        usual = max(usual, 1.0)
        out, any_g, opened = [], any(g >= 0 for _, g, _ in self.cl), False
        for i in range(n):
            j = i + 1 if i + 1 < n else 0
            closer = False
            if self.cl[i][1] >= 0:
                closer, opened = opened, not opened
            inside = not any_g or (opened and not closer)
            L = d(pts[i], pts[j])
            if (j == 0 or closer or not inside) and L > JOIN * usual:
                continue
            if L >= 1:
                out.append((i, j, L))
        return out, usual

    def generate(self):
        """coins.c's build: [(x, y, z)] (y the ground)"""
        if len(self.cl) < 2:
            return []
        steps, usual = self.steps()
        pts = [p for p, _, _ in self.cl]
        sc = [s for _, _, s in self.cl]
        total = sum(L for _, _, L in steps)
        space = min(max(total / COIN_ROWS, SPACE_MIN), SPACE_MAX)
        seed = self.hash or 0x9E3779B9
        coins, carry = [], space * 0.5
        for ia, ib, L in steps:
            if len(coins) + ROW > COIN_MAX:
                break
            a, b = pts[ia], pts[ib]
            dy = b[1] - a[1]
            hz = math.hypot(b[0] - a[0], b[2] - a[2])
            bad = hz < 1 or abs(dy) > STEEP * hz or L > LONG * usual
            t = carry
            while t < L and len(coins) + ROW <= COIN_MAX:  # (room for a whole row)
                if not bad:
                    seed ^= (seed << 13) & 0xFFFFFFFF
                    seed ^= seed >> 17
                    seed ^= (seed << 5) & 0xFFFFFFFF
                    u = t / L
                    dv = ((b[0] - a[0]) / L, dy / L, (b[2] - a[2]) / L)
                    side = (dv[2] / (hz / L), 0.0, -dv[0] / (hz / L))
                    w = sc[ia] + (sc[ib] - sc[ia]) * u
                    off = max(-SIDE_MAX, min(SIDE_MAX, ((seed % 5) - 2) * 0.5 * SIDE * w))
                    mid = [a[i] + (b[i] - a[i]) * u for i in range(3)]
                    if not (self.start and math.dist(mid, self.start) < START):
                        row = []
                        for _ in range(2):
                            row = []
                            for k in range(ROW):
                                along = (k - (ROW - 1) * 0.5) * GAP
                                p = [mid[i] + dv[i] * along + side[i] * off for i in range(3)]
                                if any(math.dist(p, q) < BOX for q in self.boxes):
                                    continue
                                gr = self.ground(p)
                                if gr and gr[1] in ROAD:
                                    row.append((p[0], gr[0], p[2]))
                            if len(row) >= ROW_MIN:
                                break
                            off = 0.0
                        if len(row) >= ROW_MIN:
                            coins += row
                t += space
            carry = math.fmod(carry - L, space)
            if carry < 0:
                carry += space
        return coins

    def line_dir(self, x, z):
        """the karts' line's direction (x, z) nearest x, z"""
        best, out = None, (1.0, 0.0)
        pts = [p for p, _, _ in self.cl]
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            dx, dz = b[0] - a[0], b[2] - a[2]
            L2 = dx * dx + dz * dz
            if L2 < 1 or L2 > 4e8:
                continue
            t = max(0.0, min(1.0, ((x - a[0]) * dx + (z - a[2]) * dz) / L2))
            e = (a[0] + t * dx - x) ** 2 + (a[2] + t * dz - z) ** 2
            if best is None or e < best:
                best, out = e, (dx / math.sqrt(L2), dz / math.sqrt(L2))
        return out


def read_layout(path):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split("#", 1)[0].split()
        if f and f[0] == "coin":
            f = f[1:]
        if len(f) >= 3 and re.fullmatch(r"[-+.\d]", f[0][0]):
            try:
                p = tuple(float(v) for v in f[:3])
            except ValueError:
                continue
            if all(math.isfinite(v) and abs(v) < 1e6 for v in p):
                out.append(p)
    return out


class Editor:
    def __init__(self, root, coins_dir=None, disc=None, mod=None):
        self.root, self.dir, self.disc, self.mod = root, None, None, None
        self.courses, self.course, self.key = {}, None, None
        self.coins, self.undo, self.dirty, self.drag = [], [], False, None
        self.zoom_i, self.image, self.base, self.where = 0, None, None, ""
        self.found, self.keys = {}, []
        self.embedded = root.winfo_toplevel() is not root
        if not self.embedded:
            root.title(TITLE)
        last = load_settings()
        self.dir_v = tk.StringVar(value=coins_dir or last.get("coins") or "")
        self.disc_v = tk.StringVar(value=disc or last.get("disc") or "")
        self.mod_v = tk.StringVar(value=mod or last.get("mod") or "")
        top = ttk.Frame(root, padding=(8, 8, 8, 0))
        top.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(top, text="Places the coins on each race course (GAME > COINS). 1) Pick the coins folder: "
                            "switch/MKDD-NX/coins on the SD card (create it if there is none), or a coins folder next to "
                            "MKDD-NX.exe. 2) Pick your disc image (only read, for the course maps) and, for a course mod's "
                            "courses, its folder (the one holding files). 3) Pick a course and place its coins.",
                  wraplength=900, justify="left").grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 6))
        rows = (("Coins folder:", self.dir_v, self.pick_dir), ("Disc image (only read):", self.disc_v, self.pick_disc),
                ("Mod folder (optional):", self.mod_v, self.pick_mod))
        for r, (label, var, cmd) in enumerate(rows, start=1):
            ttk.Label(top, text=label).grid(row=r, column=0, sticky="w")
            e = ttk.Entry(top, textvariable=var, width=90)
            e.grid(row=r, column=1, sticky="we", padx=4)
            e.bind("<Return>", lambda ev: self.scan())
            ttk.Button(top, text="Browse…", command=cmd).grid(row=r, column=2)
        ttk.Button(top, text="None", command=lambda: (self.mod_v.set(""), self.scan())).grid(row=3, column=3, padx=(4, 0))
        top.columnconfigure(1, weight=1)
        bar = ttk.Frame(root)
        bar.pack(side=tk.TOP, fill=tk.X)
        self.pick = ttk.Combobox(bar, values=[], state="readonly", width=44)
        self.pick.pack(side=tk.LEFT, padx=4, pady=4)
        self.pick.bind("<<ComboboxSelected>>", lambda e: self.load(self.pick.get().split()[0]))
        ttk.Label(bar, text="Row").pack(side=tk.LEFT, padx=(10, 2))
        self.row = tk.IntVar(value=4)
        ttk.Spinbox(bar, from_=1, to=8, width=3, textvariable=self.row, state="readonly").pack(side=tk.LEFT)
        self.show_line = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="Karts' line", variable=self.show_line, command=self.draw).pack(side=tk.LEFT, padx=(10, 2))
        for text, cmd in (("Remove file", self.remove), ("Save", self.save), ("Reload", lambda: self.key and self.load(self.key, True)),
                          ("Clear", self.clear), ("Generate", self.generate)):
            ttk.Button(bar, text=text, command=cmd).pack(side=tk.RIGHT, padx=4)
        self.status = tk.StringVar(value="")
        ttk.Label(root, textvariable=self.status, anchor=tk.W).pack(side=tk.BOTTOM, fill=tk.X)
        self.canvas = c = tk.Canvas(root, width=900, height=760, background=BG, highlightthickness=0)
        c.pack(fill=tk.BOTH, expand=True)
        c.bind("<ButtonPress-1>", self.press)
        c.bind("<B1-Motion>", self.motion)
        c.bind("<ButtonRelease-1>", self.release)
        rb, mb = ("2", "3") if sys.platform == "darwin" else ("3", "2")
        c.bind(f"<ButtonPress-{rb}>", self.right)
        c.bind(f"<B{rb}-Motion>", lambda e: self.drag and self.drag[0] == "pan" and c.scan_dragto(e.x, e.y, gain=1))
        c.bind(f"<ButtonPress-{mb}>", lambda e: (setattr(self, "drag", ("pan",)), c.scan_mark(e.x, e.y)))
        c.bind(f"<B{mb}-Motion>", lambda e: c.scan_dragto(e.x, e.y, gain=1))
        c.bind(f"<ButtonRelease-{rb}>", self.release)
        c.bind(f"<ButtonRelease-{mb}>", self.release)
        c.bind("<MouseWheel>", lambda e: self.zoom(1 if e.delta > 0 else -1, e))
        c.bind("<Button-4>", lambda e: self.zoom(1, e))
        c.bind("<Button-5>", lambda e: self.zoom(-1, e))
        c.bind("<Motion>", self.hover)
        root.winfo_toplevel().bind("<Control-z>", lambda e: self.root.winfo_ismapped() and self.do_undo())
        if not self.embedded:
            root.protocol("WM_DELETE_WINDOW", lambda: self.can_close() and root.destroy())
        self.scan()

    # --- the folders: read again whenever one changes ---
    def pick_dir(self):
        p = filedialog.askdirectory(title="Coins folder: switch/MKDD-NX/coins on the SD card, or coins next to MKDD-NX.exe",
                                    initialdir=self.dir_v.get() or None)
        if p:
            self.dir_v.set(p)
            self.scan()

    def pick_disc(self):
        d = os.path.dirname(self.disc_v.get())
        p = filedialog.askopenfilename(title="Your disc image, for the course maps (only read)", initialdir=d if os.path.isdir(d) else None,
                                       filetypes=[("GameCube disc image", " ".join("*" + t for t in DISC_TYPES)), ("All files", "*.*")])
        if p:
            self.disc_v.set(p)
            self.scan()

    def pick_mod(self):
        p = filedialog.askdirectory(title="A course mod's folder (the one holding files)", initialdir=self.mod_v.get() or None)
        if p:
            self.mod_v.set(p)
            self.scan()

    def scan(self):
        """the courses of the folders picked: the disc's and the mod's maps, and the coins folder's files"""
        if not self.unsaved("Change"):  # (cancelled: the fields back to the folders in use)
            for var, was in ((self.dir_v, self.dir), (self.disc_v, self.disc), (self.mod_v, self.mod)):
                var.set(was or "")
            return
        cdir, disc, mod = (v.get().strip() or None for v in (self.dir_v, self.disc_v, self.mod_v))
        notes = []
        if cdir and not os.path.isdir(cdir):
            notes.append(f"No folder {cdir}")
            cdir = None
        if disc and not os.path.isfile(disc):
            notes.append(f"No disc image {disc}")
            disc = None
        if mod and not os.path.isdir(os.path.join(mod, "files")):
            notes.append(f"No files folder in {mod}")
            mod = None
        self.status.set("Reading the courses...")
        self.root.update_idletasks()
        found = {}
        for what, kw in (("disc image", {"disc_path": disc}), ("mod folder", {"mod_dir": mod})):
            if not next(iter(kw.values())):
                continue
            try:
                got = br.game_collision(want=br.is_race_course, with_bol=True, **kw)
            except Exception as e:  # (a damaged or different image or folder: no maps from it)
                notes.append(f"Could not read the {what}: {e}")
                continue
            for k, v in got.items():
                found.setdefault(k, v)
        try:
            files = {f[:11] for f in os.listdir(cdir) if re.fullmatch(r"[0-9a-f]{2}_[0-9a-f]{8}\.txt", f)} if cdir else set()
        except OSError as e:
            notes.append(str(e))
            files = set()
        self.dir, self.disc, self.mod, self.found = cdir, disc, mod, found
        self.keys = sorted(set(found) | files)
        names = [f"{k}  {course_name(k, found[k][2] if k in found else '')}" for k in self.keys]
        self.pick.configure(values=names, height=max(1, min(len(names), (self.root.winfo_screenheight() - 200) // 20)),
                            width=max([44] + [len(n) + 2 for n in names]))
        self.pick.set("")
        self.course, self.key, self.coins, self.undo, self.dirty = None, None, [], [], False
        self.canvas.delete("all")
        save_settings(self.dir_v.get().strip(), self.disc_v.get().strip(), self.mod_v.get().strip())
        need = [t for t, miss in (("pick the coins folder", not cdir), ("pick your disc image", not found)) if miss]
        tail = "; ".join(need).capitalize() if need else f"{len(self.keys)} courses: pick one"
        self.status.set(". ".join(notes + [tail]))

    # --- the view: the map's image at a zoom (1/4 .. 3 image pixels a cell) ---
    ZOOMS = (-4, -3, -2, 1, 2, 3)

    def scale(self, i=None):
        z = self.ZOOMS[self.zoom_i if i is None else i]
        return z if z > 0 else 1.0 / -z

    def to_c(self, x, z):
        s = self.scale() / self.course.g
        return (x - self.course.x0) * s, (z - self.course.z0) * s

    def to_w(self, ex, ey):
        cx, cy = self.canvas.canvasx(ex), self.canvas.canvasy(ey)
        g = self.course.g / self.scale()
        return self.course.x0 + cx * g, self.course.z0 + cy * g

    def make_base(self):
        co = self.course
        tops = {}
        for (ix, iz), fl in co.cells.items():
            at = fl[0][1]
            tops[(ix, iz)] = ("#3a6ea8" if at == WATER else "#8a8a90" if at in ROAD else "#3c6a3c" if at in OFF_ROAD else "#4a3c3c")
        img = tk.PhotoImage(width=co.w, height=co.h)
        img.put(BG, to=(0, 0, co.w, co.h))
        for iz in range(co.h):
            row, run = [], False
            for ix in range(co.w):
                row.append(tops.get((ix, iz), BG))
                run |= (ix, iz) in tops
            if run:
                img.put("{" + " ".join(row) + "}", to=(0, iz))
        self.base = img

    def draw(self):
        c = self.canvas
        c.delete("all")
        if not self.course:
            return
        z = self.ZOOMS[self.zoom_i]
        self.image = self.base.zoom(z) if z > 1 else self.base.subsample(-z) if z < -1 else self.base
        c.create_image(0, 0, image=self.image, anchor=tk.NW)
        co = self.course
        c.configure(scrollregion=(-200, -200, co.w * self.scale() + 200, co.h * self.scale() + 200))
        if self.show_line.get() and len(co.cl) > 1:
            pts = [v for p, _, _ in co.cl for v in self.to_c(p[0], p[2])]
            c.create_line(*pts, *pts[:2], fill="#d0d0d0", width=1, dash=(3, 3))
        for b in co.boxes:
            x, y = self.to_c(b[0], b[2])
            c.create_rectangle(x - 4, y - 4, x + 4, y + 4, outline="#40a0ff", width=2)
        if co.start:
            x, y = self.to_c(co.start[0], co.start[2])
            c.create_oval(x - 6, y - 6, x + 6, y + 6, outline="#40e040", width=3)
        r = max(3, min(7, 60 * self.scale() / co.g))
        for i, p in enumerate(self.coins):
            x, y = self.to_c(p[0], p[2])
            fl = co.floor_near(p[0], p[2], p[1])
            col = "#40e0ff" if fl and fl[1] == WATER else "#ffd030"
            c.create_oval(x - r, y - r, x + r, y + r, fill=col, outline="#7a5a00", tags=("coin", f"c{i}"))

    def zoom(self, d, e):
        if not self.course or not 0 <= self.zoom_i + d < len(self.ZOOMS):
            return
        wx, wz = self.to_w(e.x, e.y)
        self.zoom_i += d
        self.draw()
        cx, cy = self.to_c(wx, wz)  # the point under the mouse stays there
        self.canvas.xview_moveto(0), self.canvas.yview_moveto(0)
        self.canvas.scan_mark(0, 0)
        x0, y0 = self.canvas.canvasx(0), self.canvas.canvasy(0)
        self.canvas.scan_dragto(int(x0 - cx + e.x), int(y0 - cy + e.y), gain=1)

    # --- the coins ---
    def coin_at(self, e):
        r = 8
        cx, cy = self.canvas.canvasx(e.x), self.canvas.canvasy(e.y)
        for item in reversed(self.canvas.find_overlapping(cx - r, cy - r, cx + r, cy + r)):
            for t in self.canvas.gettags(item):
                if t.startswith("c") and t[1:].isdigit():
                    return int(t[1:])
        return None

    def keep(self):
        self.undo.append(list(self.coins))
        del self.undo[:-50]
        self.dirty = True

    def press(self, e):
        if not self.course:
            return
        i = self.coin_at(e)
        if i is not None:
            self.drag = ("coin", i, False)  # (kept for undo once it moves)
            return
        x, z = self.to_w(e.x, e.y)
        top = not (e.state & 1)  # Shift: the lowest floor (under a bridge)
        n = self.row.get()
        dx, dz = self.course.line_dir(x, z)
        new = []
        for k in range(n):
            along = (k - (n - 1) * 0.5) * GAP
            px, pz = x + dx * along, z + dz * along
            fl = self.course.floor_near(px, pz, new[0][1] if new else None, top)
            if fl:
                new.append((px, fl[0], pz))
        room = COIN_MAX - len(self.coins)
        if not new or room <= 0:
            self.status.set("No ground there" if not new else f"At most {COIN_MAX} coins")
            return
        self.keep()
        self.coins += new[:room]
        self.draw()
        self.count()

    def motion(self, e):
        if self.drag and self.drag[0] == "coin":
            i = self.drag[1]
            x, z = self.to_w(e.x, e.y)
            fl = self.course.floor_near(x, z, self.coins[i][1])
            if fl:
                if not self.drag[2]:
                    self.keep()
                    self.drag = ("coin", i, True)
                self.coins[i] = (x, fl[0], z)
                self.draw()

    def release(self, e):
        self.drag = None

    def right(self, e):
        if not self.course:
            return
        i = self.coin_at(e)
        if i is None:
            self.drag = ("pan",)
            self.canvas.scan_mark(e.x, e.y)
            return
        self.drag = None
        self.keep()
        del self.coins[i]
        self.draw()
        self.count()

    def do_undo(self):
        if self.undo:
            self.coins = self.undo.pop()
            self.dirty = True
            self.draw()
            self.count()

    def hover(self, e):
        if self.course and not self.drag:
            x, z = self.to_w(e.x, e.y)
            fl = self.course.floor_near(x, z, top=True)
            where = f"x {x:.0f} z {z:.0f}" + (f" y {fl[0]:.0f}, attribute {fl[1]}" if fl else ", no ground")
            self.status.set(f"{self.key}: {len(self.coins)} coins ({self.where}). {where}")

    def count(self):
        self.status.set(f"{self.key}: {len(self.coins)} coins ({self.where})")

    def generate(self):
        if not self.course:
            return
        if self.coins and not messagebox.askyesno(TITLE, "Replace these coins with the layout the game makes?"):
            return
        self.keep()
        self.coins = self.course.generate()
        self.draw()
        self.status.set(f"{self.key}: {len(self.coins)} coins, the game's own layout (about: it is made again in the game)")

    def clear(self):
        if self.coins:
            self.keep()
            self.coins = []
            self.draw()
            self.count()

    # --- files ---
    def path(self, key=None):
        return os.path.join(self.dir, (key or self.key) + ".txt")

    def unsaved(self, doing):
        if not self.dirty:
            return True
        a = messagebox.askyesnocancel(TITLE, f"Save the coins of {self.key} first?")
        if a is None:
            return False
        return self.save() if a else True

    def can_close(self):
        return self.unsaved("Close")

    def load(self, key, again=False):
        if self.key and not again and not self.unsaved("Load"):
            self.pick.set(next(n for n in self.pick["values"] if n.startswith(self.key)))
            return
        if key not in self.found:
            self.course = None
            self.canvas.delete("all")
            self.key = key
            self.status.set(f"{key}: no map for it (not on the disc image or the mod's folder you picked)")
            return
        self.status.set(f"Loading {key}...")
        self.root.update_idletasks()
        tris, bol, arc = self.found[key]
        if not self.course or self.course.key != key:
            self.course = Course(key, tris, bol)
            self.make_base()
            room = min(max(self.canvas.winfo_width(), 400), max(self.canvas.winfo_height(), 400)) - 40
            self.zoom_i = 0  # the largest zoom the whole course fits in
            while self.zoom_i + 1 < len(self.ZOOMS) and max(self.course.w, self.course.h) * self.scale(self.zoom_i + 1) <= room:
                self.zoom_i += 1
        self.key, self.undo, self.dirty = key, [], False
        self.pick.set(next((n for n in self.pick["values"] if n.startswith(key)), key))
        if self.dir and os.path.exists(self.path()):
            self.coins, self.where = read_layout(self.path())[:COIN_MAX], os.path.basename(self.path())
        else:
            self.coins, self.where = self.course.generate(), "no file: the game's own layout"
        self.draw()
        self.count()

    def save(self):
        if not self.key:
            return False
        if not self.dir:
            messagebox.showwarning(TITLE, "Pick the coins folder first.")
            return False
        p = self.path()
        try:
            if os.path.exists(p):
                os.replace(p, p[:-4] + ".bak")
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(f"# MKDD-NX coins (tools/coin_editor.py): {course_name(self.key, self.found.get(self.key, ('', '', ''))[2])}, "
                        f"{len(self.coins)} coins, x y z a line (y: the ground)\n")
                for x, y, z in self.coins:
                    f.write(f"{x:.1f} {y:.1f} {z:.1f}\n")
        except OSError as e:
            messagebox.showerror(TITLE, f"Could not save {p}: {e}")
            return False
        self.dirty, self.where = False, os.path.basename(p)
        self.status.set(f"Saved {p}: the next race on this course uses it" + (" (no coins there)" if not self.coins else ""))
        return True

    def remove(self):
        if not self.key or not self.dir or not os.path.exists(self.path()):
            self.status.set("No file for this course: it has the game's own layout")
            return
        if not messagebox.askyesno(TITLE, f"Delete {self.path()}? The course goes back to the layout the game makes "
                                          "(the file is kept as .bak)."):
            return
        try:
            os.replace(self.path(), self.path()[:-4] + ".bak")
        except OSError as e:
            messagebox.showerror(TITLE, f"Could not remove {self.path()}: {e}")
            return
        self.load(self.key, True)


def load_settings():
    try:
        d = json.load(open(SETTINGS, encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(cdir, disc, mod):
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        json.dump({"coins": cdir, "disc": disc, "mod": mod}, open(SETTINGS, "w", encoding="utf-8"))
    except OSError:
        pass


def open_editor(frame):
    """the editor in frame (MKDD-NX Tools' tab): its folders picked on the tab"""
    return Editor(frame)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("coins", nargs="?")
    ap.add_argument("--disc", help="your disc image (.iso/.gcm/.ciso/.rvz), for the course maps")
    ap.add_argument("--mod", help="a course mod's folder (the one holding files/)")
    ap.add_argument("--dump", metavar="KEY", help="no window: print the layout the game makes for KEY (a check)")
    a = ap.parse_args()
    if a.dump:
        got = br.game_collision(a.disc, a.mod, want=br.is_race_course, with_bol=True)
        tris, bol, _ = got[a.dump]
        for p in Course(a.dump, tris, bol).generate():
            print("%.1f %.1f %.1f" % p)
        return
    root = tk.Tk()
    Editor(root, a.coins, a.disc, a.mod)
    root.mainloop()


if __name__ == "__main__":
    main()
