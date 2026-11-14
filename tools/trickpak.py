"""Builds assets/trick.pak, the Trick Style files the app carries (runtime/trickpak.c unpacks them into
mods/trick and mods/coins on the first start of a new pack).

The tracks and the mode page's menu.arc are partly the game's own data (a course archive's objects/ folder,
course data edited from the slot's, the menu with a fourth row added). The pack holds none of those bytes:
each file is a list of literal runs (ours) and copies from files on the player's disc (decompressed), so the
app rebuilds them from the player's own disc. A file whose source differs on a disc (CRC) is skipped there.

Without a disc image the files that are all ours (coins, fx, Art) are packed as usual and the files built on the
game's are taken unchanged from the existing pack (-o); the disc is needed only when one of those changed or is new.

usage: py -3.10 tools/trickpak.py [GM4E01 disc image] <app folder holding mods/trick and mods/coins> [-o assets/trick.pak]
"""
import argparse
import os
import struct
import sys
import zlib

import zstandard

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import charpack  # noqa: E402

BLOCK = 16     # bytes a copy is found by
STEP = 4       # source offsets indexed (a match is extended backwards, so any copy >= BLOCK + STEP is found)
MIN_COPY = 20  # shorter matches stay literal (a copy op costs 9 bytes; short so little of the game's data stays in the pack)


def disc_sources(rel):
    """the disc files a pack file is rebuilt from (paths as the disc's), most likely first"""
    p = rel.split("/")
    if p[:3] == ["mods", "trick", "files"]:
        return ["/".join(p[3:])]
    if p[:4] == ["mods", "trick", "tracks", "Course"]:
        return ["Course/" + p[4], "Course/Mini7.arc", "Course/Waluigi.arc"]  # the cup tracks (Tnn.arc) are built on Cookie Land's
    if p[:4] == ["mods", "trick", "tracks", "CourseName"]:
        return ["CourseName/English/" + p[4], "CourseName/English/Mini7_name.bti"]
    return []


def delta(out, srcs):
    """[(None, bytes) literal | (source index, offset, length) copy] that rebuild out"""
    idx = {}
    for si, s in enumerate(srcs):
        for o in range(0, len(s) - BLOCK + 1, STEP):
            idx.setdefault(s[o:o + BLOCK], (si, o))
    ops, lit, i, n = [], bytearray(), 0, len(out)
    while i < n:
        hit = idx.get(out[i:i + BLOCK]) if i + BLOCK <= n else None
        if hit:
            si, o = hit
            s = srcs[si]
            b = 0
            while b < len(lit) and o - b > 0 and lit[-1 - b] == s[o - b - 1]:
                b += 1
            e = BLOCK
            while i + e < n and o + e < len(s) and out[i + e] == s[o + e]:
                e += 1
            if b + e >= MIN_COPY:
                if b:
                    del lit[-b:]
                if lit:
                    ops.append((None, bytes(lit)))
                    lit = bytearray()
                ops.append((si, o - b, b + e))
                i += e
                continue
        lit.append(out[i])
        i += 1
    if lit:
        ops.append((None, bytes(lit)))
    return ops


def encode_entry(rel, sources, out_n, out_crc, ops):
    """one file's entry (the format runtime/trickpak.c reads): its path, its disc sources [(path, size, crc)],
    its size and CRC, then its ops [(None, bytes) literal | (source index, offset, length) copy]"""
    e = [struct.pack("<H", len(rel)), rel.encode(), struct.pack("<H", len(sources))]
    for sp, n, crc in sources:
        e += [struct.pack("<H", len(sp)), sp.encode(), struct.pack("<II", n, crc)]
    e.append(struct.pack("<III", out_n, out_crc, len(ops)))
    for op in ops:
        if op[0] is None:
            e += [struct.pack("<BI", 0, len(op[1])), op[1]]
        else:
            e.append(struct.pack("<BII", 1 + op[0], op[1], op[2]))
    return b"".join(e)


def decode_entry(raw, p):
    """encode_entry's fields read back from raw at p, and the offset after the entry"""
    (pl,) = struct.unpack_from("<H", raw, p)
    rel = raw[p + 2:p + 2 + pl].decode()
    p += 2 + pl
    (ns,) = struct.unpack_from("<H", raw, p)
    p += 2
    sources = []
    for _ in range(ns):
        (l,) = struct.unpack_from("<H", raw, p)
        sp = raw[p + 2:p + 2 + l].decode()
        n, crc = struct.unpack_from("<II", raw, p + 2 + l)
        sources.append((sp, n, crc))
        p += 2 + l + 8
    out_n, out_crc, nops = struct.unpack_from("<III", raw, p)
    p += 12
    ops = []
    for _ in range(nops):
        if raw[p] == 0:
            (l,) = struct.unpack_from("<I", raw, p + 1)
            ops.append((None, raw[p + 5:p + 5 + l]))
            p += 5 + l
        else:
            o, l = struct.unpack_from("<II", raw, p + 1)
            ops.append((raw[p] - 1, o, l))
            p += 9
    return (rel, sources, out_n, out_crc, ops), p


