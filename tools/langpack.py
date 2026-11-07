#!/usr/bin/env python3
"""MKDD-NX Tools' Languages tab: copies the French, German, Italian and Spanish files out of the
player's own PAL disc image (GM4P01: .iso, .gcm, .ciso or .rvz) for MKDD-NX's MODS > LANGUAGE.
Nothing else from the disc is read or kept.

    py -3 tools/langpack.py <PAL disc image> <output folder>

writes <output folder>/lang/<Language>/{AwardData,CourseName,MRAM_Locale,SceneData}/..., the PAL
disc's <folder>/<Language>/ files: the lang folder goes in MKDD-NX's mods folder
(sdmc:/switch/MKDD-NX/mods/lang on the Switch, mods\\lang next to MKDD-NX.exe). runtime/disc.c adds
them to the USA game's file table and runtime/hle.c sets the game's language.
"""
import os
import struct
import sys

LANGS = ("French", "German", "Italian", "Spanish")
TOPS = ("AwardData", "CourseName", "MRAM_Locale", "SceneData")
NEEDED = ("SceneData/title.arc", "MRAM_Locale/MRAMLoc.arc")  # what MKDD-NX checks a language folder by


def open_disc(path):
    """any of the disc image formats MKDD-NX reads (tools/build_from_iso.py's reader)"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_from_iso
    try:
        return build_from_iso.Disc(path)
    except SystemExit as e:  # its errors end a command line run; here they are the tab's message
        raise ValueError(str(e).replace("error:", "").strip())
    except OSError as e:
        raise ValueError(f"could not read {path}: {e.strerror or e}")


def disc_files(d):
    """[(path as on the disc, offset, size)] from the disc's file table"""
    fo, fs = struct.unpack(">II", d.read(0x424, 8))
    if not 12 <= fs < 16 << 20:
        raise ValueError("the disc image's file table is damaged")
    fst = d.read(fo, fs)
    n = struct.unpack(">I", fst[8:12])[0]
    if n * 12 > len(fst):
        raise ValueError("the disc image's file table is damaged")
    strs, out, ends, stack = n * 12, [], [], []
    for i in range(1, n):
        while ends and i >= ends[-1]:
            ends.pop()
            stack.pop()
        w, a, b = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        o = strs + (w & 0xFFFFFF)
        end = fst.find(b"\0", o)
        if o >= len(fst) or end < 0:
            raise ValueError("the disc image's file table is damaged")
        name = fst[o:end].decode("latin-1")
        if w >> 24:
            if b <= i or b > n:
                raise ValueError("the disc image's file table is damaged")
            stack.append(name)
            ends.append(b)
        else:
            out.append(("/".join(stack + [name]), a, b))
    return out


