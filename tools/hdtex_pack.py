#!/usr/bin/env python3
"""Convert a Dolphin HD texture pack for Mario Kart: Double Dash!! into MKDD-NX's HD texture file.

    py -3 tools/hdtex_pack.py <pack folder> hd-textures.bin [--jobs N] [--compare DIR] [--limit N]

<pack folder> is the folder Dolphin reads (Load/Textures/GM4E01). Every texture is kept at its full
size and BC7-compressed (the Switch GPU samples BC7 directly) with a full mipmap chain: the pack's
own _mipN files where it has them, box-filtered levels otherwise. Copy the output to
sdmc:/switch/MKDD-NX/mods/hd/ (as hd-textures.bin). Nothing from the pack belongs in the repository.

Dolphin names a replacement tex1_<w>x<h>[_m]_<xxh64 of level 0>[_<xxh64 of the palette>|_$]_<format>
[_arb][_mipN].png; the game texture is matched by the same hash at run time. Other files (copies,
upscaler leftovers, old 32-bit hash names) are skipped, as Dolphin skips them.

Output (little-endian):
    header  'MKDDHDT1', u32 version (1), u32 count, u32 encoding (1 = BC7), u32 0, u64 index offset
    index   count x 40 bytes, sorted by hash: u64 hash, u64 palette hash, u16 w, u16 h (game size),
            u16 W, u16 H (level 0 size), u8 format, u8 flags (1 _m, 2 any palette, 4 _arb),
            u8 levels, u8 0, u32 bytes, u64 offset
    data    per texture: its levels, largest first, each in 4x4 BC7 blocks (16 bytes, row-major)
"""
import argparse
import ctypes
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "bc7enc.c"
LIB = ROOT / "build-tools" / ("bc7enc.dll" if os.name == "nt" else "bc7enc.so")

NAME = re.compile(r"^tex1_(\d+)x(\d+)(_m)?_([0-9a-f]{16})(?:_([0-9a-f]{16}|\$))?_(\d+)(_arb)?(?:_mip(\d+))?\.png$")
MAGIC, VERSION, ENC_BC7 = b"MKDDHDT1", 1, 1
ENTRY = struct.Struct("<QQHHHHBBBBIQ")
HEADER = struct.Struct("<8sIIIIQ")
ALIGN = 512


def fail(msg):
    sys.exit("hdtex_pack: " + msg)


class PackError(Exception):
    """A problem the user can fix (wrong folder, no space, no compiler): the message says what."""


def find_cc():
    cands = [os.environ.get("CC")]
    if os.name == "nt":
        cands += [r"C:\msys64\ucrt64\bin\gcc.exe", r"C:\msys64\mingw64\bin\gcc.exe", r"C:\msys64\clang64\bin\clang.exe"]
    cands += [shutil.which("gcc"), shutil.which("clang"), shutil.which("cc")]
    for c in cands:
        if c and Path(c).exists():
            return c
    return None


def build_encoder(out, native=True):
    """Compile tools/bc7enc.c into the shared library out (native: tuned for this CPU only)."""
    cc = find_cc()
    if not cc:
        raise PackError("no C compiler found to build tools/bc7enc.c (install MSYS2 ucrt64 gcc, or set CC)")
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cc, "-O3"] + (["-march=native"] if native else []) + ["-shared", "-o", str(out), str(SRC)]
    cmd.insert(2, "-static" if os.name == "nt" else "-fPIC")
    env = dict(os.environ)
    env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise PackError("building the BC7 encoder failed:\n" + r.stderr)


def load_encoder():
    if getattr(sys, "frozen", False):  # the HD texture app: the library is bundled next to the code
        lib_path = Path(sys._MEIPASS) / LIB.name
    else:
        lib_path = LIB
        if not LIB.exists() or LIB.stat().st_mtime < SRC.stat().st_mtime:
            build_encoder(LIB)
    lib = ctypes.CDLL(str(lib_path))
    lib.bc7_encode.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
    lib.bc7_encode.restype = None
    return lib