def read_pak(path):
    """{path: (out size, out crc, the file's entry bytes)} of an existing pack; None when there is none"""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    if len(d) < 20 or d[:4] != b"MKTP" or struct.unpack_from("<I", d, 4)[0] != 1:
        sys.exit(f"{path}: not a trick pack")
    raw_n, comp_n, crc = struct.unpack_from("<III", d, 8)
    try:
        if 20 + comp_n > len(d):
            raise ValueError
        raw = zstandard.ZstdDecompressor().decompress(d[20:20 + comp_n], max_output_size=raw_n)
        if len(raw) != raw_n or zlib.crc32(raw) != crc:
            raise ValueError
        got, p = {}, 4
        for _ in range(struct.unpack_from("<I", raw, 0)[0]):
            (rel, sources, out_n, out_crc, ops), q = decode_entry(raw, p)
            if encode_entry(rel, sources, out_n, out_crc, ops) != raw[p:q]:
                raise ValueError  # the reader and the writer disagree on the format
            got[rel] = (out_n, out_crc, raw[p:q])
            p = q
    except (ValueError, struct.error, zstandard.ZstdError, UnicodeDecodeError):
        sys.exit(f"{path}: damaged")
    return got


TITLES = (b"entry_trick.bti", b"mozi_trickbattle.bti")


def blank_titles(rel, d):
    """menu.arc with its TRICK STYLE titles blank: the app letters them from the disc's MarioFont when it unpacks
    (runtime/trickpak.c), so an installed (lettered) copy packs the same as menu4.py's"""
    if not rel.endswith("SceneData/English/menu.arc") or d[:4] != b"RARC":
        return d
    d = bytearray(d)
    data = 0x20 + struct.unpack(">I", d[0x0C:0x10])[0]
    ne, eo, ss, so = struct.unpack(">IIII", d[0x28:0x38])
    eo, so = eo + 0x20, so + 0x20
    done = 0
    for i in range(ne):
        e = d[eo + 20 * i:eo + 20 * i + 20]
        name = bytes(d[so + struct.unpack(">H", e[6:8])[0]:]).split(b"\0", 1)[0].lower()
        if e[4] & 1 and name in TITLES:
            off = data + struct.unpack(">I", e[8:12])[0]
            assert d[off] == 2 and struct.unpack(">HH", d[off + 2:off + 6]) == (256, 32), name
            t = off + struct.unpack(">I", d[off + 0x1C:off + 0x20])[0]
            d[t:t + 256 * 32] = bytes(256 * 32)
            done += 1
    assert done == 2, f"{rel}: the two TRICK STYLE titles not found"
    return bytes(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc", nargs="?", help="leave out to reuse the existing pack's entries for the files built on the game's")
    ap.add_argument("app")
    ap.add_argument("-o", "--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "trick.pak"))
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(a.app, "mods", "trick")):
        sys.exit(f"{a.app}: no mods/trick folder (usage: [disc image] <app folder>)")
    disc = charpack.Disc(a.disc) if a.disc else None
    old = {} if disc else read_pak(a.out)
    if old is None:
        sys.exit(f"{a.out}: no pack to take the files built on the game's from; give the disc image")
    files = []
    for top in ("mods/trick", "mods/coins"):
        for dp, _, names in os.walk(os.path.join(a.app, top)):
            for nm in names:
                if nm.endswith((".bak", ".tmp")) or nm.startswith("."):
                    continue
                files.append(os.path.relpath(os.path.join(dp, nm), a.app).replace("\\", "/"))
    files.sort()
    if not files:
        sys.exit(f"{a.app}: no files under mods/trick or mods/coins")
    body = [struct.pack("<I", len(files))]
    tot_lit = tot_copy = kept = 0
    need = []
    for rel in files:
        out = blank_titles(rel, charpack.yaz0(open(os.path.join(a.app, rel), "rb").read()))  # written uncompressed
        if not disc and disc_sources(rel):  # built on the game's: only the stored entry can leave its bytes out
            o = old.get(rel)
            if o and o[0] == len(out) and o[1] == zlib.crc32(out):
                body.append(o[2])
                kept += 1
                print(f"{rel}: {len(out)} bytes, kept from the existing pack")
            else:
                need.append(rel)
            continue
        srcs, names = [], []
        for sp in disc_sources(rel):
            if sp.lower() in disc.files:
                srcs.append(charpack.yaz0(disc.file(sp)))
                names.append(sp)
        ops = delta(out, srcs) if srcs else [(None, out)]
        used = sorted({op[0] for op in ops if op[0] is not None})
        remap = {si: k for k, si in enumerate(used)}
        sources = [(names[si], len(srcs[si]), zlib.crc32(srcs[si])) for si in used]
        ops = [op if op[0] is None else (remap[op[0]], op[1], op[2]) for op in ops]
        body.append(encode_entry(rel, sources, len(out), zlib.crc32(out), ops))
        nl = sum(len(op[1]) for op in ops if op[0] is None)
        nc = sum(op[2] for op in ops if op[0] is not None)
        tot_lit += nl
        tot_copy += nc
        print(f"{rel}: {len(out)} bytes, {nl} ours, {nc} from {', '.join(names[si] for si in used) or '-'}")
    if need:
        sys.exit("these files are built on the game's and changed or are new; pack them with the disc image:\n  " + "\n  ".join(need))
    raw = b"".join(body)
    comp = zstandard.ZstdCompressor(level=19).compress(raw)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tmp = a.out + ".tmp"  # the old pack stays whole until the new one is (a disc-less build reads it)
    with open(tmp, "wb") as f:
        f.write(b"MKTP" + struct.pack("<IIII", 1, len(raw), len(comp), zlib.crc32(raw)) + comp)
    os.replace(tmp, a.out)
    note = f" (not counting {kept} files kept from the existing pack)" if kept else ""
    print(f"{a.out}: {len(files)} files, {tot_lit} bytes ours + {tot_copy} from the disc{note}, pack {len(comp) + 20} bytes")


if __name__ == "__main__":
    main()
