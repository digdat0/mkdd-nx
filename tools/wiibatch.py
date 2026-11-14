"""Batch conversion of Mario Kart Wii drivers and karts (.szs) into MKDD-NX characters and extra karts: the
MKDD-NX Tools tab "Batch WiiMK" (tools/mkddnx_tools.py), laid out as the Characters tab (tools/charpack.py).

Add .szs files or folders of mods (or drop them on the window): every distinct driver becomes a character
(tools/wiichar.py), every distinct kart an extra kart (tools/wiikart.py); a mod's many files of the same model
give one row. Check each row and its preview, change what it plays as / is added next to, then Export:
characters/<id>/ and karts/<id>/ in the output folder, for the SD card's /switch/MKDD-NX/mods/.

    py -3 tools/wiibatch.py            (the window on its own)
"""
import hashlib
import json
import os
import re
import sys
import threading
import types

import charpack
import wiichar
import wiikart
import ui_theme  # noqa: E402

TITLE = "Batch WiiMK (beta)"
CHAR, KART = "Character", "Kart"
FORMATS = ("MKDD-NX (Switch)", "GameCube (MKDD Patcher mods, characters only)")


class Item:
    def __init__(self, kind, szs, name):
        self.kind, self.szs, self.name = kind, szs, name
        self.author = self.source = ""
        self.base, self.auto = None, True  # BASES index (wiichar's or wiikart's); auto: picked by the converter
        self.files, self.notes, self.fit, self.game = None, [], None, None
        self.status = "queued"
        self.top = None  # the folder it was added with (its mod)


def candidates(paths):
    """[(.szs, the folder added, kinds)] worth reading under paths. Per folder, from the names (sa_kart-kk.szs:
    kart sa, driver kk): a character mod ships its driver in every kart (the karts stock), a kart mod its kart
    for every driver (the drivers stock), so the code that varies less is the mod's: one file per driver code
    (CHAR) or per kart code (KART); both for a single file. Never menu or all-kart files; a folder whose names
    say nothing: every file, both kinds."""
    found = []
    for p in paths:
        if os.path.isfile(p):
            if p.lower().endswith(".szs"):
                found.append((p, os.path.dirname(p)))
            continue
        for d, _, fs in os.walk(p):
            for f in fs:
                if f.lower().endswith(".szs"):
                    found.append((os.path.join(d, f), p))
    firsts = {}  # a folder added that is one mod (not a folder of mods): its own name names it
    for f, top in found:
        firsts.setdefault(top, set()).add(os.path.relpath(f, top).replace("\\", "/").split("/")[0] if os.path.dirname(f) != top else "")
    found = [(f, os.path.dirname(top) if len(firsts[top]) == 1 and os.path.dirname(top) != top else top) for f, top in found]

    def code(f):
        n = os.path.basename(f).lower()
        m = re.match(r"(.+?_(?:kart|bike)[^-]*)-([a-z]+)", n)
        return (m.group(1), m.group(2), n) if m and "allkart" not in n else None
    by_dir, tops_named = {}, set()
    for f, top in found:
        by_dir.setdefault(os.path.dirname(f).lower(), []).append((f, top))
        if code(f):
            tops_named.add(mod_top(f, top))
    out = []
    for group in by_dir.values():
        named = [(f, t) + code(f) for f, t in group if code(f)]
        if not named:  # (a mod's menu or award files beside its named ones: not drivers to convert)
            out += [(f, t, (CHAR, KART)) for f, t in group if mod_top(f, t) not in tops_named]
            continue
        drivers, karts = {x[3] for x in named}, {x[2] for x in named}
        kinds = (CHAR, KART) if len(drivers) == len(karts) else (CHAR,) if len(karts) > len(drivers) else (KART,)
        best = {}
        for f, t, kart, drv, n in named:
            key = drv if kinds == (CHAR,) else kart if kinds == (KART,) else (kart, drv)
            rank = (bool(re.search(r"_\d\.szs$", n)), "_bike" in n, n)  # the standard file of a kart before its variants
            if key not in best or rank < best[key][0]:
                best[key] = (rank, (f, t, kinds))
        out += [v[1] for v in best.values()]
    return sorted(set(out), key=lambda x: (len(x[2]), x[0]))  # one-kind folders first: their stock models are learnt


