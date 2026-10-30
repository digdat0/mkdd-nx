#!/usr/bin/env python3
"""Wii drivers for MKDD-NX: a Mario Kart Wii driver (.szs) -> a Double Dash character folder.

Mario Kart Wii's drivers (Race/Kart/<kart>-<driver>.szs: driver_model.brres) and Double Dash's
(MRAM.arc Driver/<folder>/driver.bmd) are both drawn by the same graphics chip (the Wii's is the
GameCube's), so a model converts without remodelling: the vertices, the triangle strips and the
textures are the same formats; the BRRES material's graphics registers become J3D material entries.

The Wii driver's body joints are named and ordered as Double Dash's (skl_root, leg_l1 ... face_1).
The model gets the Double Dash character's skeleton around them (its root, the hand and foot
targets, the cap), so the game's animations move it; a separate hair model (Toadette's braids)
becomes the character's hair model where Double Dash has one (kinoko_hair.bmd), else it is
joined to the head.

    py -3 tools/wiichar.py IN.szs OUT_FOLDER [--base toadette] [--name NAME] [--author A] [--iso GAME.iso]

The output is a character folder (tools/charpack.py) for the SD card's mods/characters/. Nothing
from the game's disc is written: the disc image, when given, is only read to check the skeleton.
"""
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import charpack  # noqa: E402  (yaz0, BASES, Disc, bmd_info)


class ConvertError(Exception):
    """the input can't be converted (the reason, for the user)"""


def u8(b, o): return b[o]
def s8(b, o): return struct.unpack_from(">b", b, o)[0]
def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def s16(b, o): return struct.unpack_from(">h", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def s32(b, o): return struct.unpack_from(">i", b, o)[0]
def f32(b, o): return struct.unpack_from(">f", b, o)[0]


# ---- archives: Yaz0 + U8 (.szs) ----
def u8_files(d):
    """{path: bytes} of a U8 archive (paths lower case, '/' separated)"""
    if d[:4] != b"\x55\xAA\x38\x2D":
        raise ConvertError("not a Wii archive (U8)")
    root = u32(d, 4)
    total = u32(d, root + 8)
    strings = root + total * 12
    out, ends, path = {}, [], []
    for i in range(1, total):
        while ends and i >= ends[-1]:
            ends.pop()
            path.pop()
        e = root + i * 12
        no = u32(d, e) & 0xFFFFFF
        name = d[strings + no:d.index(b"\0", strings + no)].decode("latin-1")
        a, b = u32(d, e + 4), u32(d, e + 8)
        if d[e]:
            path.append(name)
            ends.append(b)
        else:
            out["/".join([p for p in path if p != "."] + [name]).lower()] = d[a:a + b]
    return out


# ---- BRRES ----
def dict_group(b, o):
    """[(name, absolute offset)] of a BRRES index group at o"""
    out = []
    for k in range(1, u32(b, o + 4) + 1):
        e = o + 8 + 16 * k
        no, do = u32(b, e + 8), u32(b, e + 12)
        out.append((b[o + no:b.index(b"\0", o + no)].decode("latin-1"), o + do))
    return out


def brres_folders(b):
    """{folder name ('3DModels(NW4R)', 'Textures(NW4R)' ...): [(name, offset)]}"""
    if b[:4] != b"bres":
        raise ConvertError("not a BRRES file")
    root = u16(b, 0xC)
    return {name: dict_group(b, o) for name, o in dict_group(b, root + 8)}


def strz(b, o):
    return b[o:b.index(b"\0", o)].decode("latin-1")


class Tex:
    """a TEX0 texture: GX format, size, mip levels, the image data (as GX lays it out)"""

    def __init__(self, b, o):
        self.name = strz(b, o + s32(b, o + 0x14))
        self.ci = u32(b, o + 0x18)
        self.w, self.h, self.fmt = u16(b, o + 0x1C), u16(b, o + 0x1E), u32(b, o + 0x20)
        self.levels = u32(b, o + 0x24)
        self.min_lod, self.max_lod = f32(b, o + 0x28), f32(b, o + 0x2C)
        size = sum(tex_size(self.fmt, max(1, self.w >> i), max(1, self.h >> i)) for i in range(self.levels))
        start = o + s32(b, o + 0x10)
        self.data = b[start:start + size]

    def depalette(self, pal):
        """a palette texture (CI4/CI8/CI14x2) with its colours pal [(r, g, b, a)] -> RGBA8, each mip level"""
        out, p = bytearray(), 0
        bw, bh, bpp = BLOCK[self.fmt]
        for lv in range(self.levels):
            w, h = max(1, self.w >> lv), max(1, self.h >> lv)
            d = self.data[p:p + tex_size(self.fmt, w, h)]
            p += tex_size(self.fmt, w, h)
            px = [[(0, 0, 0, 0)] * w for _ in range(h)]
            q = 0
            for ty in range(0, h, bh):
                for tx in range(0, w, bw):
                    for k in range(bw * bh):
                        if self.fmt == 8:
                            i = d[q + k // 2] >> (0 if k & 1 else 4) & 15
                        elif self.fmt == 9:
                            i = d[q + k]
                        else:
                            i = u16(d, q + 2 * k) & 0x3FFF
                        x, y = tx + k % bw, ty + k // bw
                        if x < w and y < h:
                            px[y][x] = pal[i] if i < len(pal) else (0, 0, 0, 0)
                    q += bw * bh * bpp // 8
            out += rgba8(px, w, h)
        self.fmt, self.ci, self.data = 6, 0, bytes(out)


def rgba8(px, w, h):
    """rows of (r, g, b, a) -> GX RGBA8 (4x4 blocks: the AR half, then the GB half)"""
    out = bytearray()
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            cells = [px[y][x] if y < h and x < w else (0, 0, 0, 0) for y in range(ty, ty + 4) for x in range(tx, tx + 4)]
            out += bytes(v for c in cells for v in (c[3], c[0]))
            out += bytes(v for c in cells for v in (c[1], c[2]))
    return out


def palette(b, o):
    """a PLT0 -> its colours [(r, g, b, a)] (IA8, RGB565 or RGB5A3)"""
    fmt, n, d = u32(b, o + 0x18), u16(b, o + 0x1C), o + s32(b, o + 0x10)
    out = []
    for k in range(n):
        v = u16(b, d + 2 * k)
        if fmt == 0:
            out.append((v & 255,) * 3 + (v >> 8,))
        elif fmt == 1:
            out.append(rgb565(v) + (255,))
        elif fmt == 2:
            out.append(((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255) if v & 0x8000 else
                       ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 // 7))
        else:
            raise ConvertError(f"palette format {fmt}")
    return out


BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32),
         8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}  # format: block w, h, bits per texel


def tex_size(fmt, w, h):
    bw, bh, bpp = BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8


# ---- MDL0 models ----
def mat34(v):
    """12 floats (row major 3x4) -> [[4], [4], [4]]"""
    return [list(v[0:4]), list(v[4:8]), list(v[8:12])]


class Bone:
    def __init__(self, b, o):
        self.name = strz(b, o + s32(b, o + 8))
        self.index, self.mtx_id, self.flags = u32(b, o + 0xC), u32(b, o + 0x10), u32(b, o + 0x14)
        self.scale = struct.unpack_from(">3f", b, o + 0x20)
        self.rot = struct.unpack_from(">3f", b, o + 0x2C)  # degrees, X then Y then Z
        self.trans = struct.unpack_from(">3f", b, o + 0x38)
        par = s32(b, o + 0x5C)
        self.parent = u32(b, o + par + 0xC) if par else -1
        self.world = mat34(struct.unpack_from(">12f", b, o + 0x70))
        self.inv = mat34(struct.unpack_from(">12f", b, o + 0xA0))


COMP_FMT = {0: "B", 1: "b", 2: "H", 3: "h", 4: "f"}


def read_array(b, ncomp, ctype, frac, stride, count, data):
    """count vertices of ncomp components each, as floats"""
    if ctype not in COMP_FMT:
        raise ConvertError(f"vertex data of an unknown type ({ctype})")
    fmt, scale = ">" + COMP_FMT[ctype] * ncomp, 1.0 if ctype == 4 else 1.0 / (1 << frac)
    return [tuple(x * scale for x in struct.unpack_from(fmt, b, data + i * stride)) for i in range(count)]


def read_colors(b, data, fmt, stride, count):
    """count colours as (r, g, b, a) bytes"""
    out = []
    for i in range(count):
        p = data + i * stride
        if fmt == 0:  # RGB565
            v = u16(b, p)
            out.append(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
        elif fmt in (1, 2):  # RGB8, RGBX8
            out.append((b[p], b[p + 1], b[p + 2], 255))
        elif fmt == 3:  # RGBA4
            v = u16(b, p)
            out.append(tuple((v >> s & 15) * 17 for s in (12, 8, 4, 0)))
        elif fmt == 4:  # RGBA6
            v = b[p] << 16 | b[p + 1] << 8 | b[p + 2]
            out.append(tuple((v >> s & 63) * 255 // 63 for s in (18, 12, 6, 0)))
        elif fmt == 5:  # RGBA8
            out.append(tuple(b[p:p + 4]))
        else:
            raise ConvertError(f"vertex colours of an unknown format ({fmt})")
    return out


# vertex attributes (GX numbering): the matrix indices, then the arrays
PNMTX, TEX1MTX, POS, NRM, CLR0, CLR1, TEX0 = 0, 2, 9, 10, 11, 12, 13
PRIMS = {0x80: "quads", 0x90: "triangles", 0x98: "strip", 0xA0: "fan", 0xA8: "lines", 0xB0: "linestrip", 0xB8: "points"}
PRIM_CODE = {v: k for k, v in PRIMS.items()}


def vcd_attrs(lo, hi):
    """[(attribute, 1 direct / 2 8-bit index / 3 16-bit index)] in the order a vertex holds them"""
    out = [(PNMTX, lo & 1)] + [(1 + k, lo >> (1 + k) & 1) for k in range(8)]
    out += [(POS, lo >> 9 & 3), (NRM, lo >> 11 & 3), (CLR0, lo >> 13 & 3), (CLR1, lo >> 15 & 3)]
    out += [(TEX0 + k, hi >> (2 * k) & 3) for k in range(8)]
    return [(a, t) for a, t in out if t]


class Packet:
    """primitives drawn with one set of loaded matrices: slots {PNMTXIDX / 3: matrix id}"""

    def __init__(self, slots):
        self.slots = dict(slots)
        self.prims = []  # (kind, [{attribute: index or value}])


class Obj:
    def __init__(self, b, o):
        self.name = strz(b, o + s32(b, o + 0x38))
        self.index = u32(b, o + 0x3C)
        self.single = s32(b, o + 8)  # the matrix id all of it is bound to, -1: per vertex (PNMTXIDX)
        self.lo, self.hi = u32(b, o + 0xC), u32(b, o + 0x10)
        self.arrays = {POS: s16(b, o + 0x48), NRM: s16(b, o + 0x4A), CLR0: s16(b, o + 0x4C), CLR1: s16(b, o + 0x4E)}
        for k in range(8):
            self.arrays[TEX0 + k] = s16(b, o + 0x50 + 2 * k)
        self._cp(b, o + 0x18 + s32(b, o + 0x20), u32(b, o + 0x1C))
        self.attrs = vcd_attrs(self.lo, self.hi)
        if (POS, 1) in self.attrs:
            raise ConvertError(f"{self.name}: vertex data inline in the draw list (not supported)")
        self.packets = []
        self._prims(b, o + 0x24 + s32(b, o + 0x2C), u32(b, o + 0x28))

    def _cp(self, b, p, n):
        """the vertex descriptor from the object's set-up commands (CP 0x50, 0x60)"""
        end = p + n
        while p < end:
            c = b[p]
            if c == 0x08:
                reg, val = b[p + 1], u32(b, p + 2)
                if reg == 0x50:
                    self.lo = val
                elif reg == 0x60:
                    self.hi = val
                p += 6
            elif c == 0x10:
                p += 5 + 4 * (u16(b, p + 1) + 1)
            elif c == 0:
                p += 1
            else:
                break

    def _prims(self, b, p, n):
        end, slots, pk = p + n, {}, None
        while p < end:
            c = b[p]
            if c in (0, 0x48):
                p += 1
            elif c in (0x20, 0x28, 0x30, 0x38):  # indexed matrix loads: position, normal, texture, light
                idx, addr = u16(b, p + 1), u16(b, p + 3) & 0xFFF
                if c == 0x20 and addr < 0x78 and addr % 12 == 0:
                    slots[addr // 12] = idx
                    pk = None
                p += 5
            elif c == 0x10:
                p += 5 + 4 * (u16(b, p + 1) + 1)
            elif c == 0x08:
                p += 6
            elif c == 0x61:
                p += 5
            elif c & 0xF8 in PRIMS:
                kind, count = PRIMS[c & 0xF8], u16(b, p + 1)
                p += 3
                verts = []
                for _ in range(count):
                    v = {}
                    for a, t in self.attrs:
                        if t == 3:
                            v[a] = u16(b, p)
                            p += 2
                        else:
                            v[a] = b[p]
                            p += 1
                    verts.append(v)
                if pk is None:
                    pk = Packet(slots)
                    self.packets.append(pk)
                pk.prims.append((kind, verts))
            else:
                raise ConvertError(f"{self.name}: unknown draw command {c:#04x}")


def gx_regs(b, p, n):
    """the BP registers (masked writes applied) and XF registers a display list sets"""
    bp, xf, mask, end = {}, {}, 0xFFFFFF, p + n
    while p < end:
        c = b[p]
        if c == 0x61:
            reg, val = b[p + 1], b[p + 2] << 16 | b[p + 3] << 8 | b[p + 4]
            if reg == 0xFE:
                mask = val
            else:
                bp[reg] = (bp.get(reg, 0) & ~mask | val & mask) & 0xFFFFFF
                mask = 0xFFFFFF
            p += 5
        elif c == 0x10:
            cnt, addr = u16(b, p + 1) + 1, u16(b, p + 3)
            for k in range(cnt):
                xf[addr + k] = u32(b, p + 5 + 4 * k)
            p += 5 + 4 * cnt
        elif c == 0x08:
            p += 6
        elif c in (0x20, 0x28, 0x30, 0x38):
            p += 5
        else:
            p += 1  # padding (0)
    return bp, xf


class Layer:
    """a material's texture: its name and sampler settings"""

    def __init__(self, b, t):
        self.tex = strz(b, t + s32(b, t))
        self.map = u32(b, t + 0x10)
        self.wrap_s, self.wrap_t = u32(b, t + 0x18), u32(b, t + 0x1C)
        self.min_filter, self.mag_filter = u32(b, t + 0x20), u32(b, t + 0x24)
        self.lod_bias = f32(b, t + 0x28)
        self.uv_mtx = None  # the material's texture matrix for it (2x3, rows u and v), None: identity


def tex_srt(sx, sy, rot, tx, ty):
    """a Wii texture SRT (Maya mode, as Mario Kart Wii's are made) -> 2x3 matrix on (u, v, 1)"""
    if rot:
        c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
        return ((sx * c, sx * s, sx * (-0.5 * c - (0.5 * s - 0.5) - tx)),
                (-sy * s, sy * c, sy * (0.5 * s - (0.5 * c - 0.5) + ty) + 1 - sy))
    return ((sx, 0.0, -sx * tx), (0.0, sy, sy * ty + 1 - sy))


# MDL0 v11 material: texture SRTs (flags, mode, 8 x scale/rot/translation) at 0x1A8, 8 texture matrix
# effects (0x34 each) after them, the 2 light channels at 0x3F0, the texture layers from 0x418
MAT_SRT, MAT_CHANS, MAT_LAYERS = 0x1A8, 0x3F0, 0x418


class Material:
    def __init__(self, b, o):
        self.name = strz(b, o + s32(b, o + 8))
        self.index = u32(b, o + 0xC)
        self.xlu = bool(u32(b, o + 0x10) & 0x80000000)
        self.ntexgen, self.nchan, self.nstage = b[o + 0x14], b[o + 0x15], b[o + 0x16]
        self.cull = u32(b, o + 0x18)
        self.layers = [Layer(b, o + s32(b, o + 0x30) + 0x34 * k) for k in range(u32(b, o + 0x2C))]
        self.bp, self.xf = gx_regs(b, o + s32(b, o + 0x3C), 0x180)
        self.chans, self.srt_mode = [], 0
        if s32(b, o + 0x30) >= MAT_LAYERS:  # the fixed part is all there
            self.chans = [(tuple(b[c + 4:c + 8]), tuple(b[c + 8:c + 12]), u32(b, c + 12), u32(b, c + 16))
                          for c in (o + MAT_CHANS + 0x14 * k for k in range(min(2, self.nchan)))]  # (material, ambient, colour, alpha)
            flags, self.srt_mode = u32(b, o + MAT_SRT), u32(b, o + MAT_SRT + 4)
            for k, l in enumerate(self.layers[:8]):
                f = flags >> 4 * k & 0xF
                if f & 1 and f & 0xE != 0xE:  # used, and not all of scale 1, rotation 0, translation 0
                    sx, sy, rot, tx, ty = struct.unpack_from(">5f", b, o + MAT_SRT + 8 + 20 * k)
                    if all(math.isfinite(v) for v in (sx, sy, rot, tx, ty)) and (sx, sy, rot, tx, ty) != (1, 1, 0, 0, 0):
                        l.uv_mtx = tex_srt(sx, sy, rot, tx, ty)
        sh = o + s32(b, o + 0x28)
        sbp, _ = gx_regs(b, sh + 0x20, u32(b, sh) - 0x20)
        self.bp.update(sbp)


class Model:
    """an MDL0: bones, vertex arrays, objects, materials and what draws what"""

    def __init__(self, b, o, name):
        self.name = name
        ver = u32(b, o + 8)
        if ver != 11:
            raise ConvertError(f"model {name}: MDL0 version {ver} (Mario Kart Wii's are 11)")
        offs = [s32(b, o + 0x10 + 4 * i) for i in range(14)]
        grp = [dict_group(b, o + x) if x else [] for x in offs]
        info = o + 0x4C
        do = u32(b, info + 0x24)
        self.mtx_node = [s32(b, info + do + 4 + 4 * i) for i in range(u32(b, info + do))]
        self.bones = sorted((Bone(b, x) for _, x in grp[1]), key=lambda j: j.index)
        self.mixes = {}  # matrix id -> [(matrix id of a bone, weight)] (NodeMix)
        self.draws = []  # (material, object, bone, translucent)
        for dname, x in grp[0]:
            self._defs(b, x, dname)
        self.pos = {u32(b, x + 0x10): read_array(b, 3 if u32(b, x + 0x14) else 2, u32(b, x + 0x18), b[x + 0x1C], b[x + 0x1D],
                                                 u16(b, x + 0x1E), x + u32(b, x + 8)) for _, x in grp[2]}
        self.nrm = {}
        for _, x in grp[3]:
            if u32(b, x + 0x14):
                raise ConvertError(f"model {name}: normals with tangents (NBT) are not supported")
            self.nrm[u32(b, x + 0x10)] = read_array(b, 3, u32(b, x + 0x18), b[x + 0x1C], b[x + 0x1D], u16(b, x + 0x1E), x + u32(b, x + 8))
        self.clr = {u32(b, x + 0x10): read_colors(b, x + u32(b, x + 8), u32(b, x + 0x18), b[x + 0x1C], u16(b, x + 0x1E)) for _, x in grp[4]}
        self.uv = {u32(b, x + 0x10): read_array(b, 2 if u32(b, x + 0x14) else 1, u32(b, x + 0x18), b[x + 0x1C], b[x + 0x1D],
                                                u16(b, x + 0x1E), x + u32(b, x + 8)) for _, x in grp[5]}
        self.objs = sorted((Obj(b, x) for _, x in grp[10]), key=lambda j: j.index)
        self.mats = sorted((Material(b, x) for _, x in grp[8]), key=lambda j: j.index)

    def _defs(self, b, p, dname):
        while True:
            op = b[p]
            if op == 1:
                return
            if op == 2:  # NodeTree: bone, parent matrix
                p += 5
            elif op == 3:  # NodeMix: matrix id, weights
                cnt = b[p + 3]
                self.mixes[u16(b, p + 1)] = [(u16(b, p + 4 + 6 * k), f32(b, p + 6 + 6 * k)) for k in range(cnt)]
                p += 4 + 6 * cnt
            elif op == 4:  # Draw: material, object, bone, priority
                self.draws.append((u16(b, p + 1), u16(b, p + 3), u16(b, p + 5), dname.lower() == "drawxlu"))
                p += 8
            elif op in (5, 6):  # EVPMTX, MTXDUP
                p += 5
            else:
                raise ConvertError(f"model {self.name}: unknown definition code {op}")

    def bone_of(self, mtx_id):
        """the bone a matrix id is (None: a weighted mix)"""
        if mtx_id in self.mixes or not 0 <= mtx_id < len(self.mtx_node):
            return None
        n = self.mtx_node[mtx_id]
        return n if 0 <= n < len(self.bones) else None


# ---- 3x4 matrices (rows), rotations X then Y then Z as both games use them ----
IDENT = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0]]


def srt(s, r, t):
    x, y, z = (math.radians(a) for a in r)
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    rm = [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
          [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
          [-sy, sx * cy, cx * cy]]
    return [[rm[i][0] * s[0], rm[i][1] * s[1], rm[i][2] * s[2], t[i]] for i in range(3)]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0.0) for j in range(4)] for i in range(3)]


def inverse(m):
    a = [r[:3] for r in m]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-12:
        raise ConvertError("a joint with a zero scale")
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] - a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det
            for j in range(3)] for i in range(3)]
    t = [-sum(inv[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [inv[i] + [t[i]] for i in range(3)]


def point(m, v):
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] for i in range(3))


def direction(m, v):
    """a normal through m's rotation (the joints have no shear), unit length again"""
    d = [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3)]
    n = math.sqrt(sum(x * x for x in d)) or 1.0
    return tuple(x / n for x in d)


