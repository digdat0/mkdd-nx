"""Themed textures for the Trick Style tracks (build7.py). All drawn here from noise and shapes, nothing taken from
the game. make(dir, theme) writes t7_*.png and returns {name: format}."""
import os, math, random
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from tex6 import _noise, _rgb, _font

def ground(th, n=256):
    a = 0.55 * _noise(n, 3, 1) + 0.30 * _noise(n, 12, 2) + 0.15 * _noise(n, 40, 3)
    base = np.array(th['ground'][0], float); dark = np.array(th['ground'][1], float)
    img = dark + (base - dark) * a[..., None]
    rng = random.Random(4); im = _rgb(img); d = ImageDraw.Draw(im)
    for _ in range(th.get('pebbles', 170)):
        x, y, r = rng.uniform(0, n), rng.uniform(0, n), rng.uniform(0.8, 2.4)
        c = tuple(int(v) for v in rng.choice(th['pebble']))
        for ox in (-n, 0, n):
            for oy in (-n, 0, n): d.ellipse((x + ox - r, y + oy - r, x + ox + r, y + oy + r), fill=c)
    if th.get('grass'):                       # blades: short strokes
        for _ in range(1400):
            x, y = rng.uniform(0, n), rng.uniform(0, n); c = tuple(int(v) for v in rng.choice(th['grass']))
            d.line((x, y, x + rng.uniform(-1.5, 1.5), y - rng.uniform(2, 5)), fill=c, width=1)
    return im.filter(ImageFilter.SMOOTH)

def road(th, n=256):
    """the track surface across u (0/1 the edges); asphalt gets dashed centre and edge lines"""
    kind = th['road']
    a = 0.55 * _noise(n, 10, 41) + 0.45 * _noise(n, 48, 42)
    img = np.array(th['roadcol'], float)[None, None, :] * (0.90 + 0.16 * a)[..., None]
    u = np.linspace(0, 1, n)[None, :]
    if kind == 'asphalt':
        g = _noise(n, 1.2, 44)
        img *= (0.92 + 0.12 * g)[..., None]
        im = _rgb(img); d = ImageDraw.Draw(im)
        d.rectangle((int(n * 0.04), 0, int(n * 0.06), n), fill=(235, 235, 225)); d.rectangle((int(n * 0.94), 0, int(n * 0.96), n), fill=(235, 235, 225))
        for y in range(0, n, 64): d.rectangle((int(n * 0.49), y, int(n * 0.51), y + 32), fill=(240, 200, 40))
        return im
    streak = _noise(n, 1.5, 43, n)
    sm = np.repeat(streak.mean(axis=0, keepdims=True), n, axis=0)
    img *= (0.95 + 0.08 * sm)[..., None]
    for c in (0.30, 0.70):
        img *= (1 - 0.07 * np.exp(-((u - c) / 0.05) ** 2))[..., None]
    img *= (1 - 0.18 * np.clip(1 - np.minimum(u, 1 - u) / 0.06, 0, 1))[..., None]
    return _rgb(img).filter(ImageFilter.SMOOTH)

def berm(th, n=256):
    u = np.linspace(0, 1, n)[None, :] * np.ones((n, 1)); e = np.minimum(u, 1 - u) / 0.12
    a = 0.6 * _noise(n, 5, 51) + 0.4 * _noise(n, 20, 52)
    floor = np.array(th['ground'][1], float) * 0.5 + np.array(th['ground'][0], float) * 0.5
    ridge = np.array(th['berm'], float); clay = np.array(th['roadcol'], float)
    t = np.clip(e, 0, 1)[..., None]
    img = floor + (ridge - floor) * np.clip(t * 1.4, 0, 1)
    img = np.where(e[..., None] >= 1, clay, img)
    img *= (0.88 + 0.22 * a)[..., None]
    return _rgb(img).filter(ImageFilter.SMOOTH)

