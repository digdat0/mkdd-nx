#!/usr/bin/env python3
"""A Mario Kart Wii kart (a .szs from Race/Kart, e.g. sa_kart-kk.szs) -> an MKDD-NX extra kart folder.

The Wii file's kart_model.brres has the kart in parts as Double Dash's does: body, tire_fl and tire_fr
are kept, each on the joints of the Double Dash kart it is added next to (the "base"; tools/wiichar.py
reads and writes the models). The base decides how it drives and where its wheels turn: the game places
the wheels itself, so the body is fitted to the base's size and the tires to its wheel size. Arms and
shocks stay the base's. Bikes have one front and one rear wheel and no place in Double Dash: refused.

    py -3 tools/wiikart.py KART.szs OUT_FOLDER --iso DISC.iso [--base mario] [--name NAME]
                           [--size 100] [--up 0] [--forward 0]

The result goes in the SD card's /switch/MKDD-NX/mods/karts/. The disc image is only read.
"""
import os
import re
import struct
import sys

import charpack
import wiichar
from wiichar import ConvertError, IDENT, Mesh, POS, Skeleton

KART_MODEL = "kart_model.brres"
# Double Dash's kart folders (runtime/charpack.c KP_NAMES) by who drives them
BASES = (("mario", "Mario"), ("dk", "DK"), ("yoshi", "Yoshi"), ("noko", "Koopa Troopa"), ("peach", "Peach"), ("baby", "Baby Mario"),
         ("wario", "Wario"), ("koopa", "Bowser"), ("luigi", "Luigi"), ("diddy", "Diddy Kong"), ("cathy", "Birdo"), ("pata", "Paratroopa"),
         ("daisy", "Daisy"), ("bblui", "Baby Luigi"), ("walui", "Waluigi"), ("jr", "Bowser Jr."), ("kpio", "Toad"), ("kpico", "Toadette"),
         ("teres", "King Boo"), ("pakku", "Petey"), ("extra", "Parade Kart"))
SIX_WHEELS = ("koopa", "diddy")  # more wheels than a Wii kart: only when asked for
# Which Double Dash kart a Wii kart is added next to, by its file name's kart (sa_kart-kk.szs: sa): the Wii
# size is the game's weight class (KartInfo.cpp's SKartDB), and each Wii kart has its own kart of that class
# so that they don't all drive alike, the one whose size is nearest of those not taken (heavy has too few).
WII_BASES = {"sa": "pata", "sb": "kpio", "sc": "bblui", "sd": "jr", "se": "kpico", "sdf": "noko",
             "ma": "cathy", "mb": "walui", "mc": "mario", "md": "luigi", "me": "yoshi", "mdf": "peach",
             "la": "wario", "lb": "wario", "lc": "dk", "ld": "teres", "le": "pakku", "ldf": "dk"}
WII_FILE = re.compile(r"(sdf|mdf|ldf|[sml][a-e])_kart", re.I)
BMD_LIMIT = 3 << 20  # runtime/native.c: the heap one extra kart's model is loaded on
SIZE_RANGE = (0.5, 1.6)  # the body's fitted scale is kept in this


def base_index(text):
    """a kart folder or driver name -> its place in BASES (None: no such kart)"""
    t = (text or "").strip().lower()
    return next((i for i, (k, n) in enumerate(BASES) if t in (k, n.lower())), None)


MAX_HEIGHT = 2.0  # of the base's body: the Booster Seat's canopy stands out of the kart screen's frame