def decompose(m):
    """scale, rotation (degrees), translation of m"""
    s = [math.sqrt(sum(m[i][j] ** 2 for i in range(3))) for j in range(3)]
    r = [[m[i][j] / s[j] for j in range(3)] for i in range(3)]
    sy = -r[2][0]
    if abs(sy) < 0.99999:
        ry = math.asin(sy)
        rx, rz = math.atan2(r[2][1], r[2][2]), math.atan2(r[1][0], r[0][0])
    else:  # Y at +-90: only X - Z (or X + Z) counts
        ry = math.copysign(math.pi / 2, sy)
        rz = 0.0
        rx = math.atan2(r[0][1], r[1][1]) if sy > 0 else math.atan2(-r[0][1], r[1][1])
    rot = tuple(math.degrees(a) for a in (rx, ry, rz))
    back = srt(s, rot, (0, 0, 0))
    if max(abs(back[i][j] - m[i][j]) for i in range(3) for j in range(3)) > 1e-3 * max(1.0, *s):
        raise ConvertError("a joint transform with shear (not supported)")
    return tuple(s), rot, (m[0][3], m[1][3], m[2][3])


# ---- the Double Dash skeleton a model is given ----
class Skeleton:
    """joint names, parents (-1 the root), J3D kinds; textures and materials in their order (for the
    character's face animations, which pick textures and materials by place)"""

    def __init__(self, names, parents, kinds, textures=(), materials=()):
        self.names, self.parents, self.kinds = list(names), list(parents), list(kinds)
        self.textures, self.materials = list(textures), list(materials)
        self.mat_tex = {}  # material -> its first texture's name (from the disc)


def bmd_sections(b):
    out, o = {}, 0x20
    while o + 8 <= len(b):
        size = u32(b, o + 4)
        if size < 8:
            break
        out[b[o:o + 4]] = o
        o += size
    return out


def bmd_names(b, at):
    return [strz(b, at + u16(b, at + 4 + 4 * i + 2)) for i in range(u16(b, at))]


def skeleton_of_bmd(b):
    """the Skeleton of a game model (read from the disc, used only to shape the new one)"""
    if b[:8] != b"J3D2bmd3":
        raise ConvertError("not a J3D model")
    s = bmd_sections(b)
    j, i = s[b"JNT1"], s[b"INF1"]
    n = u16(b, j + 8)
    names = bmd_names(b, j + u32(b, j + 0x14))
    kinds = [u16(b, j + u32(b, j + 0xC) + 0x40 * k) for k in range(n)]
    parents, stack, last, h = [-1] * n, [], -1, i + u32(b, i + 0x14)
    while True:
        t, x = u16(b, h), u16(b, h + 2)
        h += 4
        if t == 0:
            break
        if t == 1:
            stack.append(last)
        elif t == 2:
            last = stack.pop() if stack else -1
        elif t == 0x10:
            parents[x] = next((p for p in reversed(stack) if p >= 0), -1)
            last = x
        else:  # materials and shapes don't change the joint their children hang from
            pass
    t, m = s[b"TEX1"], s[b"MAT3"]
    textures, materials = bmd_names(b, t + u32(b, t + 0x10)), bmd_names(b, m + u32(b, m + 0x14))
    skel = Skeleton(names, parents, kinds, textures, materials)
    # each material's first texture (MAT3: init entry per material, its texture numbers in table 15)
    init, remap, texno = (m + u32(b, m + 0xC + 4 * k) for k in (0, 1, 15))
    for k, mat in enumerate(materials):
        e = init + 0x14C * u16(b, remap + 2 * k)
        slot = u16(b, e + 0x84)
        if slot != 0xFFFF:
            ti = u16(b, texno + 2 * slot)
            if ti < len(textures):
                skel.mat_tex[mat] = textures[ti]
    return skel


