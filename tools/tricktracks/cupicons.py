"""The trick cups' icons (2026-11-12), in the style of a Mario item: bold shapes, glossy shading, a dark outline and a
white sticker rim. acorn, tornado, green pipe, snowy shell; each S x S RGBA (PIL Image).
usage alone: py -3.10 tools/cupicons.py <out.png> [<Art dir>]  (a sheet; with a dir: the trophies, ui_trophy1-3.rgba, written there)"""
import math, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

Z = 4  # supersampling


def _grad(size, box, c0, c1, horiz=False):
    """an RGBA layer: a linear gradient c0 -> c1 over box (x0, y0, x1, y1)"""
    W, H = size
    x0, y0, x1, y1 = box
    t = (np.arange(W) - x0) / max(1, x1 - x0) if horiz else (np.arange(H) - y0) / max(1, y1 - y0)
    t = np.clip(t, 0, 1)
    g = np.zeros((H, W, 4), np.uint8); g[..., 3] = 255
    for k in range(3):
        v = (c0[k] + (c1[k] - c0[k]) * t).astype(np.uint8)
        g[..., k] = v[None, :] if horiz else v[:, None]
    return Image.fromarray(g, 'RGBA')


def _fill(base, mask, layer):
    base.paste(layer, (0, 0), mask)


def _mask(size):
    m = Image.new('L', size, 0)
    return m, ImageDraw.Draw(m)


def _sticker(art, S):
    """dark outline then a white rim round the art's alpha, a soft drop shadow"""
    a = art.split()[3]
    dark = a.filter(ImageFilter.MaxFilter(4 * Z + 1))
    white = dark.filter(ImageFilter.MaxFilter(5 * Z + 1))
    out = Image.new('RGBA', art.size, (0, 0, 0, 0))
    sh = white.filter(ImageFilter.GaussianBlur(3 * Z))
    shadow = Image.new('RGBA', art.size, (0, 0, 40, 0)); shadow.putalpha(sh.point(lambda v: v * 0.55))
    out.alpha_composite(shadow, (2 * Z, 4 * Z))
    w = Image.new('RGBA', art.size, (255, 255, 255, 255)); out.paste(w, (0, 0), white)
    d = Image.new('RGBA', art.size, (24, 18, 30, 255)); out.paste(d, (0, 0), dark)
    out.alpha_composite(art)
    return out.resize((S, S), Image.LANCZOS)


def _gloss(art, box, alpha=120):
    """a white highlight ellipse, clipped to the art"""
    hl = Image.new('RGBA', art.size, (0, 0, 0, 0)); ImageDraw.Draw(hl).ellipse(box, fill=(255, 255, 255, alpha))
    hl = hl.filter(ImageFilter.GaussianBlur(2 * Z))
    clip = ImageChops.multiply(hl.split()[3], art.split()[3])
    hl.putalpha(clip)
    art.alpha_composite(hl)


def acorn(S=160):
    N = S * Z; sz = (N, N); art = Image.new('RGBA', sz, (0, 0, 0, 0)); u = N / 160
    def B(*v): return [x * u for x in v]
    # the nut
    m, d = _mask(sz); d.ellipse(B(38, 52, 122, 150), fill=255)
    _fill(art, m, _grad(sz, B(38, 52, 122, 150), (236, 160, 80), (150, 74, 28), horiz=True))
    m2, d2 = _mask(sz); d2.polygon(B(80, 154, 72, 140, 88, 140), fill=255); _fill(art, m2, Image.new('RGBA', sz, (150, 74, 28, 255)))
    _gloss(art, B(52, 86, 72, 130), 110)
    # the cap: a dome, cross-hatched
    m, d = _mask(sz); d.chord(B(28, 30, 132, 110), 180, 360, fill=255); d.rounded_rectangle(B(28, 64, 132, 82), 9 * u, fill=255)
    cap = _grad(sz, B(28, 30, 132, 82), (176, 120, 64), (96, 58, 24))
    cd = ImageDraw.Draw(cap)
    for i in range(-6, 12):
        x = 28 + i * 12
        cd.line(B(x, 30, x + 52, 82), fill=(78, 44, 18, 255), width=int(3 * u))
        cd.line(B(x + 52, 30, x, 82), fill=(78, 44, 18, 255), width=int(3 * u))
    _fill(art, m, cap)
    m, d = _mask(sz); d.rounded_rectangle(B(28, 72, 132, 84), 6 * u, fill=255)
    _fill(art, m, Image.new('RGBA', sz, (70, 40, 16, 255)))
    _gloss(art, B(46, 36, 92, 54), 90)
    # the stem
    m, d = _mask(sz); d.rounded_rectangle(B(74, 12, 88, 40), 6 * u, fill=255)
    _fill(art, m, _grad(sz, B(74, 12, 88, 40), (140, 92, 46), (90, 54, 22), horiz=True))
    return _sticker(art, S)


