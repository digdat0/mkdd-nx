"""Original textures for the Trick Style stadium (build6.py): everything drawn here from noise and shapes, nothing
taken from the game. make(dir) writes the PNGs and returns {name: (format, wrapS, wrapT)}."""
import os, math, random
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

def _noise(n, scale, seed, m=None):
    """seamless (wrapping) smooth noise n x m in 0..1: white noise low-passed in the frequency domain"""
    m = m or n
    r = np.random.default_rng(seed).standard_normal((m, n))
    fy = np.fft.fftfreq(m)[:, None] * m; fx = np.fft.fftfreq(n)[None, :] * n
    k = np.exp(-(fx ** 2 + fy ** 2) / (2.0 * scale ** 2))
    o = np.real(np.fft.ifft2(np.fft.fft2(r) * k))
    o -= o.min(); o /= max(o.max(), 1e-9)
    return o

def _rgb(a):
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGB')

def _font(sz):
    for f in ('impact.ttf', 'arialbd.ttf', 'arial.ttf'):
        try: return ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:/Windows'), 'Fonts', f), sz)
        except OSError: pass
    return ImageFont.load_default()

def dirt(n=256):
    a = 0.55 * _noise(n, 3, 1) + 0.30 * _noise(n, 12, 2) + 0.15 * _noise(n, 40, 3)
    base = np.array([158, 112, 68], float); dark = np.array([104, 70, 42], float)
    img = dark + (base - dark) * a[..., None]
    rng = random.Random(4); im = _rgb(img); d = ImageDraw.Draw(im)
    for _ in range(170):                      # pebbles (drawn wrapped)
        x, y, r = rng.uniform(0, n), rng.uniform(0, n), rng.uniform(0.8, 2.4)
        c = rng.choice([(186, 150, 110), (90, 64, 40), (172, 132, 92)])
        for ox in (-n, 0, n):
            for oy in (-n, 0, n): d.ellipse((x + ox - r, y + oy - r, x + ox + r, y + oy + r), fill=c)
    return im.filter(ImageFilter.SMOOTH)

def ramp(n=128):
    """the jump faces: orange boards running along the ridge (u), seams every 16 px in v, with grain"""
    g = _noise(n, 20, 5, n) * 0.5 + _noise(n, 4, 6, n) * 0.5
    img = np.zeros((n, n, 3)); img[...] = [236, 118, 22]
    img *= (0.82 + 0.25 * g)[..., None]
    for y in range(0, n, 16):
        img[y:y + 2] *= 0.55; img[y + 2] *= 1.12
    im = _rgb(img); d = ImageDraw.Draw(im)
    for y0 in range(0, n, 16):                # bolts
        for x0 in range(8, n, 32): d.ellipse((x0 - 1.5, y0 + 6.5, x0 + 1.5, y0 + 9.5), fill=(120, 60, 20))
    return im

def ramp_side(n=128):
    """the jump's ends: a darker frame with cross braces"""
    img = np.zeros((n, n, 3)); img[...] = [150, 70, 18]
    img *= (0.85 + 0.2 * _noise(n, 10, 7))[..., None]
    im = _rgb(img); d = ImageDraw.Draw(im)
    d.line((0, 0, n, n), fill=(205, 105, 30), width=9); d.line((0, n, n, 0), fill=(205, 105, 30), width=9)
    d.rectangle((0, 0, n - 1, n - 1), outline=(95, 45, 12), width=6)
    return im

def lip(n=64):
    im = Image.new('RGB', (n, n)); d = ImageDraw.Draw(im)
    s = n // 4
    for i in range(4):
        for j in range(4): d.rectangle((i * s, j * s, i * s + s - 1, j * s + s - 1), fill=(245, 245, 245) if (i + j) % 2 else (215, 30, 30))
    return im

def boost(n=128):
    """blue pad with chevrons pointing up the texture (+v = toward the jump)"""
    y = np.linspace(0, 1, n)[:, None] * np.ones((1, n))
    img = np.zeros((n, n, 3))
    img[..., 0] = 20 + 30 * y; img[..., 1] = 80 + 90 * y; img[..., 2] = 200 + 55 * y
    im = _rgb(img); d = ImageDraw.Draw(im)
    for k in range(2):
        y0 = k * n // 2 + n // 8
        pts = [(n * 0.12, y0 + n * 0.30), (n * 0.5, y0), (n * 0.88, y0 + n * 0.30), (n * 0.88, y0 + n * 0.42), (n * 0.5, y0 + n * 0.14), (n * 0.12, y0 + n * 0.42)]
        d.polygon(pts, fill=(255, 236, 60))
    d.rectangle((0, 0, 5, n), fill=(235, 240, 255)); d.rectangle((n - 6, 0, n, n), fill=(235, 240, 255))
    return im.transpose(Image.FLIP_TOP_BOTTOM)   # image rows run top-down, texture t runs bottom-up