# limb ends: the Double Dash hand and foot targets (*_pos) sit where these Wii joints are
TARGETS = {"foot_l_pos": "ankle_l1", "foot_r_pos": "ankle_r1", "hand_l_pos": "wrist_l1", "hand_r_pos": "wrist_r1"}


def default_skeleton(model, cap):
    """without the disc: Double Dash's driver layout around the Wii joints (a root, the hand and
    foot targets, the Wii joints, the cap for Toad and Toadette)"""
    names = [model.name + "_all_root"] + list(TARGETS) + [j.name for j in model.bones]
    parents = [-1, 0, 0, 0, 0] + [5 + j.parent if j.parent >= 0 else 0 for j in model.bones]
    kinds = [0, 2, 2, 2, 2] + [1] * len(model.bones)
    if cap and "face_1" in names:
        names.append("cap1")
        parents.append(names.index("face_1"))
        kinds.append(1)
    return Skeleton(names, parents, kinds)


# ---- a J3D model built from an MDL0 ----
class Built:
    """what the .bmd holds, before it is written"""

    def __init__(self):
        self.joints = []     # (name, kind, parent, scale, rot, trans)
        self.drw = []        # per draw matrix (weighted, joint or envelope index)
        self.env = []        # envelopes: ((joint, weight) ...)
        self.inv = []        # each joint's inverse rest matrix (for the envelopes)
        self.pos, self.nrm, self.uv = [], [], []
        self.clr = []        # vertex colours (r, g, b, a)
        self.shapes = []     # (multi, env, [(table, dl bytes)], bbox)
        self.mats = []       # material dicts (J3D fields)
        self.mesh = []       # (material index, shape index) in draw order
        self.tex = []        # (name, Tex, wrap_s, wrap_t, min, mag)
        self.bbox = None


def bbox_add(bb, p):
    if bb is None:
        return [list(p), list(p)]
    for k in range(3):
        bb[0][k], bb[1][k] = min(bb[0][k], p[k]), max(bb[1][k], p[k])
    return bb