def tornado(S=160):
    N = S * Z; sz = (N, N); art = Image.new('RGBA', sz, (0, 0, 0, 0)); u = N / 160
    def B(*v): return [x * u for x in v]
    # rings from the top (wide) down to the tip, each a little to the side of the last
    rings = []
    n = 7
    for i in range(n):
        t = i / (n - 1)
        w = 118 - 92 * t
        cx = 80 + 10 * math.sin(t * 3.4)
        y = 22 + 114 * t
        rings.append((cx, y, w))
    body, d = _mask(sz)
    for (cx, y, w) in rings:
        d.ellipse(B(cx - w / 2, y - 9 - 6 * w / 118, cx + w / 2, y + 9 + 6 * w / 118), fill=255)
    for a, b in zip(rings, rings[1:]):
        d.polygon(B(a[0] - a[2] / 2, a[1], b[0] - b[2] / 2, b[1], b[0] + b[2] / 2, b[1], a[0] + a[2] / 2, a[1]), fill=255)
    _fill(art, body, _grad(sz, B(20, 0, 140, 160), (236, 246, 255), (130, 160, 200), horiz=True))
    ad = ImageDraw.Draw(art)
    for i, (cx, y, w) in enumerate(rings):
        hh = 9 + 6 * w / 118
        ad.arc(B(cx - w / 2 + 3, y - hh, cx + w / 2 - 3, y + hh), 15, 165, fill=(96, 128, 176, 255), width=int(3.2 * u))
        ad.arc(B(cx - w / 2 + 8, y - hh + 3, cx + w / 2 - 8, y + hh - 3), 200, 300, fill=(255, 255, 255, 230), width=int(2.4 * u))
    _gloss(art, B(34, 18, 70, 70), 90)
    # a few flecks round it
    return _sticker(art, S)


def ice_shell(S=160):
    """a blue shell (a smooth Koopa shell, no spikes or wings) with snow heaped on its top and icicles off the rim"""
    N = S * Z; sz = (N, N); art = Image.new('RGBA', sz, (0, 0, 0, 0)); u = N / 160
    def B(*v): return [x * u for x in v]
    # the underside, dark brown, peeking below the rim
    m, d = _mask(sz); d.ellipse(B(24, 90, 136, 134), fill=255)
    _fill(art, m, _grad(sz, B(24, 96, 136, 134), (120, 90, 62), (44, 30, 20)))
    # the shell's dome, low and wide: sky blue to deep blue
    dome, d = _mask(sz); d.chord(B(16, 38, 144, 142), 180, 360, fill=255); d.rectangle(B(16, 89, 144, 102), fill=255)
    _fill(art, dome, _grad(sz, B(16, 0, 144, 0), (140, 226, 252), (16, 98, 186), horiz=True))
    # the plates: a centre hexagon and the seams round it
    ad = ImageDraw.Draw(art); sc = (12, 66, 146, 255); lw = int(3 * u)
    hexp = [(80, 56), (102, 66), (102, 84), (80, 94), (58, 84), (58, 66)]
    ad.polygon(B(*[v for p in hexp for v in p]), outline=sc, width=lw)
    for (x0, y0, x1, y1) in ((58, 66, 30, 62), (102, 66, 130, 62), (58, 84, 24, 94), (102, 84, 136, 94), (80, 56, 80, 40)):
        ad.line(B(x0, y0, x1, y1), fill=sc, width=lw)
    # the rim: a cream band, iced
    m, d = _mask(sz); d.rounded_rectangle(B(12, 92, 148, 108), 8 * u, fill=255)
    _fill(art, m, _grad(sz, B(12, 92, 148, 108), (255, 255, 250), (206, 222, 236)))
    for (x, h) in ((30, 10), (46, 7), (62, 12), (84, 8), (102, 12), (120, 7), (134, 9)):  # icicles
        m, d = _mask(sz); d.polygon(B(x - 4, 105, x + 4, 105, x, 105 + h), fill=255)
        _fill(art, m, _grad(sz, B(x - 4, 105, x + 4, 105 + h), (240, 252, 255), (140, 196, 240)))
    # the snow on top: a cap with lumpy drips, kept to the dome, a small heap above it
    snow, d = _mask(sz); d.ellipse(B(26, 26, 134, 62), fill=255)
    for (x, y, r) in ((34, 58, 8), (50, 62, 10), (68, 58, 7), (86, 63, 10), (104, 58, 8), (122, 57, 7)):
        d.ellipse(B(x - r, y - r, x + r, y + r), fill=255)
    top, d2 = _mask(sz); d2.ellipse(B(48, 30, 112, 50), fill=255)
    snow = ImageChops.lighter(ImageChops.multiply(snow, dome), top)
    _fill(art, snow, _grad(sz, B(0, 30, 0, 72), (255, 255, 255), (196, 222, 246)))
    ImageDraw.Draw(art).arc(B(56, 36, 104, 56), 25, 155, fill=(176, 204, 236, 255), width=int(2.2 * u))
    _gloss(art, B(28, 64, 48, 88), 120)
    _gloss(art, B(60, 32, 86, 42), 160)
    return _sticker(art, S)