def extract(disc_path, out_dir, log=print):
    """the PAL disc's languages -> out_dir/lang/<Language>/...; returns {language: files written}"""
    d = open_disc(disc_path)
    gid = d.read(0, 6)
    if gid[:3] != b"GM4":
        raise ValueError(f"{os.path.basename(disc_path)} is not Mario Kart: Double Dash!! (game ID {gid.decode('latin-1', 'replace')})")
    files = {}
    for path, off, size in disc_files(d):
        parts = path.split("/")
        top = next((t for t in TOPS if t.lower() == parts[0].lower()), None)
        lang = next((x for x in LANGS if len(parts) == 3 and x.lower() == parts[1].lower()), None)
        if top and lang:
            files.setdefault(lang, []).append((f"{top}/{parts[2]}", off, size))
    if not files:
        raise ValueError(f"game {gid.decode('latin-1')} has no French, German, Italian or Spanish files: "
                         "they are on the European (PAL) disc, GM4P01")
    done = {}
    for lang in LANGS:
        have = {p.lower() for p, _, _ in files.get(lang, [])}
        if not all(x.lower() in have for x in NEEDED):
            log(f"{lang}: not on this disc")
            continue
        base = os.path.join(out_dir, "lang", lang)
        for rel, off, size in files[lang]:
            dst = os.path.join(base, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            data = d.read(off, size)
            if len(data) != size:
                raise ValueError(f"{rel}: the disc image is cut short")
            with open(dst + ".part", "wb") as f:  # whole files only: a cancelled copy leaves no half file
                f.write(data)
            os.replace(dst + ".part", dst)
        done[lang] = len(files[lang])
        log(f"{lang}: {len(files[lang])} files")
    if not done:
        raise ValueError("no complete language on this disc")
    return done


def gui(root):
    """the Languages tab (MKDD-NX Tools, tools/mkddnx_tools.py) in frame root; returns the tool"""
    import json
    import threading
    import types
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    cfg_path = os.path.join(os.path.expanduser("~"), ".mkdd_nx_charpack.json")  # shared with the other tabs
    try:
        cfg = json.load(open(cfg_path))
    except (OSError, ValueError):
        cfg = {}
    iso_v, out_v = tk.StringVar(value=cfg.get("pal_iso", "")), tk.StringVar(value=cfg.get("lang_out", ""))
    state = {"busy": False}

    def save_cfg():
        try:
            cfg.update(json.load(open(cfg_path)))
        except (OSError, ValueError):
            pass
        cfg.update(pal_iso=iso_v.get(), lang_out=out_v.get())
        try:
            json.dump(cfg, open(cfg_path, "w"))
        except OSError:
            pass

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    ttk.Label(top, text="Copies the French, German, Italian and Spanish menus and texts from your own European (PAL) disc "
                        "image, for MKDD-NX's MODS > LANGUAGE. Only those files are read. Put the lang folder it writes in "
                        "MKDD-NX's mods folder, then pick the language in the - menu (MODS > LANGUAGE) or the Windows "
                        "launcher, and restart.", wraplength=900, justify="left").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))

    def pick_iso():
        p = filedialog.askopenfilename(title="Your PAL disc image (only read)",
                                       filetypes=[("Disc image", "*.iso *.gcm *.ciso *.rvz"), ("All", "*.*")])
        if p:
            iso_v.set(p)
            save_cfg()

    def pick_out():
        p = filedialog.askdirectory(title="Where to write the lang folder")
        if p:
            out_v.set(p)
            save_cfg()

    for r, (label, var, cmd) in enumerate((("PAL disc image:", iso_v, pick_iso), ("Output folder:", out_v, pick_out)), start=1):
        ttk.Label(top, text=label).grid(row=r, column=0, sticky="w")
        ttk.Entry(top, textvariable=var, width=80).grid(row=r, column=1, sticky="we", padx=6, pady=2)
        ttk.Button(top, text="Browse...", command=cmd).grid(row=r, column=2)
    top.columnconfigure(1, weight=1)
    go = ttk.Button(top, text="Extract languages")
    go.grid(row=3, column=1, sticky="w", padx=6, pady=(8, 4))
    text = tk.Text(root, height=12, wrap="word", state="disabled")
    text.pack(fill="both", expand=True, padx=8, pady=(0, 8))

    def say(msg):  # from the worker thread too
        def put():
            text.configure(state="normal")
            text.insert("end", msg + "\n")
            text.see("end")
            text.configure(state="disabled")
        root.after(0, put)

    def run():
        disc, out = iso_v.get().strip(), out_v.get().strip()
        if not disc or not os.path.isfile(disc):
            messagebox.showerror("Languages", "Pick your PAL disc image first.")
            return
        if not out:
            messagebox.showerror("Languages", "Pick the output folder first.")
            return
        save_cfg()
        state["busy"] = True
        go.configure(state="disabled")
        say(f"Reading {disc}...")

        def work():
            try:
                done = extract(disc, out, say)
                say(f"Done: {', '.join(done)}. Copy {os.path.join(out, 'lang')} into MKDD-NX's mods folder "
                    "(mods/lang), then pick the language in MODS > LANGUAGE and restart.")
            except (ValueError, OSError) as e:
                say(f"Error: {e}")
            except Exception as e:  # (anything else: shown, not lost in the thread)
                say(f"Error: {type(e).__name__}: {e}")
            finally:
                state["busy"] = False
                root.after(0, lambda: go.configure(state="normal"))
        threading.Thread(target=work, daemon=True).start()

    go.configure(command=run)

    def can_close():
        return not state["busy"] or messagebox.askyesno("Languages", "The languages are still being copied. Close anyway?")
    return types.SimpleNamespace(can_close=can_close)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    try:
        extract(argv[0], argv[1])
    except (ValueError, OSError) as e:
        print(f"error: {e}")
        return 1
    print(f"wrote {os.path.join(argv[1], 'lang')}: copy it into MKDD-NX's mods folder")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