def build(model, skel, wii_tex, notes, what):
    """model (MDL0) on skel -> Built; notes get what was changed or left out"""
    out = Built()
    wii = {j.name: j for j in model.bones}
    names = skel.names
    # the frame of the new model: the Wii root joint, when the skeleton has no joint of its name, is the root
    roots = [j for j in model.bones if j.parent < 0]
    frame = IDENT
    if roots and roots[0].name not in names:
        frame = inverse(roots[0].world)
    world = []
    for i, n in enumerate(names):
        if i == 0:
            w = IDENT
        elif n in wii:
            w = mul(frame, wii[n].world)
        elif n in TARGETS and TARGETS[n] in wii:
            w = mul(frame, wii[TARGETS[n]].world)
        else:
            w = world[skel.parents[i]] if skel.parents[i] >= 0 else IDENT
        world.append(w)
    for i, n in enumerate(names):
        p = skel.parents[i]
        local = mul(inverse(world[p]), world[i]) if p >= 0 else world[i]
        s, r, t = decompose(local)
        out.joints.append((n, skel.kinds[i], p, s, r, t))
    added = [n for i, n in enumerate(names) if i and n not in wii]
    if added:
        notes.append(("info", f"{what}: joints added for Double Dash's skeleton: {', '.join(added)}"))
    # each Wii matrix -> (joint, rest transform for its vertices: identity when the joint is the Wii one)
    joint_of = {}
    for mid in range(len(model.mtx_node)):
        bi = model.bone_of(mid)
        if bi is None:
            continue
        j = model.bones[bi]
        k, fix = j, None
        while k is not None and k.name not in names and not (k.parent < 0 and frame is not IDENT):
            k = model.bones[k.parent] if k.parent >= 0 else None
        if k is None:
            raise ConvertError(f"{what}: joint {j.name} has no place in Double Dash's skeleton")
        ji = names.index(k.name) if k.name in names else 0
        if k is not j:
            fix = mul(inverse(k.world), j.world)
            notes.append(("info", f"{what}: joint {j.name} (not in Double Dash's skeleton) joined to {names[ji]}"))
        joint_of[mid] = (ji, fix)
    # weighted matrices (NodeMix): their vertices sit in the bind pose. Each one -> (the matrix that puts
    # them in the new model's rest pose: frame x sum of weight x bone world x bone inverse bind, its
    # joints and weights on the new skeleton: a joint not in it gives its weight to the one it was joined to)
    mix_of = {}
    for mid, parts in model.mixes.items():
        env, blend = {}, [[0.0] * 4 for _ in range(3)]
        for bm, wt in parts:
            bi = model.bone_of(bm)
            if bi is None or bm not in joint_of:
                raise ConvertError(f"{what}: weighted matrix {mid} uses matrix {bm}, which no joint has")
            j = model.bones[bi]
            m = mul(j.world, j.inv)
            for r in range(3):
                for c in range(4):
                    blend[r][c] += wt * m[r][c]
            ji = joint_of[bm][0]
            env[ji] = env.get(ji, 0.0) + wt
        total = sum(env.values())
        if not total > 0:
            raise ConvertError(f"{what}: weighted matrix {mid} has no weight")
        mix_of[mid] = (mul(frame, [[v / total for v in row] for row in blend]),
                       tuple(sorted((ji, wt / total) for ji, wt in env.items() if wt > 0)))
    used = sorted({joint_of[m][0] for m in joint_of})
    envs = sorted({e for _, e in mix_of.values()})
    out.drw = [(0, j) for j in used] + [(1, k) for k in range(len(envs))]  # J3D's way: the rigid ones first
    out.env = list(envs)
    out.inv = [inverse(w) for w in world]
    drw_ix = {("j", j): i for i, j in enumerate(used)}
    drw_ix.update({("e", e): len(used) + k for k, e in enumerate(envs)})

    def drw_of(mid):
        """a Wii matrix id -> its DRW1 place (None: no such matrix)"""
        if mid in mix_of:
            return drw_ix[("e", mix_of[mid][1])]
        return drw_ix[("j", joint_of[mid][0])] if mid in joint_of else None
    pos_ix, nrm_ix, uv_ix, clr_ix = {}, {}, {}, {}

    def add(table, cache, key, value):
        if key not in cache:
            cache[key] = len(table)
            table.append(value)
        return cache[key]

    # materials first: the game's names (its face animations find the eyes by name), in its order
    names_of = game_material_names(model, skel)
    for i, n in names_of.items():
        if n != model.mats[i].name:
            notes.append(("info", f"{what}: material {model.mats[i].name} named {n}, as the game's (its face animations use it)"))
    order = sorted(range(len(model.mats)), key=lambda i: (skel.materials.index(names_of[i])
                                                          if names_of[i] in skel.materials else 99, i))
    mat_new = {old: new for new, old in enumerate(order)}
    out.tex = texture_layout(model, skel, wii_tex, notes, what)
    for old in order:
        out.mats.append(material(model.mats[old], out.tex, notes, what))
        out.mats[-1]["name"] = names_of[old]
        # vertex colours: where the Wii material takes its colour (or alpha) from them and a part it draws has them
        wm = model.mats[old]
        coloured = any(d[0] == old and model.objs[d[1]].arrays[CLR0] >= 0 for d in model.draws)
        ctl = wm.chans[0] if wm.chans else None
        out.mats[-1]["vclr"] = (bool(coloured and ctl and ctl[2] & 1), bool(coloured and ctl and ctl[3] & 1))
    for mi, oi, _bone, _xlu in sorted(model.draws, key=lambda d: d[3]):  # opaque first
        ob = model.objs[oi]
        mat = out.mats[mat_new[mi]]
        env = mat["env"]
        multi = ob.single < 0
        if not multi and drw_of(ob.single) is None:
            raise ConvertError(f"{what}: {ob.name} is bound to matrix {ob.single}, which no joint has")
        attrs = [(PNMTX, 1)] if multi else []
        if multi and env:
            attrs.append((TEX1MTX, 1))  # the shine's texture matrix, per vertex as its position's
        attrs.append((POS, 3))
        has_n, has_uv = ob.arrays[NRM] >= 0, ob.arrays[TEX0] >= 0
        has_c = any(mat["vclr"])  # (a part without colours of such a material: white)
        if has_n:
            attrs.append((NRM, 3))
        if has_c:
            attrs.append((CLR0, 3))
        if has_uv:
            attrs.append((TEX0, 3))
        packets, bb = [], None
        layer = model.mats[mi].layers[0] if model.mats[mi].layers else None
        uvm = layer.uv_mtx if layer else None  # Double Dash's drivers have none: put it in the coordinates
        if uvm and model.mats[mi].srt_mode:
            notes.append(("info", f"{what}: material {model.mats[mi].name}: its texture placement is a non-Maya kind, "
                                  "taken as Maya's (the texture may sit a little off)"))
        for pk in ob.packets:
            table = []
            slot_tab = {}
            if multi:
                for slot in range(10):
                    mid = pk.slots.get(slot)
                    d = drw_of(mid) if mid is not None else None
                    table.append(0xFFFF if d is None else d)
                    slot_tab[slot] = mid
            else:
                table = [drw_of(ob.single)]
            while multi and table and table[-1] == 0xFFFF:
                table.pop()
            dl = bytearray()
            for kind, verts in pk.prims:
                dl += struct.pack(">BH", PRIM_CODE[kind], len(verts))
                for v in verts:
                    mid = slot_tab.get(v.get(PNMTX, 0) // 3) if multi else ob.single
                    if mid not in joint_of and mid not in mix_of:
                        raise ConvertError(f"{what}: {ob.name} uses a matrix slot nothing was loaded in")
                    if mid in mix_of:  # weighted: in the rest pose, as J3D's are
                        ji, fix = None, mix_of[mid][0]
                    else:
                        ji, fix = joint_of[mid]
                    p = model.pos[ob.arrays[POS]][v[POS]]
                    if fix:
                        p = point(fix, p)
                    bb = bbox_add(bb, p if ji is None else point(world[ji], p))
                    if multi:
                        dl.append(v.get(PNMTX, 0))
                        if env:
                            dl.append(30 + v.get(PNMTX, 0))  # TEX1MTXIDX: the texture matrix of the same slot
                    dl += struct.pack(">H", add(out.pos, pos_ix, (ob.arrays[POS], v[POS], mid if fix else -1), p))
                    if has_n:
                        n = model.nrm[ob.arrays[NRM]][v[NRM]]
                        if fix:
                            n = direction(fix, n)
                        dl += struct.pack(">H", add(out.nrm, nrm_ix, (ob.arrays[NRM], v[NRM], mid if fix else -1), n))
                    if has_c:
                        ca = ob.arrays[CLR0]
                        c = model.clr[ca][v[CLR0]] if ca >= 0 and CLR0 in v and v[CLR0] < len(model.clr.get(ca, ())) else (255, 255, 255, 255)
                        dl += struct.pack(">H", add(out.clr, clr_ix, tuple(c), tuple(c)))
                    if has_uv:
                        uv = model.uv[ob.arrays[TEX0]][v[TEX0]]
                        if uvm:
                            u0, v0 = uv[0], uv[1] if len(uv) > 1 else 0.0
                            uv = (uvm[0][0] * u0 + uvm[0][1] * v0 + uvm[0][2], uvm[1][0] * u0 + uvm[1][1] * v0 + uvm[1][2])
                        dl += struct.pack(">H", add(out.uv, uv_ix, (ob.arrays[TEX0], v[TEX0], uvm), uv))
            dl += bytes(-len(dl) % 32)
            packets.append((table, bytes(dl)))
        if any(not t for t, _ in packets):
            raise ConvertError(f"{what}: {ob.name} draws before loading a matrix")
        # A J3D material draws one shape: the game's loader links each material into its joint's list,
        # and one material listed twice links to itself (the draw loop never ends). Parts sharing a
        # material are one shape when their vertices are laid out alike, else a copy of the material.
        same = next((s2 for m2, s2 in out.mesh if m2 == mat_new[mi] and out.shapes[s2][:3] == (multi, env, attrs)), None)
        if same is not None:
            m0, e0, a0, p0, b0 = out.shapes[same]
            out.shapes[same] = (m0, e0, a0, p0 + packets, bbox_add(bbox_add(b0, bb[0]), bb[1]) if b0 and bb else b0 or bb)
        else:
            out.shapes.append((multi, env, attrs, packets, bb))
            m_ix = mat_new[mi]
            if any(m2 == m_ix for m2, _ in out.mesh):
                out.mats.append(dict(mat))
                m_ix = len(out.mats) - 1
            out.mesh.append((m_ix, len(out.shapes) - 1))
        if bb:
            out.bbox = bbox_add(bbox_add(out.bbox, bb[0]), bb[1])
        if not has_n:
            notes.append(("warn", f"{what}: {ob.name} has no normals: drawn unlit"))
    if not out.shapes:
        raise ConvertError(f"{what}: nothing to draw")
    return out


HILIGHT = "lm_1"  # Mario Kart Wii's shine (a sphere map), in Double Dash's highlight place


def texture_layout(model, skel, wii_tex, notes, what):
    """the TEX1 list: the game model's order by name (its face animations pick textures by place),
    the highlight in its highlight's place; then what else the materials use"""
    sampler = {}
    for m in model.mats:
        for l in m.layers:
            sampler.setdefault(l.tex, (l.wrap_s, l.wrap_t, l.min_filter, l.mag_filter))

    def entry(name):
        fam = name.split(".")[0]
        smp = sampler.get(name) or next((v for k, v in sampler.items() if k.split(".")[0] == fam), (1, 1, 1, 1))
        if wii_tex[name].ci:
            raise ConvertError(f"{what}: texture {name} has a palette (not supported yet)")
        return (name, wii_tex[name]) + smp

    out, missing = [], []
    order = skel.textures
    if not order:  # Double Dash's way: the first texture, then each numbered set (kinoco_eye.0 ...) together
        first = [l.tex for m in model.mats for l in m.layers[:1] if l.tex in wii_tex]
        sets = sorted({n.rsplit(".", 1)[0] for n in first if "." in n})
        order = first[:1] + [n for s in sets for n in sorted((k for k in wii_tex if k.rsplit(".", 1)[0] == s and "." in k),
                                                             key=lambda k: int(k.rsplit(".", 1)[1]) if k.rsplit(".", 1)[1].isdigit() else 99)]
    alias = texture_alias(order, model, wii_tex)
    for n in order:
        n = alias.get(n, n)
        if n in wii_tex:
            out.append(entry(n))
        elif "hilight" in n.lower() and HILIGHT in wii_tex:
            out.append(entry(HILIGHT)[:2] + (0, 0, 1, 1))
        else:
            missing.append(n)
            out.append(numbered(n))  # filled below
    have = {e[0] for e in out if e and len(e) > 2}
    for m in model.mats:
        for l in m.layers[:1]:
            if l.tex not in have:
                if l.tex not in wii_tex:
                    raise ConvertError(f"{what}: material {m.name} uses texture {l.tex}, which isn't in the file")
                out.append(entry(l.tex))
                have.add(l.tex)
    if HILIGHT in wii_tex and HILIGHT not in have and not any(e and e[0] == HILIGHT for e in out):
        out.append(entry(HILIGHT)[:2] + (0, 0, 1, 1))
    if missing and not any(numbered(l.tex) for m in model.mats for l in m.layers[:1]):
        notes.append(("info", f"{what}: the eyes are painted on (no blink textures like the game's {', '.join(missing[:2])}...): "
                              "they stay open, as in Mario Kart Wii"))
    elif missing:
        notes.append(("warn", f"{what}: the game's model has textures this one doesn't ({', '.join(missing[:4])}): "
                              "its face animations show this one's eyes as they are there"))
    # places with nothing: a face animation may point there. A missing numbered one (an eye picture)
    # gets this model's own eyes (the first texture a material starts on that is numbered); the rest the
    # first texture. At least 16 places, so a face animation made for another layout points at a texture
    full = [e for e in out if e and len(e) > 2]
    first = full[0]
    eyes = next((e for e in full if numbered(e[0]) and any(l.tex == e[0] for m in model.mats for l in m.layers[:1])), first)
    out = [e if e and len(e) > 2 else eyes if e else first for e in out]
    return out + [first] * (16 - len(out)) if not skel.textures else out


def game_material_names(model, skel):
    """{material index: name}: the Wii names, except where the game's model has a material for the same
    part under another name (one on a numbered texture set, the eyes; one on a plain texture, the body)"""
    names = {i: m.name for i, m in enumerate(model.mats)}
    if not skel.mat_tex or all(m.name in skel.materials for m in model.mats):
        return names
    def kind(tex):
        return bool(tex and numbered(tex))
    theirs = [n for n in skel.materials if n not in names.values()]
    for want in (True, False):
        mine = [i for i, m in enumerate(model.mats) if m.name not in skel.materials and m.layers and kind(m.layers[0].tex) == want]
        game = [n for n in theirs if n in skel.mat_tex and kind(skel.mat_tex[n]) == want]
        if len(mine) == 1 and len(game) == 1:
            names[mine[0]] = game[0]
    return names


def numbered(n):
    """'kinoco_eye.3' -> ('kinoco_eye', 3), else None"""
    stem, _, k = n.rpartition(".")
    return (stem, int(k)) if stem and k.isdigit() else None


def texture_alias(order, model, wii_tex):
    """the game model's texture names this one has under other names (a model made from another
    character's: kino_eye.N for kinoco_eye.N): a numbered set for the set of the same size, a plain
    texture for the materials' textures in their order"""
    alias = {}
    def sets_of(names):
        nums = [x for x in map(numbered, names) if x]
        return {s: sorted({k for t, k in nums if t == s}) for s in {t for t, _ in nums}}
    theirs = sets_of([n for n in order if n not in wii_tex])
    mine = sets_of([n for n in wii_tex if not any(n == o for o in order)])

    def pair(s, m):
        """their set s shown with my set m: the same number where I have it (0 open, 1 half shut, 2 shut ...
        in both games), else my first"""
        alias.update({f"{s}.{k}": f"{m}.{k if k in mine[m] else mine[m][0]}" for k in theirs[s]})
        del mine[m]

    for s, ks in list(theirs.items()):  # the same size
        same = [m for m, mk in mine.items() if mk == ks]
        if len(same) == 1:
            pair(s, same[0])
            del theirs[s]
    used = {numbered(l.tex)[0] for mt in model.mats for l in mt.layers[:1] if numbered(l.tex)}
    for s in list(theirs):  # the same part (a set a material starts on: the eyes), then the same kind of name
        cands = [m for m in mine if m in used] if len(used & set(mine)) == 1 else []
        cands = cands or [m for m in mine if ("eye" in m.lower()) == ("eye" in s.lower())]
        if len(cands) == 1 or (cands and len(theirs) == 1):
            pair(s, cands[0])
            del theirs[s]
    plain = [l.tex for m in model.mats for l in m.layers[:1] if not numbered(l.tex) and l.tex not in order]
    for n in order:
        if n not in wii_tex and n not in alias and not numbered(n) and "hilight" not in n.lower() and plain:
            alias[n] = plain.pop(0)
    return alias


def bp_field(v, lo, n):
    return v >> lo & ((1 << n) - 1)


def material(m, tex, notes, what):
    """a Double Dash driver material for MDL0 material m: its texture lit by the game's lights,
    plus the shine (the Wii's sphere map) as the game's drivers have; cull, alpha test, blending
    and depth as the Wii material sets them"""
    def slot(name):
        return next(i for i, e in enumerate(tex) if e[0] == name)

    base = m.layers[0].tex if m.layers else None
    env = HILIGHT in [e[0] for e in tex] and base is not None
    ac = m.bp.get(0xF3, 0x3F0000)
    alpha_comp = (bp_field(ac, 16, 3), bp_field(ac, 0, 8), bp_field(ac, 22, 2), bp_field(ac, 19, 3), bp_field(ac, 8, 8))
    bl = m.bp.get(0x41, 0x34A0)
    blend_type = 3 if bl & 0x800 else 1 if bl & 1 else 2 if bl & 2 else 0
    blend = (blend_type, bp_field(bl, 8, 3), bp_field(bl, 5, 3), bp_field(bl, 12, 4))
    zm = m.bp.get(0x40, 0x17)
    zmode = (zm & 1, bp_field(zm, 1, 3), zm >> 4 & 1)
    alpha_used = alpha_comp[0] != 7 or alpha_comp[3] != 7 or blend_type
    # TEV: stage 0 texture x light (alpha: the texture's when tested or blended), stage 1 + shine x C0.a
    a_in = (7, 7, 7, 4) if alpha_used and base else (7, 7, 7, 7)
    stages = [((0xF, 8, 0xA, 2) if base else (0xF, 0xF, 0xF, 0xA), a_in)]
    orders = [(0, 0, 4) if base else (0xFF, 0xFF, 4)]
    texgens = [(1, 4, 60)] if base else []
    if env:
        stages.append(((0xF, 3, 8, 0), (7, 7, 7, 0)))
        orders.append((1, 1 if base else 0, 4))
        texgens.append((0, 1, 33))
    if m.nstage > 3:
        notes.append(("info", f"{what}: material {m.name}: {m.nstage} Wii shading steps made Double Dash's two"))
    return {
        "name": m.name, "mode": 4 if m.xlu else 1, "cull": m.cull if m.cull in (0, 1, 2, 3) else 2,
        "zcomploc": 0 if alpha_used else 1, "texgens": texgens,
        "tex": ([slot(base)] if base else []) + ([slot(HILIGHT)] if env else []),
        "orders": orders, "stages": stages, "alpha_comp": alpha_comp, "blend": blend, "zmode": zmode, "env": env,
    }


# ---- writing .bmd ----
PAD = b"This is padding data to align"


def align(buf, n, start=0):
    k = -(len(buf) - start) % n
    buf += (PAD * 4)[:k]


def name_table(names):
    out, strs = bytearray(struct.pack(">HH", len(names), 0xFFFF)), bytearray()
    base = 4 + 4 * len(names)
    for n in names:
        h = 0
        for ch in n.encode("latin-1"):
            h = (h * 3 + ch) & 0xFFFF
        out += struct.pack(">HH", h, base + len(strs))
        strs += n.encode("latin-1") + b"\0"
    return bytes(out + strs)


def section(tag, body):
    b = bytearray(tag + b"\0\0\0\0") + body
    align(b, 32)
    struct.pack_into(">I", b, 4, len(b))
    return bytes(b)


def w_inf1(m):
    h = bytearray(struct.pack(">HH", 0x10, 0))
    depth = 0
    for mi, si in m.mesh:  # every mesh hangs from the root, nested as the game's models have them
        h += struct.pack(">HHHH", 1, 0, 0x11, mi) + struct.pack(">HHHH", 1, 0, 0x12, si)
        depth += 2

    def kids(p):
        cs = [i for i, j in enumerate(m.joints) if j[2] == p]
        if not cs:
            return b""
        r = bytearray(struct.pack(">HH", 1, 0))
        for c in cs:
            r += struct.pack(">HH", 0x10, c) + kids(c)
        return bytes(r + struct.pack(">HH", 2, 0))

    h += kids(0) + struct.pack(">HH", 2, 0) * depth + struct.pack(">HH", 0, 0)
    body = struct.pack(">HHIII", 2, 0xFFFF, sum(len(s[3]) for s in m.shapes), len(m.pos), 0x18) + h
    return section(b"INF1", body)


def w_vtx1(m):
    fmts = [(POS, 1, 4, 0), (NRM, 0, 4, 0), (CLR0, 1, 5, 0), (TEX0, 1, 4, 0)]  # colours: RGBA8
    have = {POS: m.pos, NRM: m.nrm, CLR0: m.clr, TEX0: m.uv}
    b = bytearray(b"VTX1\0\0\0\0") + struct.pack(">I", 0x40) + bytes(52)
    for a, c, t, f in fmts:
        if have[a]:
            b += struct.pack(">IIIB3s", a, c, t, f, b"\xff\xff\xff")
    b += struct.pack(">IIIB3s", 0xFF, 1, 0, 0, b"\xff\xff\xff")
    slot = {POS: 0, NRM: 1, CLR0: 3, TEX0: 5}
    for a, _, _, _ in fmts:
        if not have[a]:
            continue
        align(b, 32)
        struct.pack_into(">I", b, 0xC + 4 * slot[a], len(b))
        for v in have[a]:
            b += bytes(v) if a == CLR0 else struct.pack(">%df" % len(v), *v)
    return section(b"VTX1", bytes(b[8:]))


def w_evp1(m):
    if not m.env:
        return section(b"EVP1", struct.pack(">HHIIII", 0, 0xFFFF, 0, 0, 0, 0))
    b = bytearray(b"EVP1\0\0\0\0") + struct.pack(">HHIIII", len(m.env), 0xFFFF, 0, 0, 0, 0)
    offs = [len(b)]
    b += bytes(len(e) for e in m.env)
    align(b, 2)
    offs.append(len(b))
    for e in m.env:
        b += b"".join(struct.pack(">H", j) for j, _ in e)
    align(b, 4)
    offs.append(len(b))
    for e in m.env:
        b += b"".join(struct.pack(">f", w) for _, w in e)
    align(b, 4)
    offs.append(len(b))
    for inv in m.inv:
        b += struct.pack(">12f", *(v for row in inv for v in row))
    struct.pack_into(">4I", b, 0xC, *offs)
    return section(b"EVP1", bytes(b[8:]))


def w_drw1(m):
    n = len(m.drw)
    b = bytearray(b"DRW1\0\0\0\0") + struct.pack(">HHII", n, 0xFFFF, 0x14, 0) + bytes(w for w, _ in m.drw)
    if len(b) % 2:
        b.append(0)
    struct.pack_into(">I", b, 0x10, len(b))
    for _, j in m.drw:
        b += struct.pack(">H", j)
    return section(b"DRW1", bytes(b[8:]))


def w_jnt1(m):
    n = len(m.joints)
    b = bytearray(b"JNT1\0\0\0\0") + struct.pack(">HHIII", n, 0xFFFF, 0x18, 0, 0)
    bb = m.bbox or [[0, 0, 0], [0, 0, 0]]
    rad = max(math.sqrt(sum(c * c for c in (bb[a][0], bb[b_][1], bb[c][2]))) for a in (0, 1) for b_ in (0, 1) for c in (0, 1))
    for i, (name, kind, parent, s, r, t) in enumerate(m.joints):
        rot = [int(round(a * 32768 / 180)) for a in r]
        rot = [((a + 32768) % 65536) - 32768 for a in rot]
        b += struct.pack(">HBB3f3hH3f", kind, 0 if parent <= 0 else 1, 0xFF, *s, *rot, 0xFFFF, *t)
        b += struct.pack(">7f", rad, *bb[0], *bb[1]) if i == 0 else bytes(28)
    struct.pack_into(">I", b, 0x10, len(b))
    for i in range(n):
        b += struct.pack(">H", i)
    align(b, 4)
    struct.pack_into(">I", b, 0x14, len(b))
    b += name_table([j[0] for j in m.joints])
    return section(b"JNT1", bytes(b[8:]))


def w_shp1(m):
    n = len(m.shapes)
    b = bytearray(b"SHP1\0\0\0\0") + struct.pack(">HH8I", n, 0xFFFF, *([0] * 8))
    offs = [0] * 8
    offs[0] = len(b)
    descs, desc_of = bytearray(), []
    tables, dls, inits, draws = [], bytearray(), [], []
    for multi, env, attrs, packets, bb in m.shapes:
        d = b"".join(struct.pack(">II", a, t) for a, t in attrs) + struct.pack(">II", 0xFF, 0)
        at = descs.find(d)
        if at < 0 or at % 8:
            at = len(descs)
            descs += d
        desc_of.append(at)
    k_mtx = k_draw = 0
    for si, (multi, env, attrs, packets, bb) in enumerate(m.shapes):
        bb = bb or [[0, 0, 0], [0, 0, 0]]
        rad = max(math.sqrt(sum(c * c for c in (bb[a][0], bb[b_][1], bb[c][2]))) for a in (0, 1) for b_ in (0, 1) for c in (0, 1))
        b += struct.pack(">BBHHHHH7f", 3 if multi else 0, 0xFF, len(packets), desc_of[si], k_mtx, k_draw, 0xFFFF, rad, *bb[0], *bb[1])
        for table, dl in packets:
            use = next((x for x in table if x != 0xFFFF), 0)
            inits.append((use, len(table), len(tables)))
            tables.extend(table)
            draws.append((len(dl), len(dls)))
            dls += dl
            k_mtx += 1
            k_draw += 1
    offs[1] = len(b)
    for i in range(n):
        b += struct.pack(">H", i)
    align(b, 32)
    offs[3] = len(b)
    b += descs
    align(b, 32)
    offs[4] = len(b)
    for x in tables:
        b += struct.pack(">H", x)
    align(b, 32)
    offs[5] = len(b)
    b += dls
    align(b, 32)
    offs[6] = len(b)
    for use, cnt, first in inits:
        b += struct.pack(">HHI", use, cnt, first)
    offs[7] = len(b)
    for size, off in draws:
        b += struct.pack(">II", size, off)
    struct.pack_into(">8I", b, 0xC, *offs)
    return section(b"SHP1", bytes(b[8:]))


# GX defaults of Double Dash's driver materials (J3D entries): lit by the game's light 0
CHANS = [(1, 0, 1, 2, 1, 0), (0, 0, 0, 2, 1, 0), (1, 0, 0, 1, 0, 0), (0, 0, 0, 0, 2, 0)]
SHINE = 64  # the shine's strength (C0's alpha)


def texmtx_entry(env):
    if not env:  # the plain one (texture coordinates as they are)
        return struct.pack(">BBH3f2fhH2f16f", 1, 0, 0xFFFF, 0.5, 0.5, 0.5, 1, 1, 0, 0xFFFF, 0, 0,
                           *[1.0 if i % 5 == 0 else 0.0 for i in range(16)])
    effect = [-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1]  # the normals turned to face the camera
    return struct.pack(">BBH3f2fhH2f16f", 0, 0x0B, 0xFFFF, 0.5, 0.5, 0.0, 1, 1, 0, 0xFFFF, 0, 0, *effect)


def ind_entry():
    b = struct.pack(">BBH", 0, 0, 0xFFFF) + b"\xff" * 16
    b += (struct.pack(">6f", 0.5, 0, 0, 0, 0.5, 0) + b"\x01\xff\xff\xff") * 3
    b += b"\x00\x00\xff\xff" * 4
    b += (bytes(9) + b"\xff\xff\xff") * 16
    return b


def w_mat3(m):
    n = len(m.mats)
    tabs = {k: [] for k in range(30)}

    def idx(t, v):
        if v not in tabs[t]:
            tabs[t].append(v)
        return tabs[t].index(v)

    inits = []
    for mat in m.mats:
        e = bytearray(b"\xff" * 0x14C)
        e[0] = mat["mode"]
        e[1] = idx(4, struct.pack(">I", mat["cull"]))
        e[2] = idx(6, bytes([1]))
        e[3] = idx(10, bytes([len(mat["texgens"])]))
        e[4] = idx(19, bytes([len(mat["stages"])]))
        e[5] = idx(27, bytes([mat["zcomploc"]]))
        e[6] = idx(26, bytes(mat["zmode"]) + b"\xff")
        e[7] = idx(28, bytes([1]))
        put = lambda off, vals: struct.pack_into(">%dH" % len(vals), e, off, *vals)
        put(0x08, [idx(5, b"\xff\xff\xff\xff")] * 2)
        vc, va = mat.get("vclr", (False, False))  # colour 0 / alpha 0 from the vertex colours (material source 1)
        chans = [CHANS[0][:1] + (1,) + CHANS[0][2:] if vc else CHANS[0], CHANS[1][:1] + (1,) + CHANS[1][2:] if va else CHANS[1]] + CHANS[2:]
        put(0x0C, [idx(7, bytes(c) + b"\xff\xff") for c in chans])
        put(0x14, [idx(8, b"\x32\x32\x32\x32"), idx(8, bytes(4))])
        put(0x28, [idx(11, bytes(g) + b"\xff") for g in mat["texgens"]])
        put(0x48, [idx(13, texmtx_entry(k == 1)) for k in range(len(mat["texgens"]))])
        put(0x84, [idx(15, struct.pack(">H", t)) for t in mat["tex"]])
        put(0x94, [idx(18, b"\xff\xff\xff\xff")] * 4)
        e[0x9C:0xAC] = b"\x0c" * 16
        e[0xAC:0xBC] = b"\x1c" * 16
        put(0xBC, [idx(16, bytes(o) + b"\xff") for o in mat["orders"]])
        put(0xDC, [idx(17, struct.pack(">4h", 0, 0, 0, SHINE)), idx(17, struct.pack(">4h", 255, 255, 255, 255)),
                   idx(17, struct.pack(">4h", 255, 255, 255, 255)), idx(17, bytes(8))])
        st = []
        for k, (ci, ai) in enumerate(mat["stages"]):
            st.append(idx(20, bytes([0xFF, *ci, 0, 0, 0, 1, 0, *ai, 0, 0, 0, 1 if k == 0 else 0, 0, 0xFF])))
        put(0xE4, st)
        put(0x104, [idx(21, b"\x00\x00\xff\xff")] * len(mat["stages"]))
        put(0x124, [idx(22, b"\x00\x01\x02\x03")] * 4)
        put(0x144, [idx(23, struct.pack(">BBH4f4B10H", 2, 0, 320, 0, 5000, 5, 50000, 255, 255, 255, 255, *[0] * 10))])
        ac = mat["alpha_comp"]
        put(0x146, [idx(24, bytes([ac[0], ac[1], ac[2], ac[3], ac[4]]) + b"\xff\xff\xff")])
        put(0x148, [idx(25, bytes(mat["blend"]))])
        put(0x14A, [idx(29, b"\x00\xff\xff\xff" + struct.pack(">3f", 1, 1, 1))])
        inits.append(bytes(e))
    b = bytearray(b"MAT3\0\0\0\0") + struct.pack(">HH30I", n, 0xFFFF, *([0] * 30))
    offs = [0] * 30
    offs[0] = len(b)
    for e in inits:
        b += e
    offs[1] = len(b)
    for i in range(n):
        b += struct.pack(">H", i)
    align(b, 4)
    offs[2] = len(b)
    b += name_table([mt["name"] for mt in m.mats])
    align(b, 4)
    offs[3] = len(b)
    for _ in range(n):
        b += ind_entry()
    for t in range(4, 30):
        if t in (12, 14):  # post texture coordinates: none
            continue
        align(b, 4)
        offs[t] = len(b)
        for v in tabs[t]:
            b += v
    struct.pack_into(">30I", b, 0xC, *offs)
    return section(b"MAT3", bytes(b[8:]))


def w_tex1(m):
    n = len(m.tex)
    b = bytearray(b"TEX1\0\0\0\0") + struct.pack(">HHII", n, 0xFFFF, 0x20, 0)
    align(b, 32)
    heads = len(b)
    b += bytes(32 * n)
    placed = {}
    for i, (name, t, ws, wt, mn, mg) in enumerate(m.tex):
        if name not in placed:
            align(b, 32)
            placed[name] = len(b)
            b += t.data
        h = heads + 32 * i
        alpha = 1 if t.fmt == 14 else 2 if t.fmt in (2, 3, 5, 6) else 0
        mips = t.levels > 1
        struct.pack_into(">BBHHBBBBHIBBBBBBbbBBhI", b, h, t.fmt, alpha, t.w, t.h, ws, wt, 0, 0, 0, 0, int(mips), 0, 0, 0,
                         mn if mips else min(mn, 1), mg, 0, int((t.levels - 1) * 8), t.levels, 0, 0, placed[name] - h)
    align(b, 4)
    struct.pack_into(">I", b, 0x10, len(b))
    b += name_table([e[0] for e in m.tex])
    return section(b"TEX1", bytes(b[8:]))


def write_bmd(m):
    secs = [w_inf1(m), w_vtx1(m), w_evp1(m), w_drw1(m), w_jnt1(m), w_shp1(m), w_mat3(m), w_tex1(m)]
    total = 0x20 + sum(len(s) for s in secs)
    head = b"J3D2bmd3" + struct.pack(">II", total, len(secs)) + b"SVR3" + b"\xff" * 12
    return head + b"".join(secs)


# ---- a picture of a .bmd (read back from the file: the preview checks what was written) ----
def rgb565(v):
    return ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)


