"""Pack one or more .gci files into an MKDDCARD save (runtime/memcard.c), the reverse of
mkddcard_to_gci.py.

usage: py -3 tools/gci_to_mkddcard.py out_slotA.bin file1.gci [file2.gci ...]
"""
import struct, sys

SECTOR = 8192
MAX_FILES = 127


def main():
    out, srcs = sys.argv[1], sys.argv[2:]
    assert srcs and len(srcs) <= MAX_FILES
    body = bytearray()
    for s in srcs:
        g = open(s, "rb").read()
        hdr, data = g[:64], g[64:]
        game, co, banner, name = hdr[0:4], hdr[4:6], hdr[7], hdr[8:40]
        t, icon, ifmt, ispd, _perm, _copies, _first, blocks, _pad, comment = struct.unpack_from(">IIHHBBHHHI", hdr, 40)
        assert len(data) == blocks * SECTOR, f"{s}: {len(data)} data bytes, header says {blocks} blocks"
        body += b"\x01" + name + game + co + struct.pack("<IBIHHII", t, banner, icon, ifmt, ispd, comment, len(data)) + data
        print(f"{s}: {game.decode()}{co.decode()} '{name.split(chr(0).encode())[0].decode()}', {blocks} blocks")
    body += b"\x00" * (MAX_FILES - len(srcs))
    with open(out, "wb") as o:
        o.write(b"MKDDCARD" + struct.pack("<I", 1) + body)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