def mod_top(szs, top):
    """the mod folder a file is in: the first folder below what was added"""
    parts = os.path.relpath(szs, top).replace("\\", "/").split("/")
    return os.path.join(top, parts[0]).lower() if len(parts) > 1 else os.path.dirname(szs).lower()


def mod_name(szs, top):
    """the mod's folder name (the first folder below what was added), else the file's"""
    rel = os.path.relpath(szs, top)
    parts = rel.replace("\\", "/").split("/")
    if len(parts) > 1:
        return parts[0]
    d = os.path.basename(os.path.dirname(szs))
    return d or wiikart.default_name(szs)


def convert_char(it, data, disc):
    hint = wiichar.guess_base(it.szs)
    picked = []
    if it.auto:
        k, why = wiichar.pick_base(data, disc, charpack.base_index(hint) if hint else None) if disc else (None, "")
        it.base = k if k is not None else (charpack.base_index(hint) if hint else None)
        if k is not None:
            picked.append(("info", f"plays as {charpack.BASES[k][1]}: {why}"))
        if it.base is None:
            raise wiichar.ConvertError("which Double Dash character it plays as isn't known: pick one below" +
                                       ("" if disc else " (or set the disc image)"))
    files, notes = wiichar.convert(data, it.base, disc)
    game = charpack.yaz0(disc.driver_file(it.base, "driver.bmd") or b"") if disc else None
    pics, more = wiichar.icons(it.szs, files, game or None)
    files.update(pics)
    it.files, it.notes, it.game = files, picked + notes + more, game or None


def convert_kart(it, data, disc):
    if not disc:
        raise wiichar.ConvertError("karts need the disc image (the Double Dash kart it is fitted to)")
    files, notes, b, fit = wiikart.convert(data, None if it.auto else it.base, disc, 1.0, 0.0, 0.0, it.szs)
    it.files, it.notes, it.base, it.fit = files, notes, b, fit


def convert(it, disc):
    it.status = "converting"
    try:
        with open(it.szs, "rb") as f:
            data = f.read()
        (convert_char if it.kind == CHAR else convert_kart)(it, data, disc)
        levels = [lv for lv, _ in it.notes]
        it.status = "check" if "warn" in levels or "error" in levels else "ok"
    except (wiichar.ConvertError, OSError, ValueError) as e:
        it.files, it.notes, it.status = None, [("error", f"can't convert: {e}")], "error"
    except Exception as e:  # a file this doesn't understand: say so, don't take the window with it
        it.files, it.notes, it.status = None, [("error", f"can't convert ({type(e).__name__}: {e})")], "error"


def models_in(szs):
    """{'driver': brres bytes, 'kart': brres bytes} of the file (those it has)"""
    with open(szs, "rb") as f:
        files = wiichar.u8_files(charpack.yaz0(f.read()))
    out = {}
    for k, v in files.items():
        n = k.split("/")[-1]
        if n == wiichar.DRIVER:
            out["driver"] = v
        elif n == wiikart.KART_MODEL:
            out["kart"] = v
    return out


def slug(name, taken, fallback):
    s = "".join(ch if ch.isalnum() else "-" for ch in name.lower()).strip("-") or fallback
    s = re.sub("-+", "-", s)
    k, n = s, 2
    while k in taken:
        k, n = f"{s}-{n}", n + 1
    taken.add(k)
    return k