def kart_models(szs):
    """a Wii kart archive (bytes) -> ({body, tire_fl, tire_fr: Model}, {name: Tex})"""
    try:
        files = wiichar.u8_files(charpack.yaz0(szs))
    except (IndexError, struct.error) as e:
        raise ConvertError(f"a broken archive ({e})")
    brres = next((v for k, v in files.items() if k.split("/")[-1] == KART_MODEL), None)
    if brres is None:
        raise ConvertError(f"no {KART_MODEL} (a Mario Kart Wii kart file is named like sa_kart-kk.szs)")
    fold = wiichar.brres_folders(brres)
    groups = dict(fold.get("3DModels(NW4R)", []))
    if "body" not in groups:
        raise ConvertError("the file has no kart body model (a low detail or menu file?)")
    # a three-wheeler has one front tire: its rear pair is a left and a right
    left, right = ("tire_fl", "tire_fr") if "tire_fl" in groups else ("tire_rl", "tire_rr")
    if left not in groups or right not in groups:
        raise ConvertError("no left and right tire models: a bike? Only karts can be converted")
    models = {"body": wiichar.Model(brres, groups["body"], "body"), "tire_fl": wiichar.Model(brres, groups[left], left),
              "tire_fr": wiichar.Model(brres, groups[right], right)}
    tex = {n: wiichar.Tex(brres, o) for n, o in fold.get("Textures(NW4R)", [])}
    pals = dict(fold.get("Palettes(NW4R)", []))
    for n, t in tex.items():
        if t.ci:
            if t.fmt not in (8, 9, 10) or n not in pals:
                raise ConvertError(f"texture {n} has a palette that isn't in the file")
            t.depalette(wiichar.palette(brres, pals[n]))
    if not any(m.layers for m in models["body"].mats):
        raise ConvertError("the kart body draws nothing (an invisible kart)")
    return models, tex


def root_turn(m):
    """the rotation of an MDL0's root joint (the Booster Seat lies on its side in the file), None if it has none"""
    w = m.bones[0].world if m.bones else IDENT
    return None if all(abs(w[r][c] - IDENT[r][c]) < 1e-3 for r in range(3) for c in range(3)) else [(*w[r][:3], 0.0) for r in range(3)]