def find_pack(folder):
    """The texture folder to convert: folder itself, or a GM4E01 folder inside it (the pack's
    download usually wraps it in a few folders). None if neither holds Dolphin texture names."""
    folder = Path(folder)
    if folder.name.upper() != "GM4E01":
        for d in sorted(folder.rglob("*")):
            if d.is_dir() and d.name.upper() == "GM4E01":
                folder = d
                break
    for path in folder.rglob("tex1_*.png"):
        if NAME.match(path.name):
            return folder
    return None


def scan(pack):
    """Dolphin replacements in pack: {key: {"files": {level: path}, ...}} plus the skipped files."""
    tex, skipped = {}, []
    for path in sorted(pack.rglob("*")):
        if not path.is_file():
            continue
        m = NAME.match(path.name)
        if not m:
            if path.suffix.lower() in (".png", ".dds"):
                skipped.append(path)
            continue
        w, h, mflag, th, tl, fmt, arb, mip = m.groups()
        key = (int(th, 16), int(tl, 16) if tl and tl != "$" else 0, int(w), int(h), int(fmt))
        e = tex.setdefault(key, {"files": {}, "flags": 0})
        level = int(mip) if mip else 0
        if level in e["files"]:  # the same texture in two folders: first wins
            continue
        e["files"][level] = path
        e["flags"] |= (1 if mflag else 0) | (2 if tl == "$" else 0) | (4 if arb else 0)
    return {k: v for k, v in tex.items() if 0 in v["files"]}, skipped


