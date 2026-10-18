"""Extract the intro movie (play1.thp) from your disc image as a numbered JPEG sequence, for
upscaling it on a PC. Nothing from the disc belongs in this repository: write it elsewhere.

    py -3 tools/thp_extract.py <disc image (.iso/.gcm/.ciso/.rvz)> <output folder> [--movie play1.thp]

Writes <output>/frames/frame_00001.jpg ... (608x448, the movie's own JPEG data, re-wrapped
without re-encoding), <output>/play1.thp (the raw file) and <output>/info.txt (size, frame rate).
THP frames are baseline JPEG whose entropy-coded data skips JPEG's byte stuffing (a 0x00 after
each 0xFF); putting it back is all a standard JPEG needs.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_from_iso import Disc  # noqa: E402


def find_file(d, name):
    fst_off, fst_size = struct.unpack(">II", d.read(0x424, 8))
    fst = d.read(fst_off, fst_size)
    n = struct.unpack_from(">I", fst, 8)[0]
    names = fst[n * 12:]
    for i in range(1, n):
        e = fst[i * 12:i * 12 + 12]
        if e[0]:
            continue
        no = struct.unpack(">I", e[:4])[0] & 0xFFFFFF
        nm = names[no:names.index(b"\0", no)].decode("ascii", "replace")
        if nm.lower() == name.lower():
            return struct.unpack(">II", e[4:])
    sys.exit(f"thp_extract: {name} not found on the disc")


def stuff(jpeg):
    """THP JPEG -> standard JPEG: byte-stuff the entropy-coded data after the SOS header."""
    sos = jpeg.find(b"\xff\xda")
    eoi = jpeg.rfind(b"\xff\xd9")
    if sos < 0 or eoi < sos:
        raise ValueError("not a THP JPEG frame")
    start = sos + 2 + struct.unpack(">H", jpeg[sos + 2:sos + 4])[0]
    return jpeg[:start] + jpeg[start:eoi].replace(b"\xff", b"\xff\x00") + b"\xff\xd9"


def main():
    ap = argparse.ArgumentParser(description="THP movie -> JPEG frame sequence")
    ap.add_argument("disc", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--movie", default="play1.thp")
    a = ap.parse_args()

    d = Disc(str(a.disc))
    off, size = find_file(d, a.movie)
    thp = d.read(off, size)
    if thp[:4] != b"THP\0":
        sys.exit("thp_extract: not a THP file")
    fps, nframes, first_size = struct.unpack_from(">fII", thp, 0x10)
    comp_off, _, data_off = struct.unpack_from(">III", thp, 0x20)
    ncomp = struct.unpack_from(">I", thp, comp_off)[0]
    kinds = thp[comp_off + 4:comp_off + 4 + ncomp]  # 0 video, 1 audio
    w, h = struct.unpack_from(">II", thp, comp_off + 0x14)

    frames = a.out / "frames"
    frames.mkdir(parents=True, exist_ok=True)
    (a.out / a.movie).write_bytes(thp)
    pos, fsize = data_off, first_size
    for i in range(nframes):
        sizes = struct.unpack_from(f">II{ncomp}I", thp, pos)
        nxt, comps = sizes[0], sizes[2:]
        p = pos + 8 + 4 * ncomp
        for kind, cs in zip(kinds, comps):
            if kind == 0:
                (frames / f"frame_{i + 1:05d}.jpg").write_bytes(stuff(thp[p:p + cs]))
            p += cs
        pos += fsize
        fsize = nxt
        if (i + 1) % 500 == 0:
            print(f"  {i + 1}/{nframes} frames", flush=True)
    info = (f"{a.movie}: {w}x{h}, {fps:.3f} fps, {nframes} frames ({nframes / fps:.1f} s), "
            f"{'with' if 1 in kinds else 'no'} audio\n")
    (a.out / "info.txt").write_text(info)
    print("wrote", frames, "-", info.strip())


if __name__ == "__main__":
    main()