def export(items, out, fmt_gc=False, as_zip=True):
    """writes the converted items -> (characters written, karts written, [notes])"""
    taken = {"characters": set(), "karts": set()}
    nc = nk = 0
    notes = []
    for it in items:
        if not it.files:
            continue
        if it.kind == CHAR:
            if fmt_gc:
                wiichar.write_gc_mod(out, it.files, it.name, it.base, it.author, it.source, as_zip)
            else:
                wiichar.write_character(os.path.join(out, "characters", slug(it.name, taken["characters"], "wii-character")),
                                        it.files, it.name, it.base, it.author, it.source)
            nc += 1
        elif fmt_gc:
            notes.append(f"{it.name}: karts are for MKDD-NX only, not written")
        else:
            wiikart.write_kart(os.path.join(out, "karts", slug(it.name, taken["karts"], "wii-kart")), it.files, it.name, it.base,
                               it.author, it.source)
            nk += 1
    return nc, nk, notes


def gui(root=None):
    """the window; in root, a frame (MKDD-NX Tools' tab), when given: then returns the tool (its can_close())"""
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    cfg_path = os.path.join(os.path.expanduser("~"), ".mkdd_nx_charpack.json")  # shared with the other tabs
    try:
        cfg = json.load(open(cfg_path))
    except (OSError, ValueError):
        cfg = {}
    own = root is None
    root = root or tk.Tk()
    if own:
        root.title("MKDD-NX " + TITLE)
        root.geometry("1100x720")
    items = []
    state = {"disc": None, "queue": [], "busy": False, "seen": set(), "stock": set(), "stop": False}

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    iso_v, out_v = tk.StringVar(value=cfg.get("iso", "")), tk.StringVar(value=cfg.get("wii_batch_out", cfg.get("wii_out", "")))
    fmt_v = tk.StringVar(value=cfg.get("wii_batch_format") if cfg.get("wii_batch_format") in FORMATS else FORMATS[0])
    pack_v = tk.StringVar(name=charpack.GC_PACK_VAR, value=cfg.get("gc_pack") if cfg.get("gc_pack") in charpack.GC_PACKS
                          else charpack.GC_PACKS[0])

    def save_cfg():
        try:  # the other tabs' settings, as saved since this one opened
            cfg.update(json.load(open(cfg_path)))
        except (OSError, ValueError):
            pass
        cfg.update(iso=iso_v.get(), wii_batch_out=out_v.get(), wii_batch_format=fmt_v.get(), gc_pack=pack_v.get())
        try:
            json.dump(cfg, open(cfg_path, "w"))
        except OSError:
            pass

    def pick_iso():
        p = filedialog.askopenfilename(title="Your MKDD disc image (only read)", filetypes=[("Disc image", "*.iso *.gcm"), ("All", "*.*")])
        if p:
            iso_v.set(p)
            open_disc(again=True)

    def open_disc(again=False):
        state["disc"] = None
        if iso_v.get():
            try:
                state["disc"] = charpack.Disc(iso_v.get())
            except (OSError, ValueError) as e:
                messagebox.showerror("Disc image", str(e))
        save_cfg()
        if again:
            reconvert_all()

    def pick_out():
        p = filedialog.askdirectory(title="Where to write the characters/ and karts/ folders")
        if p:
            out_v.set(p)
            save_cfg()

    ttk.Label(top, text="Converts many Mario Kart Wii drivers and karts at once. 1) Pick your disc image (only read: the "
                        "skeletons and karts they are fitted to). 2) Add .szs files or folders of mods, or drop them on this "
                        "window: each distinct driver becomes a character, each kart an extra kart. 3) Check each one and "
                        "what it plays as / is added next to. 4) Export, then copy characters/ and karts/ to the SD card's "
                        "/switch/MKDD-NX/mods/.", wraplength=1000, justify="left").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))
    for r, (label, var, cmd) in enumerate((("Disc image (to match):", iso_v, pick_iso), ("Output folder:", out_v, pick_out)), start=1):
        ttk.Label(top, text=label).grid(row=r, column=0, sticky="w")
        ttk.Entry(top, textvariable=var, width=90).grid(row=r, column=1, sticky="we", padx=4)
        ttk.Button(top, text="Browse…", command=cmd).grid(row=r, column=2)
    ttk.Label(top, text="Output format:").grid(row=3, column=0, sticky="w")
    fmt_row = ttk.Frame(top)
    fmt_row.grid(row=3, column=1, sticky="w", padx=4)
    fmt_box = ttk.Combobox(fmt_row, textvariable=fmt_v, values=FORMATS, state="readonly", width=44)
    fmt_box.pack(side="left")
    pack_label = ttk.Label(fmt_row, text="as")
    pack_box = ttk.Combobox(fmt_row, textvariable=pack_v, values=charpack.GC_PACKS, state="readonly", width=8)

    def fmt_changed(e=None):
        if fmt_v.get() == FORMATS[1]:
            pack_label.pack(side="left", padx=(8, 4))
            pack_box.pack(side="left")
        else:
            pack_label.pack_forget()
            pack_box.pack_forget()
        save_cfg()
    fmt_box.bind("<<ComboboxSelected>>", fmt_changed)
    pack_box.bind("<<ComboboxSelected>>", lambda e: save_cfg())
    fmt_changed()
    top.columnconfigure(1, weight=1)

    bar = ttk.Frame(root, padding=(8, 0))
    bar.pack(fill="x")
    mid = ttk.PanedWindow(root, orient="vertical")
    mid.pack(fill="both", expand=True, padx=8, pady=8)
    tree = ttk.Treeview(mid, columns=("name", "kind", "base", "author", "status", "from"), show="headings", selectmode="browse")
    for col, title, w in (("name", "Name", 190), ("kind", "Type", 80), ("base", "Plays as / next to", 130),
                          ("author", "Author", 110), ("status", "Status", 80), ("from", "From", 380)):
        tree.heading(col, text=title)
        tree.column(col, width=w)
    mid.add(tree, weight=3)

    detail = ttk.Frame(mid, padding=4)
    mid.add(detail, weight=2)
    name_v, author_v, source_v, base_v = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar()
    fields = ttk.Frame(detail)
    fields.pack(fill="x")
    for col, (label, var) in enumerate((("Name", name_v), ("Author", author_v), ("Source link", source_v))):
        ttk.Label(fields, text=label).grid(row=0, column=2 * col, sticky="w")
        ttk.Entry(fields, textvariable=var, width=26).grid(row=0, column=2 * col + 1, padx=4)
    base_label = ttk.Label(fields, text="Plays as")
    base_label.grid(row=0, column=6)
    base_box = ttk.Combobox(fields, textvariable=base_v, state="readonly", width=16, height=len(wiikart.BASES))
    base_box.grid(row=0, column=7, padx=4)
    body = ttk.Frame(detail)
    body.pack(fill="both", expand=True, pady=(6, 0))
    pics = ttk.Frame(body)
    pics.pack(side="right", anchor="n", padx=(8, 0))
    notes = tk.Text(body, height=8, wrap="word")
    notes.pack(side="left", fill="both", expand=True)
    for level, colour in ui_theme.NOTES.items():
        notes.tag_configure(level, foreground=colour)

    # the selected row's model, front and back, each with its own turn (as the Characters tab), and the
    # other half of its mod: a character's kart, a kart's driver (the row of the other kind from the same file,
    # else from the same mod folder)
    VIEW = 220
    views = []
    prev = {"parts": None, "pair": None, "seq": 0, "it": None}

    def partner(it):
        other = KART if it.kind == CHAR else CHAR
        same = [x for x in items if x.kind == other and x.szs == it.szs]
        mod = [x for x in items if x.kind == other and it.top and x.top == it.top]
        for x in same + mod:
            return x
        return None

    def show_view(v):
        parts = prev[v["src"]]
        if not parts:
            return
        v["seq"] += 1
        seq, yaw = v["seq"], v["yaw"].get()

        def work():
            try:
                pic = wiichar.render(parts, VIEW, yaw)
            except Exception:  # a picture is only a help
                pic = None
            root.after(0, lambda: put(pic))

        def put(pic):
            if seq != v["seq"] or pic is None or prev[v["src"]] is not parts:
                return
            from PIL import Image, ImageTk
            sheet = Image.new("RGBA", (VIEW, VIEW), (70, 80, 95, 255))
            sheet.alpha_composite(pic)
            v["photo"] = ImageTk.PhotoImage(sheet)
            v["label"].configure(image=v["photo"], text="", width=VIEW, height=VIEW)

        threading.Thread(target=work, daemon=True).start()

    def no_picture(text, src=None):
        for v in views:
            if src and v["src"] != src:
                continue
            v["seq"] += 1
            v["photo"] = None
            v["label"].configure(image="", text=text, width=30, height=14)

    def parts_of(it):
        try:
            return wiichar.preview_parts(it.files, it.game) if it.kind == CHAR else wiikart.preview_parts(it.files, it.base, it.fit)
        except Exception:
            return None

    def preview(it):
        prev["seq"] += 1
        prev["parts"] = prev["pair"] = None
        seq = prev["seq"]
        mate = partner(it) if it else None
        if it:
            pair_title.configure(text="Its kart" if it.kind == CHAR else "Its driver")
        if not it or not it.files:
            no_picture("" if not it else "converting…" if it.status in ("queued", "converting") else "(no picture)")
            return
        no_picture("drawing…", "parts")
        no_picture("drawing…" if mate and mate.files else
                   "converting…" if mate and mate.status in ("queued", "converting") else
                   ("(no kart of its own in this mod)" if it.kind == CHAR else "(no driver of its own in this mod)") if not mate else
                   "(no picture)", "pair")

        def work():
            parts = parts_of(it)
            pair = parts_of(mate) if mate and mate.files else None
            root.after(0, lambda: shown(parts, pair))

        def shown(parts, pair):
            if seq != prev["seq"]:
                return
            prev["parts"], prev["pair"] = parts, pair
            if not parts:
                no_picture("(no picture)", "parts")
            for v in views:
                if prev[v["src"]]:
                    show_view(v)

        threading.Thread(target=work, daemon=True).start()

    def turned(v):
        v["angle"].configure(text=f"{v['yaw'].get():3d}°")
        if v.get("timer"):
            root.after_cancel(v["timer"])
        v["timer"] = root.after(60, lambda: show_view(v))

    pair_title = None
    for k, yaw in enumerate((25, 205, 25)):  # front, back; the other half's front
        col = ttk.Frame(pics)
        col.pack(side="left", padx=(0, 6))
        if k == 2:
            pair_title = ttk.Label(col, text="Its kart")
        ttk.Label(col, text=("Front", "Back")[k]).pack(side="top") if k < 2 else pair_title.pack(side="top")
        v = {"yaw": tk.IntVar(value=yaw), "seq": 0, "src": "pair" if k == 2 else "parts"}
        v["label"] = tk.Label(col, text="Pick a row" if k == 0 else "", width=30, height=14,
                              relief="groove", background=ui_theme.PREVIEW[0], foreground=ui_theme.PREVIEW[1])
        v["label"].pack(side="top")
        row = ttk.Frame(col)
        row.pack(side="top", fill="x")
        ttk.Label(row, text="Turn").pack(side="left")
        ttk.Scale(row, from_=0, to=359, variable=v["yaw"], orient="horizontal",
                  command=lambda _x, v=v: (v["yaw"].set(int(float(v["yaw"].get()))), turned(v))).pack(side="left", fill="x", expand=True, padx=4)
        v["angle"] = ttk.Label(row, text=f"{yaw:3d}°", width=5)
        v["angle"].pack(side="left")
        views.append(v)

    def base_name(it):
        if it.base is None:
            return "?"
        return (charpack.BASES if it.kind == CHAR else wiikart.BASES)[it.base][1]

    def row_values(it):
        return (it.name, it.kind, base_name(it), it.author, it.status, it.szs)

    def refresh():
        sel = tree.selection()
        tree.delete(*tree.get_children())
        for i, it in enumerate(items):
            tree.insert("", "end", iid=str(i), values=row_values(it))
        if sel and tree.exists(sel[0]):
            tree.selection_set(sel)
        counts()

    def counts():
        nc = sum(it.kind == CHAR for it in items)
        nk = len(items) - nc
        left = sum(it.status in ("queued", "converting") for it in items)
        count_v.set(f"{nc} characters, {nk} karts" + (f" ({left} converting)" if left or state["busy"] else ""))

    def update_row(it):
        if it in items:
            i = str(items.index(it))
            if tree.exists(i):
                tree.item(i, values=row_values(it))
        counts()
        if prev["it"] is it:
            show()
        elif prev["it"] and partner(prev["it"]) is it:  # the other half just converted: its picture too
            preview(prev["it"])

    def current():
        s = tree.selection()
        return items[int(s[0])] if s and int(s[0]) < len(items) else None

    def show(_=None):
        it = current()
        notes.delete("1.0", "end")
        changed = it is not prev["it"] or (it and prev.get("files") is not it.files)
        prev["it"], prev["files"] = it, it.files if it else None
        if changed:
            preview(it)
        if not it:
            return
        name_v.set(it.name), author_v.set(it.author), source_v.set(it.source)
        base_label.configure(text="Plays as" if it.kind == CHAR else "Next to")
        bases = charpack.BASES if it.kind == CHAR else wiikart.BASES
        base_box.configure(values=[b[1] for b in bases])
        base_v.set(base_name(it) if it.base is not None else "")
        for level, text in it.notes:
            notes.insert("end", f"{level.upper():6}{text}\n", level)
        if it.files:
            notes.insert("end", "\nFiles: " + ", ".join(sorted(it.files)))
        elif it.status in ("queued", "converting"):
            notes.insert("end", "converting…", "info")

    def apply(*_):
        it = current()
        if not it:
            return
        it.name, it.author, it.source = name_v.get().strip() or it.name, author_v.get().strip(), source_v.get().strip()
        update_row(it)

    def base_picked(_=None):
        it = current()
        if not it or state["busy"] and it.status == "converting":
            return
        b = charpack.base_index(base_v.get()) if it.kind == CHAR else next((i for i, x in enumerate(wiikart.BASES) if x[1] == base_v.get()), None)
        if b is None or b == it.base and not it.auto:
            return
        it.base, it.auto = b, False
        queue([it])

    def worker():
        while True:
            if state["stop"] or not state["queue"]:
                state["busy"] = False
                root.after(0, counts)
                return
            job = state["queue"].pop(0)
            disc = state["disc"]
            if isinstance(job, Item):
                root.after(0, lambda j=job: update_row(j))
                convert(job, disc)
                root.after(0, lambda j=job: update_row(j))
                continue
            szs, top, kinds = job  # a new file: what it has, then a row for each new model
            try:
                found = models_in(szs)
            except Exception:
                found = {}
            new = []
            for key, kind in (("driver", CHAR), ("kart", KART)):
                if key not in found or kind not in kinds or kind == KART and "_bike" in os.path.basename(szs).lower():
                    continue
                h = (kind, hashlib.sha1(found[key]).hexdigest())
                if h in state["seen"] or h in state["stock"]:
                    continue
                state["seen"].add(h)
                name = mod_name(szs, top)
                it = Item(kind, szs, name if kind == CHAR else name + " kart")
                it.hash, it.top = h, top
                new.append(it)
            if len(kinds) == 1:  # a character mod's karts, a kart mod's drivers: the Wii's own, never rows
                other = ("kart", KART) if kinds == (CHAR,) else ("driver", CHAR)
                if other[0] in found:
                    h = (other[1], hashlib.sha1(found[other[0]]).hexdigest())
                    if h not in state["stock"]:
                        state["stock"].add(h)
                        root.after(0, lambda h=h: drop_stock(h))
            if new:
                done_ev = threading.Event()

                def add_rows(new=new):
                    items.extend(new)
                    refresh()
                    done_ev.set()
                root.after(0, add_rows)
                done_ev.wait(5)
                state["queue"][:0] = new  # converted next, before the next file is read

    def drop_stock(h):
        gone = [it for it in items if getattr(it, "hash", None) == h]
        for it in gone:
            if it in state["queue"]:
                state["queue"].remove(it)
            items.remove(it)
        if gone:
            refresh()

    def queue(jobs):
        for j in jobs:
            if isinstance(j, Item):
                j.status = "queued"
                update_row(j)
        state["queue"] += jobs
        if not state["busy"]:
            state["busy"] = True
            state["stop"] = False
            threading.Thread(target=worker, daemon=True).start()
        counts()

    def add_paths(paths):
        found = candidates(paths)
        if not found:
            messagebox.showinfo(TITLE, "No .szs files there.")
            return
        queue(found)

    def add_files():
        ps = filedialog.askopenfilenames(title="Mario Kart Wii driver or kart files", filetypes=[("Wii archive", "*.szs"), ("All", "*.*")])
        if ps:
            add_paths(list(ps))

    def add_folder():
        p = filedialog.askdirectory(title="A Wii mod, or a folder of Wii mods")
        if p:
            add_paths([p])

    def remove():
        it = current()
        if it:
            if it in state["queue"]:
                state["queue"].remove(it)
            items.remove(it)
            prev["it"] = None
            refresh()
            show()

    def reconvert_all():
        queue([it for it in items if it.status not in ("queued", "converting")])

    def do_export():
        if state["busy"]:
            messagebox.showinfo(TITLE, "Still converting: export when the list is done.")
            return
        if not out_v.get():
            pick_out()
        if not out_v.get() or not items:
            return
        bad = [it.name for it in items if not it.files]
        if bad and not messagebox.askyesno(TITLE, f"{len(bad)} rows didn't convert and are left out:\n" + "\n".join(bad[:20]) +
                                           ("\n…" if len(bad) > 20 else "") + "\n\nExport the rest?"):
            return
        gc = fmt_v.get() == FORMATS[1]
        try:
            nc, nk, warn = export(items, out_v.get(), gc, pack_v.get() == "zip")
        except (OSError, ValueError) as e:
            messagebox.showerror(TITLE, str(e))
            return
        if gc:
            msg = (f"{nc} MKDD Patcher mods written to\n{out_v.get()}\n\nAdd them in the MKDD Patcher and patch a copy of your disc "
                   "image. Wii models can be bigger than the game's: try them in Dolphin first.")
        else:
            msg = (f"{nc} characters (characters/) and {nk} karts (karts/) written to\n{out_v.get()}\n\nCopy those folders into the "
                   "SD card's /switch/MKDD-NX/mods/. Karts: turn on EXTRA KARTS in the Mods menu and press the kart change "
                   "button on the select screen.")
        if warn:
            msg += "\n\n" + "\n".join(warn[:10]) + ("\n…" if len(warn) > 10 else "")
        messagebox.showinfo(TITLE, msg)

    for text, cmd in (("Add .szs files…", add_files), ("Add folder of mods…", add_folder), ("Remove", remove),
                      ("Convert again", reconvert_all), ("Export…", do_export)):
        ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=(0, 6))
    count_v = tk.StringVar(value="Drop .szs files or mod folders on this window" if sys.platform == "win32" else "Add .szs files or mod folders")
    ttk.Label(bar, textvariable=count_v).pack(side="left", padx=12)
    charpack.enable_drop(root.winfo_toplevel(), add_paths, root.winfo_ismapped)
    tree.bind("<<TreeviewSelect>>", show)
    for w in fields.winfo_children():
        if isinstance(w, ttk.Entry):
            w.bind("<Return>", apply)
            w.bind("<FocusOut>", apply)
    base_box.bind("<<ComboboxSelected>>", base_picked)
    if iso_v.get():
        root.after(100, open_disc)

    def can_close():
        if state["busy"] and not messagebox.askyesno(TITLE, "A conversion is running. Close anyway?"):
            return False
        state["stop"] = True
        return True
    if own:
        root.mainloop()
    return types.SimpleNamespace(open=lambda p: add_paths([p]), can_close=can_close)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    gui()