def decode_texture(fmt, w, h, d):
    """a GX texture's first image -> [rows of (r, g, b, a)]"""
    if fmt not in BLOCK:
        raise ConvertError(f"texture format {fmt} can't be shown")
    bw, bh, bpp = BLOCK[fmt]
    px = [[(255, 0, 255, 255)] * w for _ in range(h)]
    p = 0

    def put(x, y, c):
        if x < w and y < h:
            px[y][x] = c

    for ty in range(0, h, bh):
        for tx in range(0, w, bw):
            if fmt == 14:  # CMPR: 4 DXT1 blocks
                for sb in range(4):
                    a, b2, bits = struct.unpack_from(">HHI", d, p)
                    p += 8
                    ca, cb = rgb565(a), rgb565(b2)
                    if a > b2:
                        pal = [ca + (255,), cb + (255,), tuple((2 * x + y) // 3 for x, y in zip(ca, cb)) + (255,),
                               tuple((x + 2 * y) // 3 for x, y in zip(ca, cb)) + (255,)]
                    else:
                        pal = [ca + (255,), cb + (255,), tuple((x + y) // 2 for x, y in zip(ca, cb)) + (255,), (0, 0, 0, 0)]
                    for k in range(16):
                        put(tx + (sb & 1) * 4 + k % 4, ty + (sb >> 1) * 4 + k // 4, pal[bits >> (30 - 2 * k) & 3])
                continue
            if fmt == 6:  # RGBA8: the AR half, then the GB half
                for k in range(16):
                    a, r = d[p + 2 * k], d[p + 2 * k + 1]
                    g, b2 = d[p + 32 + 2 * k], d[p + 33 + 2 * k]
                    put(tx + k % 4, ty + k // 4, (r, g, b2, a))
                p += 64
                continue
            for k in range(bw * bh):
                x, y = tx + k % bw, ty + k // bw
                if fmt == 0:
                    v = (d[p + k // 2] >> (0 if k & 1 else 4) & 15) * 17
                    c = (v, v, v, 255)
                elif fmt == 1:
                    c = (d[p + k],) * 3 + (255,)
                elif fmt == 2:
                    c = ((d[p + k] & 15) * 17,) * 3 + ((d[p + k] >> 4) * 17,)
                elif fmt == 3:
                    c = (d[p + 2 * k + 1],) * 3 + (d[p + 2 * k],)
                elif fmt == 4:
                    c = rgb565(u16(d, p + 2 * k)) + (255,)
                else:  # RGB5A3
                    v = u16(d, p + 2 * k)
                    c = (((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255) if v & 0x8000 else
                         ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 // 7))
                put(x, y, c)
            p += bw * bh * bpp // 8
    return px


class Mesh:
    """a .bmd's joints, triangles and textures, enough to draw it"""

    def __init__(self, b):
        s = bmd_sections(b)
        if b[:8] != b"J3D2bmd3" or any(t not in s for t in (b"INF1", b"VTX1", b"JNT1", b"SHP1", b"DRW1", b"MAT3", b"TEX1")):
            raise ConvertError("not a whole J3D model")
        sk = skeleton_of_bmd(b)
        self.names, self.parents = sk.names, sk.parents
        j = s[b"JNT1"]
        self.local = []
        for k in range(len(self.names)):
            e = j + u32(b, j + 0xC) + 0x40 * k
            sc, rot, tr = struct.unpack_from(">3f", b, e + 4), struct.unpack_from(">3h", b, e + 0x10), struct.unpack_from(">3f", b, e + 0x18)
            self.local.append(srt(sc, [a * 180 / 32768 for a in rot], tr))
        dw = s[b"DRW1"]
        n = u16(b, dw + 8)
        self.drw = [(b[dw + u32(b, dw + 0xC) + k], u16(b, dw + u32(b, dw + 0x10) + 2 * k)) for k in range(n)]
        self.env, self.invbind = [], {}
        ev = s.get(b"EVP1")
        if ev and u16(b, ev + 8):
            cnt = [b[ev + u32(b, ev + 0xC) + k] for k in range(u16(b, ev + 8))]
            p = 0
            for c in cnt:
                self.env.append([(u16(b, ev + u32(b, ev + 0x10) + 2 * (p + q)), f32(b, ev + u32(b, ev + 0x14) + 4 * (p + q))) for q in range(c)])
                p += c
            for jj in {x for e in self.env for x, _ in e}:
                self.invbind[jj] = mat34(struct.unpack_from(">12f", b, ev + u32(b, ev + 0x18) + 48 * jj))
        self._vtx(b, s)
        self._tex(b, s[b"TEX1"])
        m = s[b"MAT3"]
        offs = struct.unpack_from(">30I", b, m + 0xC)
        remap = [u16(b, m + offs[1] + 2 * k) for k in range(u16(b, m + 8))]
        self.mat_tex = []
        for k in range(len(remap)):
            e = m + offs[0] + 0x14C * remap[k]
            t = u16(b, e + 0x84)
            self.mat_tex.append(u16(b, m + offs[15] + 2 * t) if t != 0xFFFF else -1)
        self._shapes(b, s)

    def _vtx(self, b, s):
        v = s[b"VTX1"]
        end = v + u32(b, v + 4)
        offs = [u32(b, v + 0xC + 4 * k) for k in range(13)]
        fmts, p = {}, v + u32(b, v + 8)
        while u32(b, p) != 0xFF:
            fmts[u32(b, p)] = (u32(b, p + 4), u32(b, p + 8), b[p + 12])
            p += 16
        self.arr = {}
        if CLR0 in fmts and offs[3] and fmts[CLR0][1] == 5:  # RGBA8 colours (what this writes)
            nxt = min([o for o in offs if o > offs[3]] + [end - v])
            self.arr[CLR0] = [tuple(b[v + offs[3] + 4 * k:v + offs[3] + 4 * k + 4]) for k in range((nxt - offs[3]) // 4)]
        for a, slot, comps in ((POS, 0, {0: 2, 1: 3}), (NRM, 1, {0: 3}), (TEX0, 5, {0: 1, 1: 2})):
            if a not in fmts or not offs[slot]:
                continue
            cnt, typ, frac = fmts[a]
            nc = comps.get(cnt, 3)
            size = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}[typ] * nc
            nxt = min([o for o in offs if o > offs[slot]] + [end - v])
            self.arr[a] = read_array(b, nc, typ, frac, size, (nxt - offs[slot]) // size, v + offs[slot])

    def _tex(self, b, t):
        self.tex = []
        for k in range(u16(b, t + 8)):
            h = t + u32(b, t + 0xC) + 32 * k
            self.tex.append((b[h], u16(b, h + 2), u16(b, h + 4), b[h + 6], b[h + 7], h + u32(b, h + 0x1C)))
        self._b, self._img = b, {}

    def image(self, k):
        if k not in self._img:
            fmt, w, h, _, _, off = self.tex[k]
            self._img[k] = decode_texture(fmt, w, h, self._b[off:off + tex_size(fmt, w, h)])
        return self._img[k]

    def _shapes(self, b, s):
        i = s[b"INF1"]
        h, shape_mat, cur = i + u32(b, i + 0x14), {}, 0
        while u16(b, h):
            t, x = u16(b, h), u16(b, h + 2)
            if t == 0x11:
                cur = x
            elif t == 0x12:
                shape_mat[x] = cur
            h += 4
        sh = s[b"SHP1"]
        so = [u32(b, sh + 0xC + 4 * k) for k in range(8)]
        self.tris = []  # (texture, [(draw matrix, pos, nrm, uv) x3])
        for k in range(u16(b, sh + 8)):
            e = sh + so[0] + 0x28 * k
            mtype, npk, vd, mi, di = b[e], u16(b, e + 2), u16(b, e + 4), u16(b, e + 6), u16(b, e + 8)
            attrs, p = [], sh + so[3] + vd
            while u32(b, p) != 0xFF:
                attrs.append((u32(b, p), u32(b, p + 4)))
                p += 8
            tex = self.mat_tex[shape_mat.get(k, 0)] if self.mat_tex else -1
            table = [0] * 10
            for q in range(npk):
                m = sh + so[6] + 8 * (mi + q)
                use, cnt, first = u16(b, m), u16(b, m + 2), u32(b, m + 4)
                for z in range(cnt):
                    x = u16(b, sh + so[4] + 2 * (first + z))
                    if x != 0xFFFF:
                        table[z] = x
                if mtype != 3:
                    table[0] = use
                dsz, doff = u32(b, sh + so[7] + 8 * (di + q)), u32(b, sh + so[7] + 8 * (di + q) + 4)
                p, end = sh + so[5] + doff, sh + so[5] + doff + dsz
                while p < end and b[p] & 0xF8 in PRIMS:
                    kind, count = PRIMS[b[p] & 0xF8], u16(b, p + 1)
                    p += 3
                    vs = []
                    for _ in range(count):
                        v = {}
                        for a, t in attrs:
                            if t == 3:
                                v[a] = u16(b, p)
                                p += 2
                            else:
                                v[a] = b[p]
                                p += 1
                        dm = table[v.get(PNMTX, 0) // 3] if mtype == 3 else table[0]
                        vs.append((dm, v.get(POS, 0), v.get(NRM), v.get(TEX0), v.get(CLR0)))
                    for tri in strip_tris(kind, vs):
                        self.tris.append((tex, tri))

    def matrices(self, pose=None, base=IDENT):
        """each joint's matrix (pose: joint index -> local matrix in place of the rest one)"""
        w = []
        for k, par in enumerate(self.parents):
            loc = pose.get(k, self.local[k]) if pose else self.local[k]
            w.append(mul(w[par] if par >= 0 else base, loc))
        return w

    def draw_matrix(self, w, d):
        weighted, x = self.drw[d]
        if not weighted:
            return w[x]
        out = [[0.0] * 4 for _ in range(3)]
        for jj, wt in self.env[x]:
            m = mul(w[jj], self.invbind[jj])
            for r in range(3):
                for c in range(4):
                    out[r][c] += wt * m[r][c]
        return out


def strip_tris(kind, vs):
    if kind == "triangles":
        return [vs[i:i + 3] for i in range(0, len(vs) - 2, 3)]
    if kind == "strip":
        return [[vs[i], vs[i + 1], vs[i + 2]] if i % 2 == 0 else [vs[i + 1], vs[i], vs[i + 2]] for i in range(len(vs) - 2)]
    if kind == "fan":
        return [[vs[0], vs[i], vs[i + 1]] for i in range(1, len(vs) - 1)]
    if kind == "quads":
        return [t for i in range(0, len(vs) - 3, 4) for t in ([vs[i], vs[i + 1], vs[i + 2]], [vs[i], vs[i + 2], vs[i + 3]])]
    return []


def wrap(c, n, mode):
    if mode == 0:
        return min(max(c, 0), n - 1)
    if mode == 2 and (c // n) & 1:
        return n - 1 - c % n
    return c % n


def render(parts, size=320, yaw=25.0, top=None):
    """a picture (Pillow image) of models: parts = [(Mesh, joint matrices)], seen from the front,
    turned by yaw degrees; top: framed on what is above that height (the head), else the whole"""
    from PIL import Image
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    tris = []
    for mesh, w in parts:
        cache = {}
        for tex, tri in mesh.tris:
            pts = []
            for dm, pi, ni, ui, ci in tri:
                if dm not in cache:
                    cache[dm] = mesh.draw_matrix(w, dm)
                m = cache[dm]
                if pi >= len(mesh.arr.get(POS, ())):
                    break
                x, y, z = point(m, mesh.arr[POS][pi])
                uv = mesh.arr[TEX0][ui] if ui is not None and TEX0 in mesh.arr and ui < len(mesh.arr[TEX0]) else (0, 0)
                col = mesh.arr[CLR0][ci] if ci is not None and ci < len(mesh.arr.get(CLR0, ())) else (255, 255, 255, 255)
                pts.append((x * cy + z * sy, y, -x * sy + z * cy, uv, col))
            if len(pts) == 3:
                tris.append((mesh, tex, pts))
    if not tris:
        return Image.new("RGBA", (size, size))
    framed = [p for _, _, t in tris for p in t if top is None or p[1] >= top] or [p for _, _, t in tris for p in t]
    if top is not None:  # the head only: not arms held out at its height (a T pose)
        mid = sorted(p[0] for p in framed)[len(framed) // 2]
        h = max(p[1] for p in framed) - top
        framed = [p for p in framed if abs(p[0] - mid) <= h] or framed
    xs, ys = [p[0] for p in framed], [p[1] for p in framed]
    span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1
    k = size * 0.9 / span
    ox, oy = size / 2 - (max(xs) + min(xs)) / 2 * k, size / 2 + (max(ys) + min(ys)) / 2 * k
    img = Image.new("RGBA", (size, size))
    px = img.load()
    zb = [[-1e30] * size for _ in range(size)]
    light = (0.3, 0.5, 0.81)
    for mesh, tex, pts in tris:
        s = [(ox + p[0] * k, oy - p[1] * k, p[2]) for p in pts]
        ax, ay, az = (pts[1][i] - pts[0][i] for i in range(3))
        bx, by, bz = (pts[2][i] - pts[0][i] for i in range(3))
        nx, ny, nz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        shade = 0.45 + 0.55 * abs(nx * light[0] + ny * light[1] + nz * light[2]) / nl
        area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1])
        if abs(area) < 1e-9:
            continue
        img_t = mesh.image(tex) if 0 <= tex < len(mesh.tex) else None
        tw, th = (mesh.tex[tex][1], mesh.tex[tex][2]) if img_t else (1, 1)
        ws, wt = (mesh.tex[tex][3], mesh.tex[tex][4]) if img_t else (0, 0)
        x0, x1 = max(0, int(min(p[0] for p in s))), min(size - 1, int(max(p[0] for p in s)) + 1)
        y0, y1 = max(0, int(min(p[1] for p in s))), min(size - 1, int(max(p[1] for p in s)) + 1)
        for yy in range(y0, y1 + 1):
            for xx in range(x0, x1 + 1):
                fx, fy = xx + 0.5, yy + 0.5
                w0 = ((s[1][0] - fx) * (s[2][1] - fy) - (s[2][0] - fx) * (s[1][1] - fy)) / area
                w1 = ((s[2][0] - fx) * (s[0][1] - fy) - (s[0][0] - fx) * (s[2][1] - fy)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * s[0][2] + w1 * s[1][2] + w2 * s[2][2]
                if z <= zb[yy][xx]:
                    continue
                if img_t:
                    u = w0 * pts[0][3][0] + w1 * pts[1][3][0] + w2 * pts[2][3][0]
                    v = w0 * pts[0][3][1] + w1 * pts[1][3][1] + w2 * pts[2][3][1]
                    c = img_t[wrap(int(math.floor(v * th)), th, wt)][wrap(int(math.floor(u * tw)), tw, ws)]
                    if c[3] < 128:
                        continue
                else:
                    c = (200, 200, 200, 255)
                zb[yy][xx] = z
                vcol = [(w0 * pts[0][4][i] + w1 * pts[1][4][i] + w2 * pts[2][4][i]) / 255 for i in range(3)]
                px[xx, yy] = tuple(min(255, int(c[i] * shade * vcol[i])) for i in range(3)) + (255,)
    return img


def preview(files, size=320, yaw=25.0, game=None):
    """a picture of a converted character, turned by yaw degrees (preview_parts)"""
    return render(preview_parts(files, game), size, yaw)


def preview_parts(files, game=None):
    """what render() draws of a converted character (its driver.bmd, with the hair at the head); game:
    the game's model of its character (read from the disc, only to pose the cap the hair hangs from as
    the game's animations do: the converted cap is the head's)"""
    body = Mesh(files["driver.bmd"])
    pose = {}
    if game and "cap1" in body.names:
        g = Mesh(game)
        if "cap1" in g.names:
            pose[body.names.index("cap1")] = g.local[g.names.index("cap1")]
    w = body.matrices(pose)
    parts = [(body, w)]
    if "kinoko_hair.bmd" in files:
        hair = Mesh(files["kinoko_hair.bmd"])
        at = body.names.index("cap1") if "cap1" in body.names else body.names.index("face_1") if "face_1" in body.names else 0
        parts.append((hair, hair.matrices(base=w[at])))
    return parts


# ---- the whole character ----
DRIVER = "driver_model.brres"


def driver_models(szs):
    """a Wii driver archive (bytes) -> (body Model, hair Model or None, {name: Tex})"""
    try:
        files = u8_files(charpack.yaz0(szs))
    except (IndexError, struct.error) as e:
        raise ConvertError(f"a broken archive ({e})")
    brres = next((v for k, v in files.items() if k.split("/")[-1] == DRIVER), None)
    if brres is None:
        raise ConvertError(f"no {DRIVER} (a Mario Kart Wii driver file is named like sa_kart-kk.szs)")
    fold = brres_folders(brres)
    models = {n: Model(brres, o, n) for n, o in fold.get("3DModels(NW4R)", [])}
    tex = {n: Tex(brres, o) for n, o in fold.get("Textures(NW4R)", [])}
    pals = dict(fold.get("Palettes(NW4R)", []))
    for n, t in tex.items():  # palette textures: as RGBA8 (Double Dash's drivers have none)
        if t.ci:
            if t.fmt not in (8, 9, 10) or n not in pals:
                raise ConvertError(f"texture {n} has a palette that isn't in the file")
            t.depalette(palette(brres, pals[n]))
    body = models.get("model") or next((m for n, m in models.items() if n not in ("hair",) and "lod" not in n), None)
    if body is None:
        raise ConvertError("no driver model in the file")
    return body, models.get("hair"), tex


def join_hair(body, hair, at="face_1"):
    """body with hair (a separate MDL0, its root at the head as the Wii draws it) joined at joint at:
    the hair's joints become the body's (under at), so build() binds their vertices to it (rigid, as
    Double Dash has no joint of theirs). None when the body has no such joint."""
    import copy
    names = [j.name for j in body.bones]
    if at not in names:
        return None
    m = copy.copy(body)
    face = body.bones[names.index(at)]
    roots = [j for j in hair.bones if j.parent < 0]
    place = mul(face.world, inverse(roots[0].world)) if roots else face.world
    nb, nm = len(body.bones), len(body.mtx_node)
    m.bones = list(body.bones)
    for j in hair.bones:
        k = copy.copy(j)
        k.index, k.mtx_id = j.index + nb, j.mtx_id + nm
        k.parent = names.index(at) if j.parent < 0 else j.parent + nb
        k.world = mul(place, j.world)  # (inv, the bind pose's inverse, stays: weighted vertices are in the hair's space)
        m.bones.append(k)
    m.mtx_node = list(body.mtx_node) + [n + nb if n >= 0 else n for n in hair.mtx_node]
    m.mixes = dict(body.mixes)
    m.mixes.update({mid + nm: [(bm + nm, wt) for bm, wt in parts] for mid, parts in hair.mixes.items()})
    shift = {}
    for attr, key in ((POS, "pos"), (NRM, "nrm"), (CLR0, "clr"), (TEX0, "uv")):
        mine, theirs = getattr(body, key), getattr(hair, key)
        shift[attr] = max(mine, default=-1) + 1
        setattr(m, key, {**mine, **{i + shift[attr]: v for i, v in theirs.items()}})
    taken = {x.name for x in body.mats}
    m.mats = list(body.mats)
    for x in hair.mats:
        y = copy.copy(x)
        y.index = x.index + len(body.mats)
        if y.name in taken:
            y.name = "hair_" + y.name
        m.mats.append(y)
    m.objs = list(body.objs)
    for o in hair.objs:
        p = copy.copy(o)
        p.index = o.index + len(body.objs)
        p.single = o.single + nm if o.single >= 0 else o.single
        p.arrays = dict(o.arrays)
        for a in p.arrays:
            if p.arrays[a] >= 0:
                p.arrays[a] += shift[TEX0 if TEX0 <= a < TEX0 + 8 else CLR0 if a == CLR1 else a]
        p.packets = []
        for pk in o.packets:
            q = Packet({s: mid + nm for s, mid in pk.slots.items()})
            q.prims = pk.prims
            p.packets.append(q)
        m.objs.append(p)
    m.draws = list(body.draws) + [(mi + len(body.mats), oi + len(body.objs), bi + nb, x) for mi, oi, bi, x in hair.draws]
    return m


def pick_base(szs, disc, hint=None):
    """the Double Dash character a Wii driver fits best -> (BASES index or None, why): one whose skeleton
    the model has all of, with the fewest joints left over; among equals the file name's driver code
    (hint), else the one whose material names are nearest (bb_daisy_eye_tx -> Baby Mario's bbmario_eye_tx)"""
    import difflib
    body, _, _ = driver_models(szs)
    mine = {j.name for j in body.bones}
    scores = []
    for i in range(len(charpack.BASES)):
        try:
            d = disc.driver_file(i, "driver.bmd")
            sk = skeleton_of_bmd(charpack.yaz0(d)) if d else None
        except (ConvertError, OSError, ValueError, IndexError, struct.error):
            sk = None
        if sk is None:
            continue
        core = {n for n in sk.names[1:] if n not in TARGETS and n != "cap1" and not n.endswith("_pos")}
        if not core <= mine:
            continue
        near = max((difflib.SequenceMatcher(None, a.name.lower(), b.lower()).ratio() for a in body.mats for b in sk.materials), default=0)
        scores.append((-len(mine - core - ({"cap1"} & set(sk.names))), i == hint, near, i))
    if not scores:
        return None, "no Double Dash character has this model's skeleton"
    best = max(scores)
    ties = [s for s in scores if s[0] == best[0]]
    why = "its skeleton" + (f" (like {', '.join(charpack.BASES[s[3]][1] for s in ties if s is not best)}; "
                            "picked by " + ("its file name" if best[1] else "its material names") + ")" if len(ties) > 1 else "")
    return best[3], why


def convert(szs, base, disc=None):
    """a Wii driver archive (bytes) on base (charpack.BASES index) -> ({file: bytes}, notes)"""
    notes = []
    body, hair, tex = driver_models(szs)
    if any("mii" in n.lower() for n in tex):
        notes.append(("warn", "a Mii outfit: Mario Kart Wii draws the player's Mii head on it, and the file has none, "
                              "so the character has no face (only what the outfit models itself)"))
    folder = charpack.BASES[base][2]
    skel = hair_skel = None
    if disc:
        orig = disc.driver_file(base, "driver.bmd")
        if orig:
            skel = skeleton_of_bmd(charpack.yaz0(orig))
        h = disc.driver_file(base, "kinoko_hair.bmd") if folder == "kinopico" else None
        if h:
            hair_skel = skeleton_of_bmd(charpack.yaz0(h))
    else:
        notes.append(("warn", "no disc image: the skeleton is Double Dash's usual one, the textures in the Wii's "
                              "order (face animations may show the wrong picture)"))
    if skel is None:
        skel = default_skeleton(body, cap=folder in ("kinopio", "kinopico"))
    core = [n for n in skel.names[1:] if n not in TARGETS and n != "cap1"]
    missing = [n for n in core if n not in {j.name for j in body.bones}]
    if missing:
        raise ConvertError(f"the model's skeleton isn't {charpack.BASES[base][1]}'s: no {', '.join(missing[:5])}")
    out = {}
    if hair is not None and folder != "kinopico":  # Double Dash has a hair model for Toadette only: on the head
        joined = join_hair(body, hair)
        if joined is None:
            notes.append(("warn", "hair model left out: the model has no head joint (face_1) to join it to"))
        else:
            body = joined
            notes.append(("ok", f"hair joined to the head ({charpack.BASES[base][1]} has no hair model of its own)"))
        hair = None
    drv = build(body, skel, tex, notes, "driver")
    out["driver.bmd"] = write_bmd(drv)
    out["driver_200.bmd"] = out["driver.bmd"]  # the game's smaller model: the same one
    notes.append(("ok", f"driver: {len(drv.pos)} vertices, {len(drv.shapes)} parts, {len(drv.mats)} materials, "
                        f"{len(drv.tex)} textures, {len(drv.joints)} joints"))
    if hair is not None:
        if hair_skel is None:
            hair_skel = Skeleton(["k_hair_all_root"] + [j.name for j in hair.bones if j.parent >= 0],
                                 [-1] + [max(0, j.parent) for j in hair.bones if j.parent >= 0],
                                 [0] + [1] * (len(hair.bones) - 1))
        hb = build(hair, hair_skel, tex, notes, "hair")
        out["kinoko_hair.bmd"] = write_bmd(hb)
        notes.append(("ok", f"hair: {len(hb.pos)} vertices, {len(hb.joints)} joints (kinoko_hair.bmd)"))
    elif folder == "kinopico":
        notes.append(("warn", "no hair model: Toadette's own braids are drawn on it"))
    return out, notes


def write_character(out_dir, files, name, base, author="", source=""):
    """a character folder (tools/charpack.py's layout)"""
    os.makedirs(out_dir, exist_ok=True)
    for f, data in files.items():
        with open(os.path.join(out_dir, f), "wb") as fh:
            fh.write(data)
    with open(os.path.join(out_dir, "character.ini"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"name={name}\nbase={charpack.BASE_IDS[base]}\nauthor={author}\nsource={source}\n")


# ---- select screen pictures ----
def bti(img, size):
    """a Pillow image -> a BTI (RGB5A3, size x size, clamped: the game's faces' layout), fitted and centred"""
    from PIL import Image
    img = img.convert("RGBA")
    k = size / max(img.size)
    img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    sq = Image.new("RGBA", (size, size))
    sq.paste(img, ((size - img.width) // 2, (size - img.height) // 2))
    px = sq.load()
    out = bytearray(struct.pack(">BBHHBBBBHIBBBBBBbbBBhI", 5, 1, size, size, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                0, 0, 1, 0, 0, 0x20))
    for ty in range(0, size, 4):
        for tx in range(0, size, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    r, g, b, a = px[x, y]
                    v = (0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3) if a >= 0xF0 else (
                        (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4)
                    out += struct.pack(">H", v)
    return bytes(out)


ICON_FACE = ("css", "tt_", "64x64", "menu", "icon", "select")
ICON_MAP = ("map", "mini", "st_", "32x32", "race")


def driver_codes(d):
    """the Mario Kart Wii driver codes of the .szs files under folder d (sa_kart-kk.szs: kk)"""
    codes = set()
    for _, _, fs in os.walk(d):
        for f in fs:
            stem = os.path.splitext(f)[0].lower()
            if f.lower().endswith(".szs") and "-" in stem and "allkart" not in stem:
                code = re.split(r"[^a-z]", stem.rsplit("-", 1)[1].strip(" ()"))[0]
                if code:
                    codes.add(code)
    return codes


def find_icon(szs_path, small):
    """the pack's own select screen face (small: its map icon) PNG near the .szs, or None: the nearest
    folder (the .szs's, then up to two above) with one; a name sharing words with the .szs's path wins"""
    words = set(re.findall(r"[a-z]+", os.path.abspath(szs_path).lower()))
    d = os.path.dirname(os.path.abspath(szs_path))
    for level in range(3):
        if len(driver_codes(d)) > 1:
            break  # a folder of several characters' packs: their icons aren't this one's
        pngs = [os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs if f.lower().endswith(".png")]
        best = None
        if len(pngs) <= 60:  # more: a folder of many characters, not this one's pack
            for p in pngs:
                n = os.path.relpath(p, d).lower()
                is_map = any(k in n for k in ICON_MAP)
                if small != is_map or "render" in n:
                    continue
                score = (sum(k in n for k in (ICON_MAP if small else ICON_FACE)),
                         len(words & set(re.findall(r"[a-z]+", n))), -len(n))
                if best is None or score > best[0]:
                    best = (score, p)
        if best:
            return best[1]
        up = os.path.dirname(d)
        if up == d:
            break
        d = up
    return None


def face_render(files, game=None):
    """the converted character's head, rendered (a Pillow image)"""
    parts = preview_parts(files, game)
    body, w = parts[0]
    top = point(w[body.names.index("face_1")], (0, 0, 0))[1] if "face_1" in body.names else None
    img = render(parts, 192, 15, top)
    # brightened as the game's lit faces are: the 95th percentile of the drawn pixels' brightest
    # channel to 235, at most twice as bright (the Wii's vertex colours darken an unlit render)
    lum = sorted(max(p[:3]) for p in img.getdata() if p[3])
    if lum:
        k = min(2.0, 235 / max(1, lum[int(len(lum) * 0.95)]))
        if k > 1.05:
            r, g, b, a = img.split()
            r, g, b = (ch.point(lambda v: min(255, int(v * k))) for ch in (r, g, b))
            from PIL import Image
            img = Image.merge("RGBA", (r, g, b, a))
    return img


def icons(szs_path, files, game=None):
    """face.bti (and map.bti) for a converted character: the pack's own pictures, else its head rendered"""
    from PIL import Image
    out, notes = {}, []
    face = find_icon(szs_path, False) if szs_path else None
    try:
        out["face.bti"] = bti(Image.open(face), 64)
        notes.append(("ok", f"select screen face: {os.path.basename(face)}"))
    except Exception:  # none, or not a picture Pillow reads
        out["face.bti"] = bti(face_render(files, game), 64)
        notes.append(("ok", "select screen face: rendered from the model (the pack has no 64x64 icon)"))
    small = find_icon(szs_path, True) if szs_path else None
    try:
        out["map.bti"] = bti(Image.open(small), 32)
        notes.append(("ok", f"map icon: {os.path.basename(small)}"))
    except Exception:
        pass  # the game makes one from the face
    return out, notes


def guess_base(szs_name):
    """the Double Dash character a Mario Kart Wii file is (its driver code: sa_kart-kk.szs)"""
    stem = os.path.splitext(os.path.basename(szs_name))[0].lower()
    for part in reversed(stem.split("-")[1:] or [stem]):  # sa_kart-kk, mdf_kart_red-ys_2, ... -kk (v2)
        code = re.split(r"[^a-z]", part.strip(" ()"))[0]
        if code in WII_CODES:
            return WII_CODES[code]
    return None


# Mario Kart Wii driver codes -> Double Dash characters (those both games have)
WII_CODES = {"mr": "mario", "lg": "luigi", "pc": "peach", "ds": "daisy", "ys": "yoshi", "ca": "birdo",
             "bmr": "baby_mario", "blg": "baby_luigi", "ko": "toad", "kk": "toadette", "nk": "koopa",
             "dk": "donkey_kong", "dd": "diddy_kong", "kp": "bowser", "jr": "bowser_jr", "wr": "wario",
             "wl": "waluigi", "kt": "king_boo",
             # Wii-only drivers, on the Double Dash character with their skeleton
             "ka": "koopa", "bk": "bowser", "bpc": "baby_mario", "bds": "baby_mario", "fk": "donkey_kong"}


def gui(root):
    """the Wii Characters tab (MKDD-NX Tools, tools/mkddnx_tools.py) in frame root; returns the tool"""
    import json
    import threading
    import types
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    cfg_path = os.path.join(os.path.expanduser("~"), ".mkdd_nx_charpack.json")  # the disc image: the Characters tab's
    try:
        cfg = json.load(open(cfg_path))
    except (OSError, ValueError):
        cfg = {}
    szs_v, iso_v, out_v = tk.StringVar(), tk.StringVar(value=cfg.get("iso", "")), tk.StringVar(value=cfg.get("wii_out", cfg.get("out", "")))
    name_v, author_v, source_v, base_v = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar(value=charpack.BASES[9][1])
    state = {"files": None, "busy": False, "photo": None}

    def save_cfg():
        cfg.update(iso=iso_v.get(), wii_out=out_v.get())
        try:
            json.dump(cfg, open(cfg_path, "w"))
        except OSError:
            pass

    top = ttk.Frame(root, padding=8)
    top.pack(fill="x")
    ttk.Label(top, text="Converts a Mario Kart Wii driver (.szs, from Race/Kart, e.g. sa_kart-kk.szs: Toadette) into an "
                        "MKDD-NX character. Only the driver: karts aren't converted (it drives the game's). Credit the "
                        "model's author below.", wraplength=900, justify="left").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))

    def pick_szs():
        p = filedialog.askopenfilename(title="A Mario Kart Wii driver file", filetypes=[("Wii archive", "*.szs"), ("All", "*.*")])
        if p:
            set_szs(p)

    def set_szs(p):
        szs_v.set(p)
        b = guess_base(p)
        if b:
            base_v.set(charpack.BASES[charpack.base_index(b)][1])
        state["auto"] = charpack.base_index(b) if b else -1  # pick by the skeleton (with the disc) before converting
        stem = os.path.splitext(os.path.basename(p))[0]
        name_v.set(stem.split("(")[0].strip() or stem)
        run()

    def pick_iso():
        p = filedialog.askopenfilename(title="Your MKDD disc image (only read)", filetypes=[("Disc image", "*.iso *.gcm"), ("All", "*.*")])
        if p:
            iso_v.set(p)
            save_cfg()
            if szs_v.get():
                run()

    def pick_out():
        p = filedialog.askdirectory(title="Where to write the character folder")
        if p:
            out_v.set(p)
            save_cfg()

    for r, (label, var, cmd) in enumerate((("Wii driver file:", szs_v, pick_szs), ("Disc image (to match):", iso_v, pick_iso),
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
    ttk.Label(fields, text="Plays as").grid(row=0, column=6)
    base_box = ttk.Combobox(fields, textvariable=base_v, values=[b[1] for b in charpack.BASES], state="readonly", width=14,
                            height=len(charpack.BASES))  # the whole list open, no scrolling
    base_box.grid(row=0, column=7, padx=4)
    base_box.bind("<<ComboboxSelected>>", lambda e: (state.update(auto=None), szs_v.get() and run()))
    bar = ttk.Frame(root, padding=8)
    bar.pack(fill="x")
    mid = ttk.Frame(root, padding=(8, 0, 8, 8))
    mid.pack(fill="both", expand=True)
    views_row = ttk.Frame(mid)
    views_row.pack(side="top", anchor="w")
    VIEW = 320
    views = []  # each: picture, its turn (degrees), the render asked for last

    def show_view(v):
        """draw view v at its turn (in the background; a newer turn asked for meanwhile wins)"""
        parts = state.get("parts")
        if not parts:
            return
        v["seq"] += 1
        seq, yaw = v["seq"], v["yaw"].get()

        def work():
            try:
                pic = render(parts, VIEW, yaw)
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

    for k, yaw in enumerate((25, 205)):  # front, back
        col = ttk.Frame(views_row)
        col.pack(side="left", padx=(0, 8))
        v = {"yaw": tk.IntVar(value=yaw), "seq": 0}
        v["label"] = tk.Label(col, text="The converted character shows here" if k == 0 else "", width=44, height=20,
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
    notes = tk.Text(mid, height=8, wrap="word")
    notes.pack(side="top", fill="both", expand=True, pady=(8, 0))
    for level, colour in (("ok", "#1a7f37"), ("info", "#555555"), ("warn", "#9a6700"), ("error", "#cf222e")):
        notes.tag_configure(level, foreground=colour)

    def show_notes(items):
        notes.delete("1.0", "end")
        for level, text in items:
            notes.insert("end", f"{level.upper():6}{text}\n", level)

    def run():
        """convert (in the background: the preview takes a moment), then show the picture and the notes"""
        if state["busy"] or not szs_v.get():
            return
        base = charpack.base_index(base_v.get())
        state.update(busy=True, files=None)
        root.config(cursor="watch")
        show_notes([("info", "converting…")])

        auto = state.get("auto")
        state["auto"] = None  # once per file: after that, what the box says

        def work():
            b = base
            try:
                disc = charpack.Disc(iso_v.get()) if iso_v.get() else None
                with open(szs_v.get(), "rb") as f:
                    data = f.read()
                picked = []
                if auto is not None and disc:
                    k, why = pick_base(data, disc, auto if auto >= 0 else None)
                    if k is not None:
                        b = k
                        picked.append(("info", f"plays as {charpack.BASES[k][1]}: {why}"))
                files, items = convert(data, b, disc)
                game = charpack.yaz0(disc.driver_file(b, "driver.bmd") or b"") if disc else None
                pics, notes = icons(szs_v.get(), files, game or None)
                files.update(pics)
                items = picked + items + notes
                res = (files, items, preview_parts(files, game or None), b)
            except (ConvertError, OSError, ValueError) as e:
                res = (None, [("error", f"can't convert: {e}")], None, b)
            except Exception as e:  # a file this doesn't understand: say so, don't take the window with it
                res = (None, [("error", f"can't convert ({type(e).__name__}: {e})")], None, b)
            root.after(0, lambda: done(*res))

        threading.Thread(target=work, daemon=True).start()

    def done(files, items, parts, b):
        state.update(busy=False, files=files, parts=parts)
        base_v.set(charpack.BASES[b][1])  # what it was converted as (Write uses the box)
        root.config(cursor="")
        show_notes(items)
        for v in views:
            if parts:
                show_view(v)
            else:
                v["seq"] += 1
                v["photo"] = None
                v["label"].configure(image="", text="(no picture)")

    def write():
        if not state["files"]:
            messagebox.showinfo("Convert WiiMK Characters (beta)", "Pick a Wii driver file first (it converts when picked).")
            return
        if not out_v.get():
            pick_out()
        if not out_v.get():
            return
        name = name_v.get().strip() or "Wii character"
        cid = "".join(ch if ch.isalnum() else "-" for ch in name.lower()).strip("-") or "wii-character"
        dest = os.path.join(out_v.get(), cid)
        if os.path.exists(dest) and not messagebox.askyesno("Convert WiiMK Characters (beta)", f"{dest} exists. Replace its files?"):
            return
        try:
            write_character(dest, state["files"], name, charpack.base_index(base_v.get()), author_v.get().strip(), source_v.get().strip())
        except OSError as e:
            messagebox.showerror("Convert WiiMK Characters (beta)", str(e))
            return
        messagebox.showinfo("Convert WiiMK Characters (beta)", f"{name} written to\n{dest}\n\nCopy that folder to the SD card's "
                            "/switch/MKDD-NX/mods/characters/ (or add it in the Characters tab).")

    ttk.Button(bar, text="Convert again", command=run).pack(side="left", padx=(0, 6))
    ttk.Button(bar, text="Write character folder…", command=write).pack(side="left", padx=(0, 6))
    ttk.Label(bar, text="Drop a .szs on this window" if sys.platform == "win32" else "").pack(side="left", padx=12)
    charpack.enable_drop(root.winfo_toplevel(), lambda paths: paths[0].lower().endswith(".szs") and set_szs(paths[0]), root.winfo_ismapped)
    return types.SimpleNamespace(open=set_szs, can_close=lambda: not state["busy"] or messagebox.askyesno("Convert WiiMK Characters (beta)", "A conversion is running. Close anyway?"))


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="a Mario Kart Wii driver (.szs) -> an MKDD-NX character folder")
    ap.add_argument("szs")
    ap.add_argument("out", help="the character folder to write")
    ap.add_argument("--base", help="the Double Dash character it plays as (default: from the file name)")
    ap.add_argument("--name")
    ap.add_argument("--author", default="")
    ap.add_argument("--source", default="")
    ap.add_argument("--iso", help="your disc image (only read: the character's skeleton and texture order)")
    a = ap.parse_args(argv)
    b = a.base or guess_base(a.szs)
    base = charpack.base_index(b) if b else None
    disc = charpack.Disc(a.iso) if a.iso else None
    if not a.base and disc:
        with open(a.szs, "rb") as f:
            k, why = pick_base(f.read(), disc, base)
        if k is not None:
            base = k
            print(f"plays as {charpack.BASES[k][1]}: {why}")
    if base is None:
        print("which Double Dash character it plays as? --base (mario, toadette ...)")
        return 2
    try:
        with open(a.szs, "rb") as f:
            files, notes = convert(f.read(), base, disc)
        game = charpack.yaz0(disc.driver_file(base, "driver.bmd") or b"") if disc else None
        pics, more = icons(a.szs, files, game or None)
        files.update(pics)
        notes += more
    except ConvertError as e:
        print(f"can't convert: {e}")
        return 1
    name = a.name or os.path.splitext(os.path.basename(a.szs))[0]
    write_character(a.out, files, name, base, a.author, a.source)
    for level, text in notes:
        print(f"{level:5} {text}")
    print(f"written: {a.out} ({', '.join(sorted(files))})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