def mound(th, n=256):
    """packed jump dirt (landings, tables, rollers): streaks along v (the travel direction)"""
    a = 0.5 * _noise(n, 6, 61) + 0.5 * _noise(n, 30, 62)
    img = np.array(th['mound'], float)[None, None, :] * (0.86 + 0.22 * a)[..., None]
    st = _noise(n, 1.2, 63, n); st = np.repeat(st.mean(axis=0, keepdims=True), n, axis=0)
    img *= (0.94 + 0.1 * st)[..., None]
    if th.get('snow'):
        img = img * 0.6 + 255 * 0.4 * (0.9 + 0.1 * a)[..., None]
    return _rgb(img).filter(ImageFilter.SMOOTH)

def mound_side(th, n=128):
    a = 0.6 * _noise(n, 4, 64) + 0.4 * _noise(n, 18, 65)
    img = np.array(th['mound'], float)[None, None, :] * (0.62 + 0.25 * a)[..., None]
    return _rgb(img)

def kick(th, n=128):
    """kicker faces: wood boards (orange), steel plate (grey, diamond tread) or ice (blue)"""
    k = th['kick']
    if k == 'steel':
        g = _noise(n, 8, 66)
        img = np.zeros((n, n, 3)); img[...] = th.get('kickcol', (150, 156, 166))
        img *= (0.85 + 0.2 * g)[..., None]
        im = _rgb(img); d = ImageDraw.Draw(im)
        for y in range(0, n, 12):
            for x in range((y // 12 % 2) * 6, n, 12): d.line((x, y + 2, x + 5, y + 7), fill=(200, 205, 215), width=2)
        for y in range(0, n, 64): d.rectangle((0, y, n, y + 2), fill=(90, 92, 100))
        return im
    base = th.get('kickcol', (236, 118, 22) if k == 'wood' else (120, 200, 245))
    g = _noise(n, 20, 5, n) * 0.5 + _noise(n, 4, 6, n) * 0.5
    img = np.zeros((n, n, 3)); img[...] = base
    img *= (0.82 + 0.25 * g)[..., None]
    if k == 'ice': img = img * 0.7 + 255 * 0.3
    for y in range(0, n, 16):
        img[y:y + 2] *= 0.6; img[y + 2] *= 1.1
    return _rgb(img)

def kick_side(th, n=128):
    base = np.array(th.get('kickcol', (236, 118, 22) if th['kick'] == 'wood' else (150, 156, 166) if th['kick'] == 'steel' else (120, 200, 245)), float)
    img = np.zeros((n, n, 3)); img[...] = base * 0.62
    img *= (0.85 + 0.2 * _noise(n, 10, 7))[..., None]
    im = _rgb(img); d = ImageDraw.Draw(im)
    lc = tuple(int(v) for v in np.clip(base * 0.9, 0, 255))
    d.line((0, 0, n, n), fill=lc, width=9); d.line((0, n, n, 0), fill=lc, width=9)
    d.rectangle((0, 0, n - 1, n - 1), outline=tuple(int(v) for v in base * 0.4), width=6)
    return im

def boost(th, n=128):
    y = np.linspace(0, 1, n)[:, None] * np.ones((1, n))
    img = np.zeros((n, n, 3))
    img[..., 0] = 20 + 30 * y; img[..., 1] = 80 + 90 * y; img[..., 2] = 200 + 55 * y
    im = _rgb(img); d = ImageDraw.Draw(im)
    for k in range(2):
        y0 = k * n // 2 + n // 8
        pts = [(n * 0.12, y0 + n * 0.30), (n * 0.5, y0), (n * 0.88, y0 + n * 0.30), (n * 0.88, y0 + n * 0.42), (n * 0.5, y0 + n * 0.14), (n * 0.12, y0 + n * 0.42)]
        d.polygon(pts, fill=(255, 236, 60))
    d.rectangle((0, 0, 5, n), fill=(235, 240, 255)); d.rectangle((n - 6, 0, n, n), fill=(235, 240, 255))
    return im   # chevrons toward the image top: SuperBMD makes the DAE's v = 1 (the pad's far end) texture row 0

def barrier(th, w=256, h=64):
    """track side: blocks in the theme's two colours with a board in the middle"""
    c1, c2 = th['bar']
    im = Image.new('RGB', (w, h), c1); d = ImageDraw.Draw(im)
    for i in range(0, w, 32):
        if (i // 32) % 2: d.rectangle((i, 0, i + 31, h), fill=c2)
    if th.get('bartext'):
        d.rectangle((48, 10, w - 49, h - 11), fill=(20, 24, 60))
        f = _font(28); t = th['bartext']
        bb = d.textbbox((0, 0), t, font=f); tw, th_ = bb[2] - bb[0], bb[3] - bb[1]
        d.text(((w - tw) // 2 - bb[0], (h - th_) // 2 - bb[1]), t, font=f, fill=(255, 200, 30))
    d.rectangle((0, 0, w, 3), fill=(50, 50, 50)); d.rectangle((0, h - 4, w, h), fill=(50, 50, 50))
    return im

def fence(th, w=128, h=64):
    """low rustic fence (open tracks): posts and two rails over the ground colour"""
    im = Image.new('RGB', (w, h), tuple(int(v) for v in th['ground'][1])); d = ImageDraw.Draw(im)
    wood = th.get('fencecol', (120, 84, 50))
    for x in range(0, w, 64): d.rectangle((x + 2, 0, x + 12, h), fill=wood)
    d.rectangle((0, 10, w, 20), fill=wood); d.rectangle((0, 36, w, 46), fill=wood)
    return im

def crowd(th, n=256):
    from tex6 import crowd as c6
    return c6(n)

def concrete(th, n=128):
    a = 0.6 * _noise(n, 6, 11) + 0.4 * _noise(n, 30, 12)
    return _rgb(np.array(th.get('concrete', (120, 120, 124)), float) + 60 * (a[..., None] - 0.5))

def roof(th, n=128):
    im = concrete(th, n); d = ImageDraw.Draw(im)
    for x in range(0, n, 32): d.rectangle((x, 0, x + 3, n), fill=(80, 82, 90))
    return im

def light(th, n=64):
    from tex6 import light as l6
    return l6(n)

def sky(th, w=512, h=128):
    """dome: v=0 the horizon .. v=1 the top; silhouettes (mesas, hills, mountains, skyline) on the horizon band"""
    t = np.linspace(1, 0, h)[:, None] * np.ones((1, w))
    top = np.array(th['sky'][0], float); hor = np.array(th['sky'][1], float)
    img = hor + (top - hor) * (t ** 0.7)[..., None]
    if th.get('stars'):
        rng = np.random.default_rng(5); s = rng.random((h, w)) > 0.994
        img[s & (t > 0.3)] = 255
    else:
        c = _noise(w, 8, 21, h) * 0.6 + _noise(w, 24, 22, h) * 0.4
        cl = np.clip((c - 0.55) * 3.2, 0, 1) * np.clip((1 - t) * 3.0, 0, 1) * np.clip(t * 4.0, 0, 1)
        img = img + (np.array(th.get('cloud', (255, 255, 255)), float) - img) * cl[..., None] * 0.8
    im = _rgb(img); d = ImageDraw.Draw(im)
    sil = th.get('sil')
    if sil:
        kind, col = sil; rng = random.Random(77); base = h - 1
        if kind == 'skyline':
            x = 0
            while x < w:
                bw = rng.randint(8, 22); bh = rng.randint(6, 34)
                d.rectangle((x, base - bh, x + bw, base), fill=col)
                for wy in range(base - bh + 3, base - 2, 5):
                    for wx in range(x + 2, x + bw - 2, 4):
                        if rng.random() < 0.35: d.point((wx, wy), fill=(255, 220, 140))
                x += bw + rng.randint(0, 3)
        else:
            amp = {'mesa': 26, 'hills': 18, 'mountains': 40}[kind]
            pts = [(0, base)]
            hn = _noise(w, 3 if kind != 'mountains' else 6, 88, 4)[0]
            for x in range(0, w + 1, 2):
                v = hn[x % w]
                if kind == 'mesa': v = 1.0 if v > 0.62 else 0.25 + 0.2 * v
                elif kind == 'mountains': v = v ** 1.5
                pts.append((x, base - 6 - amp * v))
            pts.append((w, base)); d.polygon(pts, fill=col)
            if kind == 'mountains':           # snow caps
                for x, y in pts[1:-1]:
                    if y < base - 6 - amp * 0.55: d.line((x, y, x, y + 4), fill=(245, 248, 255))
    return im

def windows(th, n=128):
    """building walls: concrete with lit and dark windows"""
    im = concrete(th, n); d = ImageDraw.Draw(im); rng = random.Random(91)
    for y in range(6, n, 16):
        for x in range(4, n, 12):
            d.rectangle((x, y, x + 7, y + 9), fill=(255, 214, 130) if rng.random() < 0.45 else (40, 50, 70))
    return im

def container(th, n=128):
    """shipping container side: vertical ribs, a frame, a white code panel (tinted per box by vertex colour)"""
    img = np.zeros((n, n, 3)); img[...] = (200, 200, 200)
    img *= (0.9 + 0.12 * _noise(n, 8, 92))[..., None]
    for x in range(0, n, 8): img[:, x:x + 2] *= 0.72; img[:, x + 2:x + 3] *= 1.12
    im = _rgb(img); d = ImageDraw.Draw(im)
    d.rectangle((0, 0, n - 1, n - 1), outline=(120, 120, 120), width=4)
    d.rectangle((10, 12, 46, 24), fill=(250, 250, 250))
    return im

# ---- 2026-11-12 unique tracks: surfaces (zones), rails, spring pads, the props' plain base ----
def _tone(col, n, f1, f2, s1, s2, k=0.25):
    a = 0.6 * _noise(n, f1, s1) + 0.4 * _noise(n, f2, s2)
    return np.array(col, float)[None, None, :] * ((1 - k) + 2 * k * a)[..., None]
def sand(th, n=128):
    img = _tone(th.get('sandcol', (232, 206, 150)), n, 6, 30, 201, 202, 0.12)
    rip = np.sin(np.linspace(0, 12 * math.pi, n))[:, None] * np.ones((1, n)) * (0.5 + 0.5 * _noise(n, 3, 203))
    return _rgb(img * (1 + 0.05 * rip)[..., None])
def ice(th, n=128):
    img = _tone((176, 220, 246), n, 3, 20, 211, 212, 0.12)
    im = _rgb(img); d = ImageDraw.Draw(im); rng = random.Random(213)
    for _ in range(14):
        x, y = rng.uniform(0, n), rng.uniform(0, n); a = rng.uniform(0, math.pi)
        L = rng.uniform(10, 40); d.line((x, y, x + L * math.cos(a), y + L * math.sin(a)), fill=(236, 248, 255), width=1)
    return im
def snow(th, n=128): return _rgb(_tone((242, 246, 252), n, 5, 24, 221, 222, 0.06))
def shallow(th, n=128):
    """the bottom of shallow water: wet sand, blue-green"""
    return _rgb(_tone((150, 186, 170), n, 5, 22, 231, 232, 0.14))
def water(th, n=128):
    c = th.get('watercol', (40, 150, 200))
    a = 0.5 * _noise(n, 4, 241) + 0.5 * _noise(n, 16, 242)
    img = np.array(c, float)[None, None, :] * (0.82 + 0.3 * a)[..., None]
    w = np.clip((a - 0.62) * 6, 0, 1)
    img = img + (255 - img) * (w * 0.55)[..., None]
    return _rgb(img)
def lava(th, n=128):
    a = 0.55 * _noise(n, 4, 251) + 0.45 * _noise(n, 14, 252)
    hot = np.array((255, 200, 60), float); cool = np.array((200, 40, 10), float)
    t = np.clip((a - 0.3) * 1.6, 0, 1)[..., None]
    img = cool + (hot - cool) * t
    crust = np.clip((0.42 - a) * 5, 0, 1)[..., None]
    return _rgb(img * (1 - crust) + np.array((50, 24, 20), float) * crust)
def lavabed(th, n=128): return _rgb(_tone((70, 30, 22), n, 4, 20, 261, 262, 0.2))
def deepbed(th, n=128): return _rgb(_tone((22, 60, 100), n, 4, 20, 263, 264, 0.15))
def rock(th, n=128):
    img = _tone(th.get('rockcol', (128, 112, 100)), n, 4, 18, 271, 272, 0.28)
    st = np.sin(np.linspace(0, 9 * math.pi, n))[:, None] * np.ones((1, n))
    return _rgb(img * (1 + 0.06 * st)[..., None])
def grass(th, n=128):
    img = _tone(th.get('grasscol', (92, 160, 64)), n, 5, 26, 281, 282, 0.16)
    im = _rgb(img); d = ImageDraw.Draw(im); rng = random.Random(283)
    for _ in range(500):
        x, y = rng.uniform(0, n), rng.uniform(0, n); c = tuple(int(v * rng.uniform(0.8, 1.2)) for v in th.get('grasscol', (92, 160, 64)))
        d.line((x, y, x + rng.uniform(-1, 1), y - rng.uniform(2, 4)), fill=tuple(min(255, v) for v in c))
    return im
def rail(th, w=128, h=32):
    c = th.get('railcol', (200, 200, 205))
    im = Image.new('RGB', (w, h), tuple(int(v * 0.5) for v in c)); d = ImageDraw.Draw(im)
    d.rectangle((0, 2, w, 8), fill=c); d.rectangle((0, h - 9, w, h - 4), fill=c)
    for x in range(0, w, 32): d.rectangle((x, 0, x + 5, h), fill=tuple(int(v * 0.8) for v in c))
    return im
def spring(th, n=128):
    """spring pads: hazard stripes round a red disc with a white up arrow"""
    im = Image.new('RGB', (n, n), (30, 30, 30)); d = ImageDraw.Draw(im)
    for i in range(-n, 2 * n, 24): d.polygon([(i, 0), (i + 12, 0), (i + 12 - n, n), (i - n, n)], fill=(250, 200, 20))
    d.ellipse((14, 14, n - 15, n - 15), fill=th.get('springcol', (230, 40, 50)), outline=(255, 255, 255), width=4)
    m = n / 2; d.polygon([(m, 26), (m + 30, 62), (m + 12, 62), (m + 12, n - 28), (m - 12, n - 28), (m - 12, 62), (m - 30, 62)], fill=(255, 255, 255))
    return im
def white(th, n=32): return _rgb(np.ones((n, n, 3)) * 255 * (0.94 + 0.06 * _noise(n, 4, 291))[..., None])

TEX = dict(t7_sand=sand, t7_ice=ice, t7_snow=snow, t7_shallow=shallow, t7_water=water, t7_lava=lava, t7_lavabed=lavabed, t7_deepbed=deepbed,
           t7_rock=rock, t7_grass=grass, t7_rail=rail, t7_spring=spring, t7_white=white, t7_container=container, t7_windows=windows, t7_ground=ground, t7_road=road, t7_berm=berm, t7_mound=mound, t7_moundside=mound_side, t7_kick=kick, t7_kickside=kick_side,
           t7_boost=boost, t7_barrier=barrier, t7_fence=fence, t7_crowd=crowd, t7_concrete=concrete, t7_roof=roof, t7_light=light, t7_sky=sky)

def make(d, th):
    os.makedirs(d, exist_ok=True)
    out = {}
    for name, fn in TEX.items():
        fn(th).save(os.path.join(d, name + '.png'))
        out[name] = 'CMPR'
    return out
