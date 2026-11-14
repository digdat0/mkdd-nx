"""BTI texel codecs for the trick stage art: IA4 (2), RGB5A3 (5), CMPR (14). decode(bti bytes) -> RGBA image;
encode(img, template bti bytes) -> bti bytes with the template's header (same format and size)."""
import struct
import numpy as np
from PIL import Image

BLK = {2: (8, 4), 5: (4, 4), 14: (8, 8)}

def head(b):
    fmt, _, w, h = struct.unpack('>BBHH', b[:6])
    return fmt, w, h, struct.unpack('>I', b[0x1c:0x20])[0]

def _565(v): return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31

def decode(b):
    fmt, w, h, off = head(b); px = b[off:]
    out = np.zeros((h, w, 4), np.uint8); p = 0
    bw, bh = BLK[fmt]
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == 2:
                for y in range(4):
                    for x in range(8):
                        v = px[p]; p += 1; i = (v & 15) * 17
                        out[by + y, bx + x] = (i, i, i, (v >> 4) * 17)
            elif fmt == 5:
                for y in range(4):
                    for x in range(4):
                        v = struct.unpack('>H', px[p:p + 2])[0]; p += 2
                        if v & 0x8000: c = (((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
                        else: c = (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7)
                        out[by + y, bx + x] = c
            else:
                for sy in (0, 4):
                    for sx in (0, 4):
                        c0, c1, bits = struct.unpack('>HHI', px[p:p + 8]); p += 8
                        a, b_ = _565(c0), _565(c1)
                        if c0 > c1: pal = [a + (255,), b_ + (255,), tuple((2 * a[i] + b_[i]) // 3 for i in range(3)) + (255,), tuple((a[i] + 2 * b_[i]) // 3 for i in range(3)) + (255,)]
                        else: pal = [a + (255,), b_ + (255,), tuple((a[i] + b_[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                        for k in range(16):
                            out[by + sy + k // 4, bx + sx + k % 4] = pal[(bits >> (30 - 2 * k)) & 3]
    return Image.fromarray(out, 'RGBA')

def _to565(c):
    r, g, b = [int(x) for x in c[:3]]
    return (r * 31 + 127) // 255 << 11 | (g * 63 + 127) // 255 << 5 | (b * 31 + 127) // 255

def _cmpr_block(blk):
    """4x4 RGBA -> 8 bytes; transparent texels (a < 128) use the 3-colour mode's transparent index"""
    pts = blk.reshape(16, 4).astype(float)
    opaque = pts[:, 3] >= 128
    if not opaque.any(): return struct.pack('>HHI', 0, 0xffff, 0xffffffff)
    rgb = pts[opaque, :3]
    m = rgb.mean(0); d = rgb - m
    ax = np.linalg.svd(d, full_matrices=False)[2][0] if len(rgb) > 1 and np.abs(d).sum() > 0 else np.array([1, 1, 1]) / 3 ** .5
    t = d @ ax; e0, e1 = m + ax * t.max(), m + ax * t.min()
    c0, c1 = _to565(np.clip(e0, 0, 255)), _to565(np.clip(e1, 0, 255))
    trans = not opaque.all()
    if trans:
        if c0 > c1: c0, c1 = c1, c0
    else:
        if c0 == c1: return struct.pack('>HHI', c0, c1, 0) if c0 else struct.pack('>HHI', 1, 0, 0x55555555)
        if c0 < c1: c0, c1 = c1, c0
    a, b = np.array(_565(c0), float), np.array(_565(c1), float)
    pal = [a, b, (2 * a + b) / 3, (a + 2 * b) / 3] if not trans else [a, b, (a + b) / 2]
    bits = 0
    for k in range(16):
        if trans and pts[k, 3] < 128: idx = 3
        else: idx = int(np.argmin([((pts[k, :3] - q) ** 2).sum() for q in pal]))
        bits |= idx << (30 - 2 * k)
    return struct.pack('>HHI', c0, c1, bits)

def encode(img, tmpl):
    fmt, w, h, off = head(tmpl)
    img = img.convert('RGBA')
    assert img.size == (w, h), (img.size, w, h)
    a = np.asarray(img); o = bytearray()
    bw, bh = BLK[fmt]
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == 2:
                for y in range(4):
                    for x in range(8):
                        r, g, b, al = a[by + y, bx + x]
                        i = (int(r) * 30 + int(g) * 59 + int(b) * 11) // 100
                        o.append(((int(al) * 15 + 127) // 255) << 4 | ((i * 15 + 127) // 255))
            elif fmt == 5:
                for y in range(4):
                    for x in range(4):
                        r, g, b, al = [int(v) for v in a[by + y, bx + x]]
                        v = (0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)) if al >= 0xE0 else ((al >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4))
                        o += struct.pack('>H', v)
            else:
                for sy in (0, 4):
                    for sx in (0, 4): o += _cmpr_block(a[by + sy:by + sy + 4, bx + sx:bx + sx + 4])
    out = tmpl[:off] + bytes(o)
    assert len(out) == len(tmpl), (len(out), len(tmpl))
    return out

if __name__ == '__main__':
    import sys
    for n in sys.argv[1:]:
        im = decode(open(n, 'rb').read())
        bg = Image.new('RGBA', im.size, (0, 0, 120, 255)); bg.alpha_composite(im)
        bg.convert('RGB').resize((im.size[0] * 2, im.size[1] * 2)).save(n + '.png')