def green_pipe(S=160):
    N = S * Z; sz = (N, N); art = Image.new('RGBA', sz, (0, 0, 0, 0)); u = N / 160
    def B(*v): return [x * u for x in v]
    def body_grad(box): return _grad(sz, B(*box), (40, 120, 30), (40, 120, 30), horiz=True)
    # the body
    m, d = _mask(sz); d.rectangle(B(42, 62, 118, 146), fill=255)
    g = _grad(sz, B(42, 0, 118, 0), (150, 240, 110), (20, 96, 24), horiz=True)
    _fill(art, m, g)
    # the lip
    m, d = _mask(sz); d.rounded_rectangle(B(28, 26, 132, 66), 6 * u, fill=255)
    _fill(art, m, _grad(sz, B(28, 0, 132, 0), (170, 250, 130), (24, 110, 28), horiz=True))
    # the opening on top
    m, d = _mask(sz); d.ellipse(B(36, 20, 124, 38), fill=255); _fill(art, m, Image.new('RGBA', sz, (12, 40, 12, 255)))
    ad = ImageDraw.Draw(art)
    ad.ellipse(B(36, 20, 124, 38), outline=(110, 210, 90, 255), width=int(3 * u))
    # highlight stripes
    ad.rectangle(B(52, 70, 60, 142), fill=(220, 255, 200, 200))
    ad.rectangle(B(40, 40, 50, 62), fill=(230, 255, 210, 210))
    ad.line(B(42, 66, 118, 66), fill=(14, 60, 14, 255), width=int(3 * u))
    ad.line(B(42, 146, 118, 146), fill=(14, 60, 14, 255), width=int(3 * u))
    return _sticker(art, S)



TROPHY_COL = (((255, 246, 170), (232, 168, 20), (130, 80, 0)),     # gold
              ((255, 255, 255), (176, 188, 206), (80, 92, 112)),   # silver
              ((255, 220, 180), (200, 120, 60), (100, 50, 20)))    # bronze


def trophy(S=256, tier=0):
    """a podium trophy, gold / silver / bronze (tier 0..2): bowl, handles, stem, base with a star"""
    N = S * Z; sz = (N, N); art = Image.new('RGBA', sz, (0, 0, 0, 0)); u = N / 160
    def B(*v): return [x * u for x in v]
    hi, mid, lo = TROPHY_COL[tier]
    ad = ImageDraw.Draw(art)
    for side in (-1, 1):  # handles
        cx = 80 + side * 46
        ad.ellipse(B(cx - 20, 26, cx + 20, 78), outline=lo + (255,), width=int(11 * u))
        ad.ellipse(B(cx - 20, 26, cx + 20, 78), outline=mid + (255,), width=int(6 * u))
    m, d = _mask(sz); d.chord(B(30, -34, 130, 104), 0, 180, fill=255); d.rectangle(B(30, 14, 130, 36), fill=255)
    _fill(art, m, _grad(sz, B(30, 0, 130, 0), hi, lo, horiz=True))
    m, d = _mask(sz); d.ellipse(B(30, 8, 130, 22), fill=255); _fill(art, m, Image.new('RGBA', sz, lo + (255,)))
    ad = ImageDraw.Draw(art)
    ad.ellipse(B(30, 8, 130, 22), outline=hi + (255,), width=int(2.4 * u))
    m, d = _mask(sz); d.polygon(B(70, 100, 90, 100, 86, 120, 74, 120), fill=255); d.rounded_rectangle(B(60, 116, 100, 126), 3 * u, fill=255)
    _fill(art, m, _grad(sz, B(60, 0, 100, 0), hi, lo, horiz=True))
    m, d = _mask(sz); d.rounded_rectangle(B(40, 124, 120, 152), 6 * u, fill=255)
    _fill(art, m, _grad(sz, B(0, 124, 0, 152), (110, 60, 40), (50, 24, 14)))
    # a star on the bowl, the place's number on the base
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5; r = 22 if i % 2 == 0 else 9
        pts += [80 + r * math.cos(a), 56 + r * math.sin(a)]
    ImageDraw.Draw(art).polygon(B(*pts), fill=(255, 255, 255, 235))
    ImageDraw.Draw(art).rounded_rectangle(B(66, 129, 94, 147), 3 * u, fill=mid + (255,), outline=hi + (255,), width=int(1.5 * u))
    _gloss(art, B(38, 14, 58, 70), 130)
    return _sticker(art, S)


def rgba_file(path, im):
    im = im.convert('RGBA')
    with open(path, 'wb') as f:
        f.write(bytes((im.width & 255, im.width >> 8, im.height & 255, im.height >> 8)) + im.tobytes())


ICONS = (acorn, tornado, green_pipe, ice_shell)  # Kicker, Canyon, City, Summit

if __name__ == '__main__':
    sheet = Image.new('RGBA', (4 * 180, 200), (20, 40, 110, 255))
    for i, f in enumerate(ICONS):
        sheet.alpha_composite(f(160), (i * 180 + 10, 20))
    for t in range(3):
        sheet.alpha_composite(trophy(160, t), (t * 180 + 10, 20)) if len(sys.argv) > 2 else None
        if len(sys.argv) > 2: rgba_file('%s/ui_trophy%d.rgba' % (sys.argv[2], t + 1), trophy(256, t))
    sheet.save(sys.argv[1])
