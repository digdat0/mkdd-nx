"""The TRICKS row's three icons (40 x 40 RGB5A3, as the Battle row's shine00/bomb00/balloon00.bti):
a star, a jump ramp, a coin; drawn here (no game art), header from a template BTI.
py -3.10 icons.py <template balloon00.bti> <out dir>"""
import sys, struct, os
from PIL import Image, ImageDraw, ImageFilter

S = 4  # drawn at 4x, then scaled down
N = 40 * S
OUTLINE = (24, 16, 8, 255)

def outlined(mask, fill, width=3):
    """fill (RGBA image) inside mask, a dark outline round it"""
    big = mask.filter(ImageFilter.MaxFilter(2 * width * S // 2 * 2 + 1))
    out = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    out.paste(Image.new('RGBA', (N, N), OUTLINE), (0, 0), big)
    out.paste(fill, (0, 0), mask)
    return out

def star():
    import math
    cx, cy, R, r = N / 2, N / 2 + 1.5 * S, 17.5 * S, 7.6 * S
    pts = [(cx + (R if k % 2 == 0 else r) * math.cos(-math.pi / 2 + k * math.pi / 5),
            cy + (R if k % 2 == 0 else r) * math.sin(-math.pi / 2 + k * math.pi / 5)) for k in range(10)]
    m = Image.new('L', (N, N), 0); ImageDraw.Draw(m).polygon(pts, fill=255)
    f = Image.new('RGBA', (N, N), (255, 204, 32, 255)); d = ImageDraw.Draw(f)
    # a lighter upper half, a gold lower edge
    d.rectangle((0, 0, N, cy - 2 * S), fill=(255, 226, 80, 255))
    d.polygon([(p[0], p[1]) for p in pts[5:8]] + [(cx, cy + 3 * S)], fill=(232, 160, 16, 255))
    for ex in (-3.6, 3.6):  # eyes
        d.ellipse((cx + (ex - 1.5) * S, cy - 5.5 * S, cx + (ex + 1.5) * S, cy + 0.5 * S), fill=(20, 16, 8, 255))
        d.ellipse((cx + (ex - 0.7) * S, cy - 4.8 * S, cx + (ex + 0.3) * S, cy - 3.2 * S), fill=(255, 255, 255, 255))
    return outlined(m, f)

def ramp():
    x0, x1, yb, yt = 4 * S, 36 * S, 34 * S, 12 * S
    pts = [(x0, yb), (x1, yb), (x1, yt)]
    m = Image.new('L', (N, N), 0); ImageDraw.Draw(m).polygon(pts, fill=255)
    f = Image.new('RGBA', (N, N), (232, 92, 24, 255)); d = ImageDraw.Draw(f)
    d.polygon([(x1 - 8 * S, yb), (x1, yb), (x1, yt)], fill=(184, 56, 16, 255))  # its side, darker
    for k in (0.3, 0.55, 0.8):  # white stripes across the slope
        xa = x0 + (x1 - x0) * k
        d.line([(xa, yb), (xa + 5 * S, yb - (xa + 5 * S - x0) / (x1 - x0) * (yb - yt))], fill=(255, 240, 220, 255), width=int(2.2 * S))
    ic = outlined(m, f)
    # a jump arc with an arrow head, above the ramp
    a = Image.new('L', (N, N), 0); da = ImageDraw.Draw(a)
    da.arc((10 * S, 2 * S, 40 * S, 30 * S), 200, 300, fill=255, width=int(2.6 * S))
    da.polygon([(30.5 * S, 1.5 * S), (37 * S, 5.5 * S), (30 * S, 8.5 * S)], fill=255)
    ic2 = outlined(a, Image.new('RGBA', (N, N), (255, 255, 255, 255)), 2)
    ic.alpha_composite(ic2)
    return ic

def coin():
    cx, cy, rx, ry = N / 2, N / 2, 12.5 * S, 16.5 * S
    m = Image.new('L', (N, N), 0); ImageDraw.Draw(m).ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=255)
    f = Image.new('RGBA', (N, N), (232, 154, 16, 255)); d = ImageDraw.Draw(f)
    d.ellipse((cx - rx * 0.78, cy - ry * 0.8, cx + rx * 0.78, cy + ry * 0.8), fill=(255, 204, 40, 255))
    d.rounded_rectangle((cx - 1.7 * S, cy - 8 * S, cx + 1.7 * S, cy + 8 * S), radius=1.7 * S, fill=(255, 246, 192, 255))
    return outlined(m, f)

def rgb5a3(im):
    out = bytearray()
    for by in range(0, 40, 4):
        for bx in range(0, 40, 4):
            for y in range(4):
                for x in range(4):
                    r, g, b, a = im.getpixel((bx + x, by + y))
                    if a >= 0xF0:
                        v = 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
                    else:
                        v = (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)
                    out += struct.pack('>H', v)
    return bytes(out)

def save(im, template, path):
    t = open(template, 'rb').read()
    assert t[0] == 5 and struct.unpack('>HH', t[2:6]) == (40, 40), 'template: a 40 x 40 RGB5A3 BTI'
    off = struct.unpack('>I', t[0x1c:0x20])[0]
    open(path, 'wb').write(t[:off] + rgb5a3(im))

if __name__ == '__main__':
    tmpl, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    sheet = Image.new('RGBA', (130, 44), (0, 0, 120, 255))
    for k, (name, fn) in enumerate([('trick_star00.bti', star), ('trick_ramp00.bti', ramp), ('trick_coin00.bti', coin)]):
        im = fn().resize((40, 40), Image.LANCZOS)
        save(im, tmpl, os.path.join(out, name))
        sheet.alpha_composite(im, (k * 44, 2))
    sheet.resize((520, 176), Image.NEAREST).save(os.path.join(out, 'trick_icons_preview.png'))
    print('icons written to', out)
