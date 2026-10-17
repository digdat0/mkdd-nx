"""Read-only access to RVZ disc images (Dolphin's compressed format), for build_from_iso.py.

Written from Dolphin's format description (docs/WiaAndRvz.md); the runtime's C reader
(runtime/disc.c) follows the same layout. GameCube discs only: everything is "raw data" split
into groups of chunk_size bytes, each stored compressed (Zstandard by default; bzip2, LZMA,
LZMA2 or none also possible). RVZ also "packs" a group: runs of the disc's pseudo-random filler
are stored as a 68-byte seed for a lagged Fibonacci generator instead of the bytes themselves.

    d = RVZ(path); d.read(offset, size)
"""
import bz2
import lzma
import struct

HEAD = struct.Struct(">4sIII20sQQ20s")          # file head, 0x48 bytes
DISC = struct.Struct(">IIiI128sIIQ20sIQIIQIB7s")  # disc struct, 0xDC bytes
RAW = struct.Struct(">QQII")                       # raw data entry
GROUP = struct.Struct(">III")                      # RVZ group entry
NONE, PURGE, BZIP2, LZMA, LZMA2, ZSTD = range(6)
BLOCK = 0x8000                                     # junk and raw-data alignment unit
LFG_K, LFG_J, SEED = 521, 32, 17


class LFG:
    """The disc filler generator: seeded with 17 words, outputs 521 words per step."""

    def __init__(self, seed):
        b = list(struct.unpack(">17I", seed)) + [0] * (LFG_K - SEED)
        for i in range(SEED, LFG_K):
            b[i] = ((b[i - 17] << 23) & 0xFFFFFFFF) ^ (b[i - 16] >> 9) ^ b[i - 1]
        self.b = [(x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000) for x in b]
        for _ in range(4):
            self.step()
        self.buf = None
        self.pos = 0

    def step(self):
        b = self.b
        for i in range(LFG_J):
            b[i] ^= b[i + LFG_K - LFG_J]
        for i in range(LFG_J, LFG_K):
            b[i] ^= b[i - LFG_J]
        self.buf = None

    def skip(self, n):
        self.pos += n
        while self.pos >= LFG_K * 4:
            self.step()
            self.pos -= LFG_K * 4

    def take(self, n):
        out = bytearray()
        while n:
            if self.buf is None:
                self.buf = struct.pack(">521I", *self.b)
            k = min(n, LFG_K * 4 - self.pos)
            out += self.buf[self.pos:self.pos + k]
            self.pos += k
            n -= k
            if self.pos == LFG_K * 4:
                self.step()
                self.pos = 0
        return bytes(out)


class RVZ:
    def __init__(self, path):
        self.f = open(path, "rb")
        magic, _ver, _compat, disc_size, _h, self.size, _fsize, _h2 = HEAD.unpack(self.f.read(HEAD.size))
        if magic != b"RVZ\x01":
            raise ValueError("not an RVZ file")
        d = DISC.unpack(self.f.read(DISC.size).ljust(DISC.size, b"\0"))
        (disc_type, self.method, _lvl, self.chunk, self.dhead, _np, _pts, _po, _ph,
         n_raw, raw_off, raw_size, n_groups, group_off, group_size, cd_len, cd) = d
        if disc_type != 1:
            raise ValueError("not a GameCube RVZ")
        if self.method not in (NONE, BZIP2, LZMA, LZMA2, ZSTD):
            raise ValueError(f"unsupported RVZ compression {self.method}")
        self.props = cd[:cd_len]
        raws = self.unpack_table(raw_off, raw_size, n_raw * RAW.size)
        self.raw = [RAW.unpack_from(raws, i * RAW.size) for i in range(n_raw)]
        groups = self.unpack_table(group_off, group_size, n_groups * GROUP.size)
        self.groups = [GROUP.unpack_from(groups, i * GROUP.size) for i in range(n_groups)]
        self.cache = {}

    def decompress(self, data, out_size):
        if self.method == NONE:
            return data
        if self.method == ZSTD:
            import zstandard
            return zstandard.ZstdDecompressor().decompress(data, max_output_size=out_size)
        if self.method == BZIP2:
            return bz2.decompress(data)
        fid = lzma.FILTER_LZMA1 if self.method == LZMA else lzma.FILTER_LZMA2
        filt = lzma._decode_filter_properties(fid, self.props)
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[filt]).decompress(data, out_size)

    def unpack_table(self, off, size, out_size):
        self.f.seek(off)
        return self.decompress(self.f.read(size), out_size)[:out_size]

    def group(self, gi, start, size):
        """Bytes of group gi, which holds disc bytes [start, start + size)."""
        key = gi
        if key in self.cache:
            return self.cache[key]
        off4, dsize, packed = self.groups[gi]
        compressed, dsize = bool(dsize & 0x80000000), dsize & 0x7FFFFFFF
        if dsize == 0:
            data = bytes(size)
        else:
            self.f.seek(off4 << 2)
            data = self.f.read(dsize)
            if compressed:
                data = self.decompress(data, packed or size)
            if packed:
                data = self.unpack(data[:packed], start, size)
        if len(self.cache) > 8:
            self.cache.clear()
        self.cache[key] = data[:size]
        return self.cache[key]

    @staticmethod
    def unpack(data, pos, size):
        out, i = bytearray(), 0
        while len(out) < size and i < len(data):
            (n,) = struct.unpack_from(">I", data, i)
            i += 4
            if n & 0x80000000:
                n &= 0x7FFFFFFF
                g = LFG(data[i:i + SEED * 4])
                i += SEED * 4
                g.skip(pos % BLOCK)
                out += g.take(n)
            else:
                out += data[i:i + n]
                i += n
            pos += n
        return bytes(out)

    def read(self, off, size):
        out = bytearray(size)
        end = off + size
        if off < 0x80:  # the disc header's first 0x80 bytes live in the disc struct
            k = min(end, 0x80) - off
            out[0:k] = self.dhead[off:off + k]
        for r_off, r_size, gi0, ng in self.raw:
            base = r_off - r_off % BLOCK  # groups start at the BLOCK-aligned offset below the entry
            r_end = r_off + r_size
            lo, hi = max(off, r_off), min(end, r_end)
            if lo >= hi:
                continue
            for g in range((lo - base) // self.chunk, (hi - 1 - base) // self.chunk + 1):
                if g >= ng:
                    break
                gs = base + g * self.chunk
                gsize = min(self.chunk, r_end - gs)
                data = self.group(gi0 + g, gs, gsize)
                a, b = max(lo, gs), min(hi, gs + gsize)
                out[a - off:b - off] = data[a - gs:b - gs]
        return bytes(out)