def half(img):
    w, h = img.size
    pm = img.convert("RGBa")  # premultiplied, so transparent texels don't darken their neighbours
    return pm.resize((max(1, w // 2), max(1, h // 2)), Image.BOX).convert("RGBA")


def levels_of(files):
    """Level images, largest first, down to 1x1."""
    img = Image.open(files[0]).convert("RGBA")
    out = [img]
    w, h = img.size
    i = 1
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        img = None
        if i in files:
            cand = Image.open(files[i]).convert("RGBA")
            if cand.size == (w, h):
                img = cand
        out.append(img or half(out[-1]).resize((w, h), Image.BOX))
        i += 1
    return out


def encode(lib, img):
    w, h = img.size
    buf = ctypes.create_string_buffer(((w + 3) // 4) * ((h + 3) // 4) * 16)
    lib.bc7_encode(img.tobytes(), w, h, buf)
    return buf.raw


def convert(pack, out, jobs=None, progress=None, cancel=None, compare=None, limit=None):
    """Convert the Dolphin pack folder into the HD texture file out. progress(done, total, bytes,
    seconds) is called from worker threads; setting the threading.Event cancel stops early (the
    partial file is removed). Textures that can't be read are skipped. Returns a summary dict."""
    pack, out = Path(pack), Path(out)
    if not pack.is_dir():
        raise PackError(f"{pack} is not a folder")
    lib = load_encoder()
    tex, skipped = scan(pack)
    keys = sorted(tex)
    if limit:
        keys = keys[:limit]
    if not keys:
        raise PackError(f"no Dolphin HD textures in {pack} (expected tex1_<w>x<h>_<hash>_<format>.png files)")
    need = sum(p.stat().st_size for k in keys for p in tex[k]["files"].values())  # BC7 output is about this size
    out.parent.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(out.parent).free
    if free < need * 1.2 + (64 << 20):
        raise PackError(f"not enough free space in {out.parent}: about {need * 1.2 / 2**30:.1f} GB needed, "
                        f"{free / 2**30:.1f} GB free")
    if compare:
        compare = Path(compare)
        compare.mkdir(parents=True, exist_ok=True)
        step = max(1, len(keys) // 12)
        compare_keys = set(keys[::step][:12])
    else:
        compare_keys = set()

    index, failed, lock, done = {}, [], threading.Lock(), [0]
    t0 = time.time()
    tmp = out.with_suffix(out.suffix + ".part")
    try:
        with open(tmp, "wb") as f:
            f.write(b"\0" * (HEADER.size + ENTRY.size * len(keys)))
            f.seek(0, 2)
            f.write(b"\0" * ((-f.tell()) % ALIGN))

            def job(key):
                if cancel is not None and cancel.is_set():
                    return key, None
                try:
                    levels = levels_of(tex[key]["files"])
                except Exception as e:  # a damaged or unreadable PNG: the game keeps that texture's original
                    return key, f"{tex[key]['files'][0].name}: {e}"
                data = b"".join(encode(lib, im) for im in levels)
                if key in compare_keys:
                    im = levels[0]
                    dec = Image.open(io.BytesIO(dds_bc7(im.size, data[: ((im.size[0] + 3) // 4) * ((im.size[1] + 3) // 4) * 16])))
                    side = Image.new("RGBA", (im.size[0] * 2, im.size[1]))
                    side.paste(im, (0, 0))
                    side.paste(dec.convert("RGBA"), (im.size[0], 0))
                    side.save(compare / tex[key]["files"][0].name)
                return key, (levels[0].size, len(levels), data)

            with ThreadPoolExecutor(jobs or os.cpu_count() or 4) as ex:
                futs = [ex.submit(job, k) for k in keys]
                for fu in as_completed(futs):
                    key, res = fu.result()
                    with lock:
                        if isinstance(res, tuple):
                            (W, H), nlev, data = res
                            off = f.tell()
                            f.write(data)
                            f.write(b"\0" * ((-f.tell()) % ALIGN))
                            index[key] = (W, H, nlev, len(data), off)
                        elif res:
                            failed.append(res)
                        done[0] += 1
                        if progress:
                            progress(done[0], len(keys), f.tell(), time.time() - t0)
                    if cancel is not None and cancel.is_set():
                        ex.shutdown(wait=True, cancel_futures=True)
                        break
            if cancel is not None and cancel.is_set():
                raise PackError("cancelled")
            if not index:
                raise PackError("none of the textures could be read:\n" + "\n".join(failed[:5]))

            f.seek(0)
            f.write(HEADER.pack(MAGIC, VERSION, len(index), ENC_BC7, 0, HEADER.size))
            for key in sorted(index):
                th, tl, w, h, fmt = key
                W, H, nlev, size, off = index[key]
                f.write(ENTRY.pack(th, tl, w, h, W, H, fmt, tex[key]["flags"], nlev, 0, size, off))
        os.replace(tmp, out)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise
    return {"textures": len(index), "failed": failed, "skipped": len(skipped), "bytes": out.stat().st_size,
            "seconds": time.time() - t0, "out": out}


def main():
    ap = argparse.ArgumentParser(description="Dolphin HD texture pack -> MKDD-NX BC7 texture file")
    ap.add_argument("pack", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--compare", type=Path, help="write PNG | BC7 side-by-side images of a few textures here")
    ap.add_argument("--limit", type=int, help="convert only the first N textures (testing)")
    ap.add_argument("--build-encoder", type=Path, metavar="LIB",
                    help="only compile the BC7 encoder into LIB, for any CPU of this kind (app packaging)")
    a = ap.parse_args()
    if a.build_encoder:
        build_encoder(a.build_encoder, native=False)
        return

    try:
        a.out.resolve().relative_to(ROOT)
        if "build" not in a.out.resolve().relative_to(ROOT).parts[0]:
            fail("write the output outside the repository (it is game content)")
    except ValueError:
        pass

    def progress(done, total, size, el):
        if done % 50 == 0 or done == total:
            print(f"  {done}/{total}  {size / 2**20:.0f} MB  {el:.0f} s, ~{el / done * (total - done):.0f} s left", flush=True)

    try:
        r = convert(a.pack, a.out, a.jobs, progress, compare=a.compare, limit=a.limit)
    except PackError as e:
        fail(str(e))
    for msg in r["failed"]:
        print("  skipped (unreadable): " + msg)
    print(f"wrote {a.out} ({r['bytes'] / 2**20:.0f} MB, {r['textures']} textures, {r['skipped']} other files skipped) "
          f"in {r['seconds']:.0f} s")


def dds_bc7(size, data):
    w, h = size
    hdr = struct.pack("<4sIIIIIII44xIIIIIIIIIIIII", b"DDS ", 124, 0x81007, h, w, len(data), 0, 1,
                      32, 0x4, 0x30315844, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    return hdr + struct.pack("<IIIII", 98, 3, 0, 1, 0) + data


if __name__ == "__main__":
    main()
