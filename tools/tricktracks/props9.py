"""Low-poly scenery for the Trick Style tracks (2026-11-12 unique tracks): trees, rocks, cacti, beach and harbour
pieces, buildings. Visual geometry on the plain t7_white texture, coloured per vertex (build7's Mesh.poly shades by
the face's normal). build(mesh, kind, x, y, z, size, k, theme) adds one and returns (half size, height) of its solid
part for build7.solid() (the trunk a kart bumps into), or None (drive through it: bushes, umbrellas)."""
import math, random

A0 = 0.31                                        # build7.solid()'s square is turned by this: boxy props match it
W = 't7_white'


def _c(c, k=1.0): return tuple(max(0.0, min(1.0, v / 255.0 * k)) for v in c)


def ring(x, y, z, r, n, rot=0.0, sx=1.0, sz=1.0):
    return [(x + r * sx * math.cos(rot + 2 * math.pi * i / n), y, z + r * sz * math.sin(rot + 2 * math.pi * i / n)) for i in range(n)]


def prism(m, x, z, y0, y1, r0, r1, n, col, rot=0.0, cap=True, mat=W, lean=(0.0, 0.0)):
    a = ring(x, y0, z, r0, n, rot); b = ring(x + lean[0], y1, z + lean[1], r1, n, rot)
    for i in range(n):
        j = (i + 1) % n
        m(mat).poly([a[i], a[j], b[j], b[i]], [(i / n, 0), ((i + 1) / n, 0), ((i + 1) / n, 1), (i / n, 1)], _c(col))
    if cap and r1 > 1:
        c = (x + lean[0], y1, z + lean[1])
        for i in range(n): m(mat).poly([c, b[i], b[(i + 1) % n]], [(0.5, 0.5), (0, 0), (1, 0)], _c(col, 0.92))


def cone(m, x, z, y0, y1, r, n, col, rot=0.0, tip=None, mat=W):
    a = ring(x, y0, z, r, n, rot); t = (x, y1, z)
    for i in range(n):
        j = (i + 1) % n
        m(mat).poly([a[i], a[j], t], [(0, 0), (1, 0), (0.5, 1)], _c(col) if tip is None else _c(col))
        if tip is not None:                      # a lighter band near the tip (snow, highlights)
            p, q = [(a[k][0] * 0.35 + t[0] * 0.65, a[k][1] * 0.35 + t[1] * 0.65, a[k][2] * 0.35 + t[2] * 0.65) for k in (i, j)]
            m(mat).poly([p, q, (t[0], t[1] + 2, t[2])], [(0, 0), (1, 0), (0.5, 1)], _c(tip))
    for i in range(n): m(mat).poly([(x, y0, z), a[(i + 1) % n], a[i]], [(0.5, 0.5), (1, 0), (0, 0)], _c(col, 0.7))


def blob(m, x, y, z, rx, ry, rz, col, seed, jit=0.18, mat=W, nlat=4, nlon=7):
    rr = random.Random(seed)
    P = {}
    for i in range(nlat + 1):
        th = math.pi * i / nlat
        for j in range(nlon):
            ph = 2 * math.pi * j / nlon + (0.4 if i % 2 else 0)
            k = 1 + rr.uniform(-jit, jit) if 0 < i < nlat else 1
            P[(i, j)] = (x + rx * k * math.sin(th) * math.cos(ph), y + ry * k * math.cos(th), z + rz * k * math.sin(th) * math.sin(ph))
    for i in range(nlat):
        for j in range(nlon):
            q = [P[(i, j)], P[(i, (j + 1) % nlon)], P[(i + 1, (j + 1) % nlon)], P[(i + 1, j)]]
            sh = 0.85 + 0.15 * math.cos(math.pi * i / nlat)
            m(mat).poly(q, [(0, 0), (1, 0), (1, 1), (0, 1)], _c(col, sh))


def box(m, x, z, y0, sx, sz, h, col, ang=A0, mat=W, roof=None, uvs=None):
    ca, sa = math.cos(ang), math.sin(ang)
    P = lambda u, v: (x + u * ca - v * sa, z + u * sa + v * ca)
    q = [P(-sx, -sz), P(sx, -sz), P(sx, sz), P(-sx, sz)]
    for i in range(4):
        a, b = q[i], q[(i + 1) % 4]; L = math.hypot(b[0] - a[0], b[1] - a[1])
        uv = uvs(L, h) if uvs else [(0, 0), (1, 0), (1, 1), (0, 1)]
        m(mat).poly([(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y0 + h, b[1]), (a[0], y0 + h, a[1])], uv, _c(col))
    m(roof[0] if roof else mat).poly([(p[0], y0 + h, p[1]) for p in q], [(0, 0), (1, 0), (1, 1), (0, 1)], _c(roof[1] if roof else col, 0.85))
    return q


