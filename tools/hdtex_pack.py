#!/usr/bin/env python3
"""Convert a Dolphin HD texture pack for Mario Kart: Double Dash!! into MKDD-NX's HD texture file.

    py -3 tools/hdtex_pack.py <pack folder> hd-textures.bin [--jobs N] [--compare DIR] [--limit N]

<pack folder> is the folder Dolphin reads (Load/Textures/GM4E01). Every texture is kept at its full
size and BC7-compressed (the Switch GPU samples BC7 directly) with a full mipmap chain: the pack's
own _mipN files where it has them, box-filtered levels otherwise. Copy the output to
sdmc:/switch/MKDD-NX/ (as hd-textures.bin) next to the .nro. Nothing from the pack belongs in the repository.

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


def find_cc():
    cands = [os.environ.get("CC")]
    if os.name == "nt":
        cands += [r"C:\msys64\ucrt64\bin\gcc.exe", r"C:\msys64\mingw64\bin\gcc.exe", r"C:\msys64\clang64\bin\clang.exe"]
    cands += [shutil.which("gcc"), shutil.which("clang"), shutil.which("cc")]
    for c in cands:
        if c and Path(c).exists():
            return c
    return None


def load_encoder():
    if not LIB.exists() or LIB.stat().st_mtime < SRC.stat().st_mtime:
        cc = find_cc()
        if not cc:
            fail("no C compiler found to build tools/bc7enc.c (install MSYS2 ucrt64 gcc, or set CC)")
        LIB.parent.mkdir(exist_ok=True)
        cmd = [cc, "-O3", "-march=native", "-shared", "-o", str(LIB), str(SRC)]
        if os.name == "nt":
            cmd.insert(3, "-static")
        else:
            cmd.insert(3, "-fPIC")
        env = dict(os.environ)
        env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode:
            fail("building the BC7 encoder failed:\n" + r.stderr)
    lib = ctypes.CDLL(str(LIB))
    lib.bc7_encode.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
    lib.bc7_encode.restype = None
    return lib


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


def main():
    ap = argparse.ArgumentParser(description="Dolphin HD texture pack -> MKDD-NX BC7 texture file")
    ap.add_argument("pack", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--compare", type=Path, help="write PNG | BC7 side-by-side images of a few textures here")
    ap.add_argument("--limit", type=int, help="convert only the first N textures (testing)")
    a = ap.parse_args()

    if not a.pack.is_dir():
        fail(f"{a.pack} is not a folder")
    try:
        a.out.resolve().relative_to(ROOT)
        if "build" not in a.out.resolve().relative_to(ROOT).parts[0]:
            fail("write the output outside the repository (it is game content)")
    except ValueError:
        pass
    lib = load_encoder()
    tex, skipped = scan(a.pack)
    keys = sorted(tex)
    if a.limit:
        keys = keys[: a.limit]
    print(f"{len(tex)} textures, {len(skipped)} other files skipped")
    if not keys:
        fail("no Dolphin texture names found (expected tex1_<w>x<h>_<hash>_<format>.png)")
    if a.compare:
        a.compare.mkdir(parents=True, exist_ok=True)
        step = max(1, len(keys) // 12)
        compare = set(keys[::step][:12])
    else:
        compare = set()

    index, lock, done = {}, threading.Lock(), [0]
    t0 = time.time()
    tmp = a.out.with_suffix(a.out.suffix + ".part")
    with open(tmp, "wb") as f:
        f.write(b"\0" * (HEADER.size + ENTRY.size * len(keys)))
        f.seek(0, 2)
        pad = (-f.tell()) % ALIGN
        f.write(b"\0" * pad)

        def job(key):
            levels = levels_of(tex[key]["files"])
            data = b"".join(encode(lib, im) for im in levels)
            if key in compare:
                im = levels[0]
                dec = Image.open(__import__("io").BytesIO(dds_bc7(im.size, data[: ((im.size[0] + 3) // 4) * ((im.size[1] + 3) // 4) * 16])))
                side = Image.new("RGBA", (im.size[0] * 2, im.size[1]))
                side.paste(im, (0, 0))
                side.paste(dec.convert("RGBA"), (im.size[0], 0))
                side.save(a.compare / tex[key]["files"][0].name)
            return key, levels[0].size, len(levels), data

        with ThreadPoolExecutor(a.jobs) as ex:
            futs = [ex.submit(job, k) for k in keys]
            for fu in as_completed(futs):
                key, (W, H), nlev, data = fu.result()
                with lock:
                    off = f.tell()
                    f.write(data)
                    f.write(b"\0" * ((-f.tell()) % ALIGN))
                    index[key] = (W, H, nlev, len(data), off)
                    done[0] += 1
                    if done[0] % 50 == 0 or done[0] == len(keys):
                        el = time.time() - t0
                        print(f"  {done[0]}/{len(keys)}  {f.tell() / 2**20:.0f} MB  {el:.0f} s, ~{el / done[0] * (len(keys) - done[0]):.0f} s left", flush=True)

        f.seek(0)
        f.write(HEADER.pack(MAGIC, VERSION, len(keys), ENC_BC7, 0, HEADER.size))
        for key in sorted(index, key=lambda k: (k[0], k[1], k[2], k[3], k[4])):
            th, tl, w, h, fmt = key
            W, H, nlev, size, off = index[key]
            f.write(ENTRY.pack(th, tl, w, h, W, H, fmt, tex[key]["flags"], nlev, 0, size, off))
    os.replace(tmp, a.out)
    print(f"wrote {a.out} ({a.out.stat().st_size / 2**20:.0f} MB, {len(keys)} textures) in {time.time() - t0:.0f} s")


def dds_bc7(size, data):
    w, h = size
    hdr = struct.pack("<4sIIIIIII44xIIIIIIIIIIIII", b"DDS ", 124, 0x81007, h, w, len(data), 0, 1,
                      32, 0x4, 0x30315844, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    return hdr + struct.pack("<IIIII", 98, 3, 0, 1, 0) + data


if __name__ == "__main__":
    main()
