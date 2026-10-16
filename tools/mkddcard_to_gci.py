"""Convert an MKDDCARD save (runtime/memcard.c) into standard .gci files (one per card file).

usage: py -3 tools/mkddcard_to_gci.py slotA.bin [out_dir]

A .gci is the card's 64-byte directory entry (big-endian) followed by the file's 8 KiB blocks,
the format Dolphin's Memory Card Manager / GCI-folder mode and save editors use. The file data is
copied unchanged. Saves made before 0939 may carry a blank game code; those get GM4E/01.
"""
import os, struct, sys

SECTOR = 8192


def parse(b):
    assert b[:8] == b"MKDDCARD", "not an MKDDCARD file"
    (ver,) = struct.unpack_from("<I", b, 8)
    assert ver == 1, f"unknown version {ver}"
    p, files = 12, []
    while p < len(b):
        present = b[p]; p += 1
        if not present:
            continue
        name, game, co = b[p:p + 32], b[p + 32:p + 36], b[p + 36:p + 38]
        t, banner, icon, ifmt, ispd, comment, ln = struct.unpack_from("<IBIHHII", b, p + 38)
        p += 59
        files.append(dict(name=name, game=game, co=co, time=t, banner=banner, icon=icon, ifmt=ifmt,
                          ispd=ispd, comment=comment, data=b[p:p + ln]))
        p += ln
    return files


def gci(f):
    game = f["game"] if f["game"].strip(b"\0") else b"GM4E"
    co = f["co"] if f["co"].strip(b"\0") else b"01"
    data = f["data"]
    assert len(data) % SECTOR == 0, f"data length {len(data)} is not whole blocks"
    hdr = (game + co + b"\xff" + bytes([f["banner"]]) + f["name"] +
           struct.pack(">IIHHBBHHHI", f["time"], f["icon"], f["ifmt"], f["ispd"],
                       0x04, 0, 5, len(data) // SECTOR, 0xFFFF, f["comment"]))
    assert len(hdr) == 64
    return game, co, hdr + data


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(src))
    for f in parse(open(src, "rb").read()):
        game, co, g = gci(f)
        name = f["name"].split(b"\0")[0].decode("ascii", "replace")
        dst = os.path.join(out, f"{co.decode()}-{game.decode()}-{name}.gci")
        if os.path.exists(dst):
            dst = dst[:-4] + f" ({os.path.basename(src)}).gci"
        with open(dst, "wb") as o:
            o.write(g)
        print(f"{dst}: {len(f['data']) // SECTOR} blocks, time {f['time']}")


if __name__ == "__main__":
    main()