def barrier(w=256, h=64):
    """the arena wall: red/white blocks with a TRICK STYLE board in the middle"""
    im = Image.new('RGB', (w, h), (240, 240, 240)); d = ImageDraw.Draw(im)
    for i in range(0, w, 32):
        if (i // 32) % 2: d.rectangle((i, 0, i + 31, h), fill=(205, 28, 28))
    d.rectangle((48, 10, w - 49, h - 11), fill=(20, 24, 60))
    f = _font(30); t = 'TRICK STYLE'
    bb = d.textbbox((0, 0), t, font=f); tw, th = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((w - tw) // 2 - bb[0], (h - th) // 2 - bb[1]), t, font=f, fill=(255, 200, 30))
    d.rectangle((0, 0, w, 3), fill=(60, 60, 60)); d.rectangle((0, h - 4, w, h), fill=(60, 60, 60))
    return im

def crowd(n=256):
    """the stands: rows of seats with people (one row per 32 px), seamless"""
    im = Image.new('RGB', (n, n), (70, 70, 84)); d = ImageDraw.Draw(im); rng = random.Random(9)
    shirts = [(220, 40, 40), (40, 90, 220), (250, 210, 40), (40, 170, 70), (240, 240, 240), (240, 120, 30), (150, 60, 190), (30, 30, 30)]
    skins = [(240, 200, 160), (200, 150, 110), (150, 100, 70), (110, 70, 45)]
    for row in range(n // 32):
        y = row * 32
        d.rectangle((0, y + 22, n, y + 31), fill=(110, 112, 125))    # step
        d.rectangle((0, y + 22, n, y + 23), fill=(150, 152, 165))
        x = 0
        while x < n:
            w = rng.randint(9, 13)
            if rng.random() < 0.88:
                c = rng.choice(shirts); s = rng.choice(skins)
                d.rectangle((x + 1, y + 11, x + w - 2, y + 23), fill=c)
                d.ellipse((x + w / 2 - 3.5, y + 3, x + w / 2 + 3.5, y + 11), fill=s)
                if rng.random() < 0.25: d.rectangle((x + 1, y + 1, x + 3, y + 10), fill=s)   # a waving arm
            x += w
    return im

def road(n=256):
    """groomed clay track across u (0 and 1 the edges, which the berm below covers); v along it: no paint, no kerbs"""
    a = 0.55 * _noise(n, 10, 41) + 0.45 * _noise(n, 48, 42)
    img = np.array([192, 136, 82], float)[None, None, :] * (0.90 + 0.16 * a)[..., None]
    u = np.linspace(0, 1, n)[None, :]
    streak = _noise(n, 1.5, 43, n)                      # grooming streaks running along the track
    sm = np.repeat(streak.mean(axis=0, keepdims=True), n, axis=0)
    img *= (0.95 + 0.08 * sm)[..., None]
    for c in (0.30, 0.70):                              # worn tyre lines, very soft
        img *= (1 - 0.07 * np.exp(-((u - c) / 0.05) ** 2))[..., None]
    img *= (1 - 0.18 * np.clip(1 - np.minimum(u, 1 - u) / 0.06, 0, 1))[..., None]   # edges darken into the berm
    return _rgb(img).filter(ImageFilter.SMOOTH)

def berm(n=256):
    """the strip under each road, wider than it (outer 12% each side): floor-coloured dirt rising into a darker
    ridge of loose dirt where it meets the clay"""
    u = np.linspace(0, 1, n)[None, :] * np.ones((n, 1)); e = np.minimum(u, 1 - u) / 0.12   # 0 outer edge .. 1 clay edge
    a = 0.6 * _noise(n, 5, 51) + 0.4 * _noise(n, 20, 52)
    floor = np.array([128, 88, 52], float); ridge = np.array([104, 68, 40], float); clay = np.array([170, 118, 70], float)
    t = np.clip(e, 0, 1)[..., None]
    img = floor + (ridge - floor) * np.clip(t * 1.4, 0, 1)
    img = np.where(e[..., None] >= 1, clay, img)
    img *= (0.88 + 0.22 * a)[..., None]
    rng = random.Random(53); im = _rgb(img); d = ImageDraw.Draw(im)
    for _ in range(260):                                # clumps on the ridge
        x = rng.choice((rng.uniform(n * 0.03, n * 0.12), rng.uniform(n * 0.88, n * 0.97))); y = rng.uniform(0, n); r = rng.uniform(0.8, 2.2)
        for oy in (-n, 0, n): d.ellipse((x - r, y + oy - r, x + r, y + oy + r), fill=rng.choice([(88, 58, 34), (140, 100, 62), (116, 78, 46)]))
    return im.filter(ImageFilter.SMOOTH)

def boost_top(n=128):
    """the jump's top: a blue boost strip across v (0 and 1 the lips), yellow double chevrons pointing off both sides"""
    y = np.abs(np.linspace(-1, 1, n))[:, None] * np.ones((1, n))
    img = np.zeros((n, n, 3))
    img[..., 0] = 20 + 40 * (1 - y); img[..., 1] = 90 + 110 * (1 - y); img[..., 2] = 210 + 45 * (1 - y)
    im = _rgb(img); d = ImageDraw.Draw(im)
    for x0 in range(0, n, 64):                # diamonds: one chevron toward each lip
        c = x0 + 32
        d.polygon([(c - 22, n * 0.5), (c, n * 0.20), (c + 22, n * 0.5), (c, n * 0.80)], fill=(255, 236, 60))
        d.polygon([(c - 10, n * 0.5), (c, n * 0.36), (c + 10, n * 0.5), (c, n * 0.64)], fill=(40, 120, 230))
    d.rectangle((0, 0, n, 5), fill=(235, 240, 255)); d.rectangle((0, n - 6, n, n), fill=(235, 240, 255))   # plain light lips
    return im

def concrete(n=128):
    a = 0.6 * _noise(n, 6, 11) + 0.4 * _noise(n, 30, 12)
    img = (np.array([120, 120, 124], float) + 60 * (a[..., None] - 0.5))
    return _rgb(img)

def roof(n=128):
    im = concrete(n); d = ImageDraw.Draw(im)
    for x in range(0, n, 32): d.rectangle((x, 0, x + 3, n), fill=(80, 82, 90))
    return im

def light(n=64):
    im = Image.new('RGB', (n, n), (40, 40, 48)); d = ImageDraw.Draw(im)
    for i in range(4):
        for j in range(2):
            x, y = 4 + i * 15, 6 + j * 28
            d.rectangle((x, y, x + 11, y + 22), fill=(255, 250, 215)); d.rectangle((x + 2, y + 2, x + 9, y + 20), fill=(255, 255, 245))
    return im

def startgrid(w=64, h=64):
    im = Image.new('RGB', (w, h)); d = ImageDraw.Draw(im)
    s = 16
    for i in range(w // s):
        for j in range(h // s): d.rectangle((i * s, j * s, i * s + s - 1, j * s + s - 1), fill=(250, 250, 250) if (i + j) % 2 else (25, 25, 25))
    return im

def sky(w=256, h=128):
    """top (v=1) deep blue to a pale horizon (v=0), soft clouds"""
    t = np.linspace(1, 0, h)[:, None] * np.ones((1, w))
    top = np.array([52, 110, 205], float); hor = np.array([190, 220, 245], float)
    img = hor + (top - hor) * (t ** 0.7)[..., None]
    c = _noise(w, 6, 21, h) * 0.6 + _noise(w, 18, 22, h) * 0.4
    cl = np.clip((c - 0.55) * 3.2, 0, 1) * np.clip((1 - t) * 3.0, 0, 1) * np.clip(t * 4.0, 0, 1)
    img = img + (255 - img) * cl[..., None] * 0.85
    return _rgb(img)

TEX = dict(t6_dirt=(dirt, 'CMPR'), t6_ramp=(ramp, 'CMPR'), t6_rampside=(ramp_side, 'CMPR'), t6_lip=(lip, 'CMPR'),
           t6_boost=(boost, 'CMPR'), t6_barrier=(barrier, 'CMPR'), t6_crowd=(crowd, 'CMPR'), t6_concrete=(concrete, 'CMPR'),
           t6_roof=(roof, 'CMPR'), t6_road=(road, 'CMPR'), t6_berm=(berm, 'CMPR'), t6_boosttop=(boost_top, 'CMPR'), t6_light=(light, 'CMPR'), t6_grid=(startgrid, 'CMPR'), t6_sky=(sky, 'CMPR'))

def make(d):
    os.makedirs(d, exist_ok=True)
    out = {}
    for name, (fn, fmt) in TEX.items():
        fn().save(os.path.join(d, name + '.png'))
        out[name] = fmt
    return out

if __name__ == '__main__':
    import sys
    print(make(sys.argv[1] if len(sys.argv) > 1 else 'a6'))