def gable(m, x, z, y0, sx, sz, h, rh, wall, rf, ang=A0):
    q = box(m, x, z, y0, sx, sz, h, wall, ang)
    ca, sa = math.cos(ang), math.sin(ang)
    P = lambda u, v, y: (x + u * ca - v * sa, y, z + u * sa + v * ca)
    e = 1.15
    a, b, c, d = P(-sx * e, -sz * e, y0 + h), P(sx * e, -sz * e, y0 + h), P(sx * e, sz * e, y0 + h), P(-sx * e, sz * e, y0 + h)
    r0, r1 = P(-sx * e, 0, y0 + h + rh), P(sx * e, 0, y0 + h + rh)
    m(W).poly([a, b, r1, r0], [(0, 0), (1, 0), (1, 1), (0, 1)], _c(rf)); m(W).poly([d, c, r1, r0], [(0, 0), (1, 0), (1, 1), (0, 1)], _c(rf))
    m(W).poly([a, d, r0], [(0, 0), (1, 0), (0.5, 1)], _c(wall, 0.9)); m(W).poly([b, c, r1], [(0, 0), (1, 0), (0.5, 1)], _c(wall, 0.9))


def build(mesh, kind, x, y, z, s, k, th):
    rr = random.Random(k * 7919 + 13)
    m = mesh
    snow = th.get('snow')
    if kind == 'pine':                           # s: height
        prism(m, x, z, y - 40, y + s * 0.3, s * 0.05, s * 0.035, 5, (96, 64, 40))
        g = th.get('pinecol', (40, 110, 60)); tip = (240, 246, 255) if snow else None
        for i, (a, b, r) in enumerate(((0.18, 0.62, 0.32), (0.42, 0.84, 0.25), (0.64, 1.0, 0.17))):
            cone(m, x, z, y + s * a, y + s * b, s * r, 7, tuple(v * (0.9 + 0.08 * i) for v in g), rr.uniform(0, 6), tip)
        return (s * 0.05, 900)
    if kind == 'palm':
        lx, lz = rr.uniform(-1, 1) * s * 0.18, rr.uniform(-1, 1) * s * 0.18; segs = 5; px, py, pz = x, y - 40, z
        for i in range(segs):
            t0, t1 = i / segs, (i + 1) / segs
            nx_, ny_, nz_ = x + lx * t1 * t1, y + s * t1, z + lz * t1 * t1
            prism(m, px, pz, py, ny_, s * 0.045, s * 0.04, 5, (170, 130, 86) if i % 2 else (150, 112, 72), cap=False, lean=(nx_ - px, nz_ - pz))
            px, py, pz = nx_, ny_, nz_
        for f in range(8):
            a = 2 * math.pi * f / 8 + rr.uniform(-0.2, 0.2); L = s * rr.uniform(0.42, 0.55); ca, sa = math.cos(a), math.sin(a)
            mid = (px + ca * L * 0.55, py + s * 0.06, pz + sa * L * 0.55); end = (px + ca * L, py - s * 0.18, pz + sa * L)
            w = s * 0.09; ox, oz = -sa * w, ca * w; gc = (60, 150, 60) if f % 2 else (46, 128, 50)
            m(W).poly([(px, py, pz), (mid[0] + ox, mid[1], mid[2] + oz), end], [(0, 0), (1, 0), (1, 1)], _c(gc))
            m(W).poly([(px, py, pz), end, (mid[0] - ox, mid[1], mid[2] - oz)], [(0, 0), (1, 1), (0, 1)], _c(gc, 0.9))
        blob(m, px, py - s * 0.03, pz, s * 0.05, s * 0.05, s * 0.05, (110, 80, 40), k, nlat=3, nlon=5)
        return (s * 0.045, 900)
    if kind == 'shrub':
        col = th.get('shrubcol', (60, 130, 56))
        for i in range(3):
            blob(m, x + rr.uniform(-s, s) * 0.5, y + s * 0.35, z + rr.uniform(-s, s) * 0.5, s * rr.uniform(0.5, 0.8), s * 0.45, s * rr.uniform(0.5, 0.8), col, k * 3 + i)
        return None
    if kind == 'rock':
        col = th.get('rockcol', (128, 112, 100))
        blob(m, x, y + s * 0.25, z, s, s * rr.uniform(0.55, 0.85), s * rr.uniform(0.7, 1.0), col, k, jit=0.3)
        return (s * 0.6, min(900, s * 0.8))
    if kind == 'cactus':
        g = (66, 140, 70)
        prism(m, x, z, y - 40, y + s, s * 0.08, s * 0.07, 6, g)
        for side in (-1, 1):
            if rr.random() < 0.8:
                a = rr.uniform(0, math.pi); ox, oz = math.cos(a) * side, math.sin(a) * side; h0 = s * rr.uniform(0.35, 0.55)
                prism(m, x + ox * s * 0.06, z + oz * s * 0.06, y + h0, y + h0 + s * 0.08, s * 0.05, s * 0.05, 5, g, cap=False, lean=(ox * s * 0.2, oz * s * 0.2))
                prism(m, x + ox * s * 0.26, z + oz * s * 0.26, y + h0 + s * 0.06, y + h0 + s * 0.38, s * 0.05, s * 0.045, 5, g)
        return (s * 0.08, 900)
    if kind == 'hoodoo':                         # a red rock pillar in bands with a cap stone
        col = th.get('rockcol', (190, 96, 60)); hh = 0
        for i in range(4):
            h1 = s * (i + 1) / 4; r = s * (0.16 - 0.025 * i) * rr.uniform(0.9, 1.1)
            prism(m, x, z, y + hh - 40, y + h1, r * 1.1, r * 0.85, 7, tuple(v * (0.85 + 0.1 * (i % 2)) for v in col), rr.uniform(0, 6), cap=False)
            hh = h1
        blob(m, x, y + s, z, s * 0.2, s * 0.07, s * 0.2, tuple(v * 0.7 for v in col), k)
        return (s * 0.15, 900)
    if kind == 'crystal':                        # ice / glass spires
        for i in range(3):
            ox, oz = rr.uniform(-1, 1) * s * 0.2, rr.uniform(-1, 1) * s * 0.2; h = s * rr.uniform(0.5, 1.0)
            cone(m, x + ox, z + oz, y - 40, y + h, s * 0.12, 5, th.get('crystalcol', (150, 210, 245)), rr.uniform(0, 6), (235, 250, 255))
        return (s * 0.2, 900)
    if kind == 'snowman':
        for i, (r, h) in enumerate(((0.36, 0.3), (0.26, 0.78), (0.18, 1.12))):
            blob(m, x, y + s * h, z, s * r, s * r, s * r, (248, 250, 255), k * 5 + i, jit=0.04)
        a = rr.uniform(0, 6); ca, sa = math.cos(a), math.sin(a)
        nose = (x + ca * s * 0.17, y + s * 1.12, z + sa * s * 0.17)
        m(W).poly([nose, (x + ca * s * 0.45, y + s * 1.12, z + sa * s * 0.45), (nose[0] - sa * 25, nose[1] + 40, nose[2] + ca * 25)], [(0, 0), (1, 0), (0, 1)], _c((240, 120, 30)))
        prism(m, x, z, y + s * 1.26, y + s * 1.5, s * 0.15, s * 0.15, 6, (30, 30, 36))
        prism(m, x, z, y + s * 1.25, y + s * 1.28, s * 0.25, s * 0.25, 6, (30, 30, 36))
        return (s * 0.3, 700)
    if kind == 'umbrella':
        prism(m, x, z, y - 20, y + s, 22, 22, 4, (240, 240, 240), cap=False)
        cols = ((230, 60, 60), (250, 250, 250)) if k % 2 else ((40, 120, 220), (250, 220, 60))
        a = ring(x, y + s * 0.82, z, s * 0.55, 8, rr.uniform(0, 6)); t = (x, y + s * 1.02, z)
        for i in range(8): m(W).poly([a[i], a[(i + 1) % 8], t], [(0, 0), (1, 0), (0.5, 1)], _c(cols[i % 2]))
        box(m, x + s * 0.5, z + s * 0.2, y, s * 0.32, s * 0.16, 8, (240, 230, 200), ang=rr.uniform(0, 3))     # a towel
        return None
    if kind == 'lifeguard':                      # a stilt hut, red and white
        for u, v in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            prism(m, x + u * s * 0.3, z + v * s * 0.3, y - 30, y + s * 0.6, 26, 26, 4, (240, 240, 240), cap=False)
        box(m, x, z, y + s * 0.6, s * 0.38, s * 0.38, s * 0.35, (220, 40, 40))
        gable(m, x, z, y + s * 0.95, s * 0.38, s * 0.38, 1, s * 0.2, (230, 230, 230), (240, 240, 240))
        return (s * 0.36, 900)
    if kind == 'lighthouse':
        for i in range(6):
            r0, r1 = s * (0.14 - 0.012 * i), s * (0.14 - 0.012 * (i + 1))
            prism(m, x, z, y + s * i / 7 - (40 if i == 0 else 0), y + s * (i + 1) / 7, r0, r1, 10, (240, 240, 240) if i % 2 else (210, 40, 40), cap=False)
        prism(m, x, z, y + s * 6 / 7, y + s * 0.98, s * 0.06, s * 0.06, 8, (255, 240, 160), cap=False)
        cone(m, x, z, y + s * 0.98, y + s * 1.1, s * 0.08, 8, (200, 40, 40))
        return (s * 0.14, 900)
    if kind == 'crane':                          # a harbour gantry crane
        c = th.get('cranecol', (240, 180, 30))
        for u in (-1, 1):
            for v in (-1, 1): prism(m, x + u * s * 0.18, z + v * s * 0.12, y - 30, y + s * 0.8, s * 0.025, s * 0.025, 4, c, cap=False)
        box(m, x, z, y + s * 0.8, s * 0.22, s * 0.16, s * 0.06, c, ang=0)
        box(m, x + s * 0.42, z, y + s * 0.86, s * 0.65, s * 0.03, s * 0.04, c, ang=0)
        box(m, x, z, y + s * 0.86, s * 0.08, s * 0.08, s * 0.1, (60, 60, 70), ang=0)
        return None
    if kind == 'lamp':
        prism(m, x, z, y - 20, y + s, 30, 24, 5, (70, 70, 80), cap=False)
        box(m, x, z, y + s, 120, 120, 90, (255, 245, 200), mat='t7_light')
        return (60, 900)
    if kind == 'neon':                           # a glowing sign on a pole (neon nights)
        col = rr.choice(((255, 60, 200), (40, 240, 255), (255, 230, 40), (120, 255, 80)))
        prism(m, x, z, y - 20, y + s * 0.6, 40, 40, 5, (40, 40, 50), cap=False)
        box(m, x, z, y + s * 0.6, s * 0.35, 40, s * 0.4, col, ang=rr.uniform(0, 3), mat='t7_light')
        return (60, 900)
    if kind == 'chalet':
        gable(m, x, z, y - 20, s * 0.5, s * 0.36, s * 0.45, s * 0.35, (150, 100, 60), (150, 40, 36))
        return (s * 0.5, 900)
    if kind == 'bale':
        a = rr.uniform(0, 3); ca, sa = math.cos(a), math.sin(a)
        for i in range(8):
            t0, t1 = 2 * math.pi * i / 8, 2 * math.pi * (i + 1) / 8
            P = lambda t, w: (x + ca * w - sa * math.cos(t) * s * 0.5 * 0, y + s * 0.5 + s * 0.5 * math.sin(t), z + sa * w)
            q = [(x + ca * w + (-sa) * s * 0.5 * math.cos(t), y + s * 0.5 + s * 0.5 * math.sin(t), z + sa * w + ca * s * 0.5 * math.cos(t)) for t, w in ((t0, -s * 0.5), (t1, -s * 0.5), (t1, s * 0.5), (t0, s * 0.5))]
            m(W).poly(q, [(0, 0), (1, 0), (1, 1), (0, 1)], _c((222, 190, 100)))
        return (s * 0.5, 600)
    if kind == 'crate':
        box(m, x, z, y - 20, s * 0.5, s * 0.5, s, (170, 120, 70))
        return (s * 0.5, min(900, s))
    if kind == 'tank':                           # a factory tank, banded
        prism(m, x, z, y - 30, y + s, s * 0.45, s * 0.45, 10, (180, 184, 190))
        prism(m, x, z, y + s * 0.4, y + s * 0.48, s * 0.46, s * 0.46, 10, (220, 120, 30), cap=False)
        return (s * 0.4, 900)
    if kind == 'stack':                          # a smokestack
        prism(m, x, z, y - 30, y + s, s * 0.08, s * 0.06, 8, (150, 70, 56))
        for f in (0.7, 0.9): prism(m, x, z, y + s * f, y + s * f + s * 0.03, s * 0.065, s * 0.065, 8, (240, 240, 240), cap=False)
        return (s * 0.08, 900)
    if kind == 'building':
        h = s * rr.uniform(0.8, 1.6)
        box(m, x, z, y - 20, s * 0.3, s * 0.3, h, (230, 230, 240), mat='t7_windows', roof=(W, (90, 90, 100)), uvs=lambda L, hh: [(0, 0), (L / 1500, 0), (L / 1500, hh / 1500), (0, hh / 1500)])
        return (s * 0.3, 900)
    if kind == 'boat':                           # a moored boat (on water: no trunk)
        a = rr.uniform(0, 6); ca, sa = math.cos(a), math.sin(a)
        P = lambda u, v, h: (x + u * ca - v * sa, y + h, z + u * sa + v * ca)
        L, Wd = s, s * 0.32; hc = rr.choice(((240, 240, 240), (200, 50, 40), (40, 90, 170)))
        pts = [(-L / 2, -Wd / 2), (L / 3, -Wd / 2), (L / 2, 0), (L / 3, Wd / 2), (-L / 2, Wd / 2)]
        for i in range(5):
            (u0, v0), (u1, v1) = pts[i], pts[(i + 1) % 5]
            m(W).poly([P(u0 * 0.9, v0 * 0.8, 0), P(u1 * 0.9, v1 * 0.8, 0), P(u1, v1, s * 0.16), P(u0, v0, s * 0.16)], [(0, 0), (1, 0), (1, 1), (0, 1)], _c(hc))
        c = (sum(P(u, v, 0)[0] for u, v in pts) / 5, sum(P(u, v, 0)[2] for u, v in pts) / 5)
        for i in range(5):
            (u0, v0), (u1, v1) = pts[i], pts[(i + 1) % 5]
            m(W).poly([(c[0], y + s * 0.16, c[1]), P(u0, v0, s * 0.16), P(u1, v1, s * 0.16)], [(0, 0), (1, 0), (0, 1)], _c((200, 170, 120)))
        box(m, x - ca * L * 0.1, z - sa * L * 0.1, y + s * 0.16, L * 0.18, Wd * 0.3, s * 0.18, (245, 245, 245), ang=a)
        return None
    if kind == 'buoy':
        cone(m, x, z, y - 40, y + s, s * 0.35, 6, (230, 60, 40))
        return None
    if kind == 'flag':                           # a pole with a pennant in the theme's colour
        prism(m, x, z, y - 20, y + s, 22, 18, 4, (230, 230, 230), cap=False)
        c = th.get('bar', ((240, 240, 240), (220, 40, 40)))[1]
        m(W).poly([(x, y + s, z), (x + s * 0.3, y + s * 0.9, z + 10), (x, y + s * 0.8, z)], [(0, 0), (1, 0.5), (0, 1)], _c(c))
        return None
    if kind == 'pillar':                         # a scaffold / bridge pier from the ground (a canyon floor) up s
        c = th.get('pillarcol', (120, 90, 60))
        prism(m, x, z, y - 40, y + s, 170, 150, 4, c, rot=A0)
        return (160, 900)
    if kind == 'vent':                           # a geyser's mouth: a ring of rocks round a dark pool
        rc = th.get('rockcol', (120, 110, 100))
        for i in range(9):
            a = 2 * math.pi * i / 9
            blob(m, x + math.cos(a) * s, y + 60, z + math.sin(a) * s, s * 0.28, s * 0.22, s * 0.28, rc, k * 11 + i, nlat=3, nlon=5)
        return None
    if kind == 'pyramid':                        # a big landmark cone (volcano peak, a mountain): visual only
        cone(m, x, z, y - 200, y + s, s * 1.4, 9, th.get('rockcol', (110, 100, 96)), rr.uniform(0, 6), th.get('peakcol'))
        return None
    raise ValueError('prop kind ' + kind)