def model_box(m):
    """([x, y, z] low, high) of what an MDL0 draws, in its rest pose"""
    lo, hi = [1e30] * 3, [-1e30] * 3
    def add(p):
        for k in range(3):
            lo[k], hi[k] = min(lo[k], p[k]), max(hi[k], p[k])
    if root_turn(m):  # its vertices stand in their joints' frames
        for d in m.draws:
            ob = m.objs[d[1]]
            for pk in ob.packets:
                for _, vs in pk.prims:
                    for v in vs:
                        mid = pk.slots[v.get(wiichar.PNMTX, 0) // 3] if ob.single < 0 else ob.single
                        add(wiichar.point(m.bones[m.mtx_node[mid]].world, m.pos[ob.arrays[POS]][v[POS]]))
    else:
        for a in {m.objs[d[1]].arrays[POS] for d in m.draws}:
            for p in m.pos.get(a, ()):
                add(p)
    if lo[0] > hi[0]:
        raise ConvertError(f"the {m.name} model has no vertices")
    return lo, hi


def mesh_box(mesh):
    """([x, y, z] low, high) of a game model's vertices without its root joint's place: the game puts the
    root where the kart is (Peach's kart has its root at 0 and Mario's at 57, their bodies at the same height)"""
    lo, hi = [1e30] * 3, [-1e30] * 3
    w = mesh.matrices()
    root = wiichar.inverse(w[0])
    for _, tri in mesh.tris:
        for dm, pi, *_rest in tri:
            p = wiichar.point(wiichar.mul(root, mesh.draw_matrix(w, dm)), mesh.arr[POS][pi])
            for k in range(3):
                lo[k], hi[k] = min(lo[k], p[k]), max(hi[k], p[k])
    if lo[0] > hi[0]:
        raise ConvertError("the game's kart model has no vertices")
    return lo, hi


def game_part(disc, kart, part):
    """the game's model of a part of a kart (bytes), None if it has none"""
    disc.driver_file(0, "driver.bmd")  # (MRAM.arc read)
    d = disc._mram.get((kart + "_all", f"{kart}_k_{part}.bmd"))
    return charpack.yaz0(d) if d and d[:4] == b"Yaz0" else d


def reach(disc, kart, body_hi):
    """how far out a body may reach on each side: to about the base's wheels, which stand an arm's length out
    (a Wii body often covers its wheels; Mario's kart body is narrow, with them outside)"""
    arm = game_part(disc, kart, "arm")
    return body_hi[0] + (-mesh_box(Mesh(arm))[0][0] if arm else 0.0)


def wii_base(file_name):
    """the kart for a Wii kart file's name -> (index in BASES, the name's kart), None when the name doesn't say"""
    m = WII_FILE.search(os.path.basename(file_name or ""))
    return (base_index(WII_BASES[m.group(1).lower()]), m.group(1).lower()) if m else None


def pick_base(models, disc):
    """the 4-wheeled kart nearest the Wii body's size -> index in BASES"""
    lo, hi = model_box(models["body"])
    best, score = 0, 1e30
    for i, (kart, _) in enumerate(BASES):
        if kart in SIX_WHEELS or kart == "extra":
            continue
        b = game_part(disc, kart, "body")
        if not b:
            continue
        blo, bhi = mesh_box(Mesh(b))
        d = abs((hi[0] - lo[0]) - reach(disc, kart, bhi) * 2) + abs((hi[2] - lo[2]) - (bhi[2] - blo[2]))
        if d < score:
            best, score = i, d
    return best


def on_joints(game_bmd, model, tex, notes, what):
    """model built on the joints of game_bmd (its root and any effect joints, ef_exh_l ...), kept as the game's
    but for the root, which stands at 0 with the vertices around it"""
    sk = wiichar.skeleton_of_bmd(game_bmd)
    quiet = []
    built = wiichar.build(model, Skeleton(sk.names, sk.parents, sk.kinds), tex, quiet, what)
    notes.extend(n for n in quiet if n[0] != "info" or "joints added" not in n[1])
    while len(built.tex) > 1 and built.tex[-1] is built.tex[0]:  # the places a driver's face animations may point at
        built.tex.pop()
    mesh = Mesh(game_bmd)
    for i, j in enumerate(built.joints):
        s, r, t = wiichar.decompose(mesh.local[i])
        built.joints[i] = (j[0], j[1], j[2], (1.0, 1.0, 1.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)) if i == 0 else (j[0], j[1], j[2], s, r, t)
    return built


def place(built, scale, move, turn=None):
    """every vertex of built turned, scaled, then moved (the parts' boxes with them: the game culls by them)"""
    def at(p):
        p = wiichar.point(turn, p) if turn else p
        return tuple(p[k] * scale + move[k] for k in range(3))
    def box(bb):
        ps = [at((x, y, z)) for x in (bb[0][0], bb[1][0]) for y in (bb[0][1], bb[1][1]) for z in (bb[0][2], bb[1][2])]
        return [[min(p[k] for p in ps) for k in range(3)], [max(p[k] for p in ps) for k in range(3)]]
    built.pos = [at(p) for p in built.pos]
    if turn:
        built.nrm = [wiichar.point(turn, n) for n in built.nrm]
    built.shapes = [(a, b, c, d, box(bb) if bb else bb) for a, b, c, d, bb in built.shapes]
    if built.bbox:
        built.bbox = box(built.bbox)


def convert(szs, base, disc, size=1.0, up=0.0, forward=0.0, file_name=""):
    """a Wii kart archive (bytes) next to base (index in BASES, None: by file_name's Wii kart, else the nearest) ->
    ({file: bytes}, notes, base, fit). size, up, forward: the player's adjustments of the fit"""
    notes = []
    models, tex = kart_models(szs)
    if base is None:
        by_name = wii_base(file_name)
        if by_name:
            base = by_name[0]
            notes.append(("info", f"added next to {BASES[base][1]}'s kart, for the Wii file's {by_name[1]} class and kart (pick another below)"))
        else:
            base = pick_base(models, disc)
            notes.append(("warn", f"the file's name doesn't give its Wii kart (like sa_kart): added next to {BASES[base][1]}'s kart, the nearest in size (pick another below)"))
    kart = BASES[base][0]
    game_body = game_part(disc, kart, "body")
    game_l, game_r = game_part(disc, kart, "tire_l"), game_part(disc, kart, "tire_r")
    if not game_body or not game_l or not game_r:
        raise ConvertError(f"the disc image has no {kart} kart")
    # the body: as long and wide as the base's, on its floor
    lo, hi = model_box(models["body"])
    blo, bhi = mesh_box(Mesh(game_body))
    fit = min(reach(disc, kart, bhi) * 2 / max(hi[0] - lo[0], 1e-3), (bhi[2] - blo[2]) / max(hi[2] - lo[2], 1e-3),
              MAX_HEIGHT * (bhi[1] - blo[1]) / max(hi[1] - lo[1], 1e-3))
    if not SIZE_RANGE[0] <= fit <= SIZE_RANGE[1]:
        notes.append(("warn", f"its body is very different in size from the base's: scaled {min(max(fit, SIZE_RANGE[0]), SIZE_RANGE[1]):.2f}, "
                              f"not the {fit:.2f} that fits. Try another kart to add it next to"))
    fit = min(max(fit, SIZE_RANGE[0]), SIZE_RANGE[1]) * size
    move = [(bhi[0] + blo[0] - (hi[0] + lo[0]) * fit) / 2, blo[1] - lo[1] * fit + up, (bhi[2] + blo[2] - (hi[2] + lo[2]) * fit) / 2 + forward]
    body = on_joints(game_body, models["body"], tex, notes, "body")
    place(body, fit, move, root_turn(models["body"]))
    out = {f"{kart}_k_body.bmd": wiichar.shrink_textures(body, notes)}
    notes.append(("ok", f"body: {len(body.pos)} vertices, {len(body.mats)} materials, {len(body.tex)} textures, scaled {fit:.2f}"))
    # the tires: the size of the base's. Wii's left and right are Double Dash's (their hubs face the same way)
    radius = (Mesh(game_l).arr[POS] and mesh_box(Mesh(game_l))[1][1]) or 1.0
    for wii, game, part in (("tire_fl", game_l, "tire_l"), ("tire_fr", game_r, "tire_r")):
        tl, th = model_box(models[wii])
        k = radius / max((th[1] - tl[1]) / 2, 1e-3)
        t = on_joints(game, models[wii], tex, notes, part)
        place(t, k, [0.0, -(th[1] + tl[1]) / 2 * k, -(th[2] + tl[2]) / 2 * k])
        out[f"{kart}_k_{part}.bmd"] = wiichar.shrink_textures(t, notes)
    notes.append(("ok", f"tires: {radius * 2:.0f} tall, as {BASES[base][1]}'s"))
    notes.append(("info", f"arms and shocks are {BASES[base][1]}'s; so are how it drives, its class and where its wheels go"))
    if kart in SIX_WHEELS:
        notes.append(("warn", f"{BASES[base][1]}'s kart has six wheels: all get the Wii tires"))
    for f, d in out.items():
        if len(d) >= BMD_LIMIT:
            notes.append(("warn", f"{f} is {len(d) / (1 << 20):.1f} MB: too big for the game, which would show the stock part"))
    return out, notes, base, {"box": (blo, bhi), "radius": radius}


def preview_parts(files, base, fit):
    """what wiichar.render() draws: the body and tires, the wheels where the base's body suggests (the game
    has the real places)"""
    kart = BASES[base][0]
    body = Mesh(files[f"{kart}_k_body.bmd"])
    parts = [(body, body.matrices())]
    blo, bhi = fit["box"]
    cz, half = (bhi[2] + blo[2]) / 2, (bhi[2] - blo[2]) / 2 * 0.62
    side, floor = bhi[0] * 0.95, blo[1] + fit["radius"] * 0.4
    for part, x in (("tire_l", side), ("tire_r", -side)):
        t = Mesh(files[f"{kart}_k_{part}.bmd"])
        for z in (cz + half, cz - half):
            parts.append((t, t.matrices(base=[[1.0, 0.0, 0.0, x], [0.0, 1.0, 0.0, floor], [0.0, 0.0, 1.0, z]])))
    return parts


def write_kart(out_dir, files, name, base, author="", source=""):
    """an extra kart's folder (runtime/charpack.c kart_scan)"""
    kart_dir = os.path.join(out_dir, BASES[base][0] + "_all")
    os.makedirs(kart_dir, exist_ok=True)
    for f, data in files.items():
        with open(os.path.join(kart_dir, f), "wb") as fh:
            fh.write(data)
    with open(os.path.join(out_dir, "modinfo.ini"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"modname={name}\nauthor={author}\nsource={source}\n")


def default_name(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem.split("(")[0].strip() or stem


def gui(root):
    """the Wii Karts tab (MKDD-NX Tools, tools/mkddnx_tools.py) in frame root; returns the tool"""
    import json
    import threading
    import types
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    TITLE = "Convert WiiMK Karts (beta)"
    cfg_path = os.path.join(os.path.expanduser("~"), ".mkdd_nx_charpack.json")  # the disc image: the Characters tab's
    try:
        cfg = json.load(open(cfg_path))
    except (OSError, ValueError):
        cfg = {}
    AUTO = "By Wii kart"
    szs_v, iso_v, out_v = tk.StringVar(), tk.StringVar(value=cfg.get("iso", "")), tk.StringVar(value=cfg.get("wii_kart_out", ""))
    name_v, author_v, source_v, base_v = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar(value=AUTO)
    size_v, up_v, fwd_v = tk.IntVar(value=100), tk.IntVar(value=0), tk.IntVar(value=0)
    state = {"files": None, "busy": False, "again": False, "base": None, "parts": None}

    def save_cfg():
        try:  # the other tabs' settings, as saved since this one opened
            cfg.update(json.load(open(cfg_path)))
        except (OSError, ValueError):
            pass
        cfg.update(iso=iso_v.get(), wii_kart_out=out_v.get())
        try:
            json.dump(cfg, open(cfg_path, "w"))
        except OSError:
            pass

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    ttk.Label(top, text="Converts a Mario Kart Wii kart (.szs, from Race/Kart, e.g. sa_kart-kk.szs) into an extra kart for MKDD-NX, added "
                        "next to a Double Dash kart and driving like it. Karts only, not bikes. Needs your disc image. Credit the "
                        "model's author below.", wraplength=900, justify="left").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))

    def set_szs(p):
        szs_v.set(p)
        name_v.set(default_name(p))
        run()

    def pick_szs():
        p = filedialog.askopenfilename(title="A Mario Kart Wii kart file", filetypes=[("Wii archive", "*.szs"), ("All", "*.*")])
        if p:
            set_szs(p)

    def pick_iso():
        p = filedialog.askopenfilename(title="Your MKDD disc image (only read)", filetypes=[("Disc image", "*.iso *.gcm"), ("All", "*.*")])
        if p:
            iso_v.set(p)
            save_cfg()
            run()

    def pick_out():
        p = filedialog.askdirectory(title="Where to write the kart folder")
        if p:
            out_v.set(p)
            save_cfg()

    for r, (label, var, cmd) in enumerate((("Wii kart file:", szs_v, pick_szs), ("Disc image:", iso_v, pick_iso),
                                           ("Output folder:", out_v, pick_out)), start=1):
        ttk.Label(top, text=label).grid(row=r, column=0, sticky="w")
        ttk.Entry(top, textvariable=var, width=90).grid(row=r, column=1, sticky="we", padx=4)
        ttk.Button(top, text="Browse…", command=cmd).grid(row=r, column=2)
    top.columnconfigure(1, weight=1)
    fields = ttk.Frame(root, padding=(8, 0))
    fields.pack(fill="x")
    for col, (label, var, wd) in enumerate((("Name", name_v, 22), ("Author", author_v, 20), ("Source link", source_v, 30))):
        ttk.Label(fields, text=label).grid(row=0, column=2 * col, sticky="w")
        ttk.Entry(fields, textvariable=var, width=wd).grid(row=0, column=2 * col + 1, padx=4)
    fit_row = ttk.Frame(root, padding=(8, 6, 8, 0))
    fit_row.pack(fill="x")
    ttk.Label(fit_row, text="Added next to").pack(side="left")
    base_box = ttk.Combobox(fit_row, textvariable=base_v, values=[AUTO] + [b[1] for b in BASES], state="readonly", width=14, height=len(BASES) + 1)
    base_box.pack(side="left", padx=(4, 12))
    base_box.bind("<<ComboboxSelected>>", lambda e: run())
    for label, var, lo, hi, step in (("Size %", size_v, 50, 150, 5), ("Up", up_v, -60, 60, 5), ("Forward", fwd_v, -80, 80, 5)):
        ttk.Label(fit_row, text=label).pack(side="left")
        ttk.Spinbox(fit_row, textvariable=var, from_=lo, to=hi, increment=step, width=5, command=lambda: run()).pack(side="left", padx=(4, 12))
    bar = ttk.Frame(root, padding=8)
    bar.pack(fill="x")
    mid = ttk.Frame(root, padding=(8, 0, 8, 8))
    mid.pack(fill="both", expand=True)
    views_row = ttk.Frame(mid)
    views_row.pack(side="top", anchor="w")
    VIEW = 320
    views = []

    def show_view(v):
        parts = state.get("parts")
        if not parts:
            return
        v["seq"] += 1
        seq, yaw = v["seq"], v["yaw"].get()

        def work():
            try:
                pic = wiichar.render(parts, VIEW, yaw)
            except Exception:  # a picture is only a help: never take the window with it
                pic = None
            root.after(0, lambda: put(pic))

        def put(pic):
            from PIL import Image, ImageTk
            if seq != v["seq"] or pic is None:
                return
            sheet = Image.new("RGBA", (VIEW, VIEW), (70, 80, 95, 255))
            sheet.alpha_composite(pic)
            v["photo"] = ImageTk.PhotoImage(sheet)
            v["label"].configure(image=v["photo"], text="", width=VIEW, height=VIEW)

        threading.Thread(target=work, daemon=True).start()

    def turned(v):
        v["angle"].configure(text=f"{v['yaw'].get():3d}°")
        if v.get("timer"):
            root.after_cancel(v["timer"])
        v["timer"] = root.after(60, lambda: show_view(v))

    for k, yaw in enumerate((35, 215)):  # front, back
        col = ttk.Frame(views_row)
        col.pack(side="left", padx=(0, 8))
        v = {"yaw": tk.IntVar(value=yaw), "seq": 0}
        v["label"] = tk.Label(col, text="The converted kart shows here" if k == 0 else "", width=44, height=20,
                              relief="groove", background="#46505f", foreground="#ddd")
        v["label"].pack(side="top")
        row = ttk.Frame(col)
        row.pack(side="top", fill="x")
        ttk.Label(row, text="Turn").pack(side="left")
        ttk.Scale(row, from_=0, to=359, variable=v["yaw"], orient="horizontal",
                  command=lambda _x, v=v: (v["yaw"].set(int(float(v["yaw"].get()))), turned(v))).pack(side="left", fill="x", expand=True, padx=4)
        v["angle"] = ttk.Label(row, text=f"{yaw:3d}°", width=5)
        v["angle"].pack(side="left")
        views.append(v)
    ttk.Label(mid, text="The wheels shown are placed by eye: the game puts them where the base kart's go.", foreground="#666").pack(side="top", anchor="w")
    notes = tk.Text(mid, height=8, wrap="word")
    notes.pack(side="top", fill="both", expand=True, pady=(8, 0))
    for level, colour in (("ok", "#1a7f37"), ("info", "#555555"), ("warn", "#9a6700"), ("error", "#cf222e")):
        notes.tag_configure(level, foreground=colour)

    def show_notes(items):
        notes.delete("1.0", "end")
        for level, text in items:
            notes.insert("end", f"{level.upper():6}{text}\n", level)

    def run():
        """convert in the background; a change made meanwhile converts again when it ends"""
        if not szs_v.get():
            return
        if state["busy"]:
            state["again"] = True
            return
        if not iso_v.get():
            show_notes([("error", "pick your disc image: the kart is fitted to the one it is added next to")])
            return
        try:
            size, up, fwd = size_v.get() / 100, float(up_v.get()), float(fwd_v.get())
        except (tk.TclError, ValueError):
            show_notes([("error", "Size, Up and Forward are numbers")])
            return
        base = base_index(base_v.get())
        state.update(busy=True, files=None)
        root.config(cursor="watch")
        show_notes([("info", "converting…")])
        path, iso = szs_v.get(), iso_v.get()

        def work():
            try:
                disc = charpack.Disc(iso)
                with open(path, "rb") as f:
                    data = f.read()
                files, items, b, fit = convert(data, base, disc, size, up, fwd, path)
                res = (files, items, preview_parts(files, b, fit), b)
            except (ConvertError, OSError, ValueError) as e:
                res = (None, [("error", f"can't convert: {e}")], None, None)
            except Exception as e:  # a file this doesn't understand: say so, don't take the window with it
                res = (None, [("error", f"can't convert ({type(e).__name__}: {e})")], None, None)
            root.after(0, lambda: done(*res))

        threading.Thread(target=work, daemon=True).start()

    def done(files, items, parts, b):
        state.update(busy=False, files=files, parts=parts, base=b)
        root.config(cursor="")
        show_notes(items)
        for v in views:
            if parts:
                show_view(v)
            else:
                v["seq"] += 1
                v["photo"] = None
                v["label"].configure(image="", text="(no picture)")
        if state["again"]:
            state["again"] = False
            run()

    def write():
        if not state["files"]:
            messagebox.showinfo(TITLE, "Pick a Wii kart file first (it converts when picked).")
            return
        if not out_v.get():
            pick_out()
        if not out_v.get():
            return
        name = name_v.get().strip() or "Wii kart"
        cid = "".join(ch if ch.isalnum() else "-" for ch in name.lower()).strip("-") or "wii-kart"
        dest = os.path.join(out_v.get(), cid)
        if os.path.exists(dest) and not messagebox.askyesno(TITLE, f"{dest} exists. Replace its files?"):
            return
        try:
            write_kart(dest, state["files"], name, state["base"], author_v.get().strip(), source_v.get().strip())
        except OSError as e:
            messagebox.showerror(TITLE, str(e))
            return
        messagebox.showinfo(TITLE, f"{name} written to\n{dest}\n\nCopy that folder to the SD card's /switch/MKDD-NX/mods/karts/, turn on "
                            "EXTRA KARTS in the Mods menu and press the kart change button on the select screen.")

    ttk.Button(bar, text="Convert again", command=run).pack(side="left", padx=(0, 6))
    ttk.Button(bar, text="Write kart folder…", command=write).pack(side="left", padx=(0, 6))
    ttk.Label(bar, text="Drop a .szs on this window" if sys.platform == "win32" else "").pack(side="left", padx=12)
    charpack.enable_drop(root.winfo_toplevel(), lambda paths: paths[0].lower().endswith(".szs") and set_szs(paths[0]), root.winfo_ismapped)
    return types.SimpleNamespace(open=set_szs, can_close=lambda: not state["busy"] or messagebox.askyesno(TITLE, "A conversion is running. Close anyway?"))


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="a Mario Kart Wii kart (.szs) -> an MKDD-NX extra kart folder")
    ap.add_argument("szs")
    ap.add_argument("out", help="the kart folder to write")
    ap.add_argument("--iso", required=True, help="your disc image (only read)")
    ap.add_argument("--base", help="the Double Dash kart it is added next to (default: by the file name's Wii kart, else the nearest in size)")
    ap.add_argument("--name")
    ap.add_argument("--author", default="")
    ap.add_argument("--source", default="")
    ap.add_argument("--size", type=float, default=100, help="the body's size, percent of the fit")
    ap.add_argument("--up", type=float, default=0)
    ap.add_argument("--forward", type=float, default=0)
    ap.add_argument("--picture", help="also write a preview .png here")
    a = ap.parse_args(argv)
    base = None
    if a.base:
        base = base_index(a.base)
        if base is None:
            print(f"no kart {a.base}: {', '.join(b[0] for b in BASES)}")
            return 2
    try:
        with open(a.szs, "rb") as f:
            files, notes, base, fit = convert(f.read(), base, charpack.Disc(a.iso), a.size / 100, a.up, a.forward, a.szs)
    except (ConvertError, OSError, ValueError) as e:
        print(f"can't convert: {e}")
        return 1
    write_kart(a.out, files, a.name or default_name(a.szs), base, a.author, a.source)
    if a.picture:
        wiichar.render(preview_parts(files, base, fit), 480, 35).save(a.picture)
    for level, text in notes:
        print(f"{level:5} {text}")
    print(f"written: {a.out} ({', '.join(sorted(files))})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
