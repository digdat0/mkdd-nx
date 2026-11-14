"""2026-11-12 unique tracks: twelve of the sixteen rebuilt as real-world-ish places (imported at the end of tracks7.py,
after tracks8, whose entries they replace; Trick Stadium, Rhythm Ridge SX and Mega Ramp Arena stay as they were, Meadow
Motoplex keeps its layout and gains scenery). Each mixes the jump kit with build7's newer parts:
  zones     sand, shallow water, ice, snow (the game's own surfaces) and hazards (lava, deep water, canyon floors:
            dead zones, Lakitu brings the kart back to the nearest respawn point), each with an optional basin
  terrain   basins, raised ground, a graded road (Alpine: the road climbs and descends; jumps ride its slope)
  features  canyon jumps (two kickers over a gap, ramps on posts where they overhang), bridges and an overpass (thin
            decks with rails), a skyway, spring pads; markers that place tunnels, geysers along the road
  props     trees, rocks, cacti, hoodoos, beach and harbour pieces, buildings (props9.py), with solid trunks
  fx        spring pads, geysers and wandering tornadoes for the runtime (runtime/trickfx.c)"""
import math, random
import tracks7 as t7
from tracks7 import TRACKS, rounded, at, leglen, hill, hills, gap2, spine, table2, rainbow, corner_berms, _face, _boost_segs, containers
from tracks8 import course


def ss(t):
    t = max(0.0, min(1.0, t)); return t * t * (3 - 2 * t)


def _start(L, s=1000): return (at(L, s)[0], at(L, s)[1], L[2])


# ---------------- shapes and zones ----------------
def chaikin(pts, n=2):
    for _ in range(n):
        out = [pts[0]]
        for a, b in zip(pts, pts[1:]):
            out.append(tuple(0.75 * p + 0.25 * q for p, q in zip(a, b))); out.append(tuple(0.25 * p + 0.75 * q for p, q in zip(a, b)))
        out.append(pts[-1]); pts = out
    return pts


def band_q(pts):
    """(x, z) -> distance outside a band of (x, z, half width) points (negative inside)"""
    xs = [p[0] for p in pts]; zs = [p[1] for p in pts]; hm = max(p[2] for p in pts)
    bx0, bx1, bz0, bz1 = min(xs) - hm, max(xs) + hm, min(zs) - hm, max(zs) + hm
    segs = [(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
    def q(x, z):
        if not (bx0 - 6000 < x < bx1 + 6000 and bz0 - 6000 < z < bz1 + 6000): return 1e9
        best = 1e18
        for (ax, az, ah), (bx, bz, bh) in segs:
            vx, vz = bx - ax, bz - az; L2 = vx * vx + vz * vz or 1.0
            t = max(0.0, min(1.0, ((x - ax) * vx + (z - az) * vz) / L2))
            best = min(best, math.hypot(x - ax - vx * t, z - az - vz * t) - (ah + (bh - ah) * t))
        return best
    return q


def band(kind, pts, depth=0.0, level=None, ramp=900.0, smooth=2):
    pts = chaikin([tuple(p) for p in pts], smooth) if smooth else [tuple(p) for p in pts]
    q = band_q(pts)
    Z = dict(kind=kind, band=pts, level=level, q=q)
    Z['in'] = lambda x, z: q(x, z) <= 0
    if depth: Z['dh'] = lambda x, z: -depth * ss(-q(x, z) / ramp)
    return Z


def oval(kind, cx, cz, rx, rz, depth=0.0, level=None, ramp=1500.0):
    m = min(rx, rz)
    q = lambda x, z: (math.hypot((x - cx) / rx, (z - cz) / rz) - 1) * m
    Z = dict(kind=kind, level=level, q=q, poly=[(cx + rx * math.cos(2 * math.pi * i / 32), cz + rz * math.sin(2 * math.pi * i / 32)) for i in range(32)])
    Z['in'] = lambda x, z: q(x, z) <= 0
    if depth: Z['dh'] = lambda x, z: -depth * ss(-q(x, z) / ramp)
    return Z


# ---------------- the features (u along the leg, 0 the reference point) ----------------
def canyon_jump(h=1700, run=2600, gap=2200, w=6400, back=320, mat='kick', name='CANYON JUMP'):
    """two kickers lip to lip over a gap (a canyon, a channel, a lava river: centre at u = gap / 2), rideable both
    ways; built at the rim's height (base 0), so where they overhang the gap they stand on posts"""
    w = max(w, 9600)                             # (first test: the computers often missed a road-wide one and fell beside it)
    a = _face(run, h, 'kick')
    A = a + [(back, 0)]
    B = [(gap - back, 0), (gap, h)] + [(gap - u, hh) for u, hh in reversed(a[:-1])]
    return dict(t='strip', c=(0, 0), ang=0, w=w, name=name, kind='canyon', gap=gap, run=run, base=0.0, h=h,
                pieces=[dict(p=A, mat=mat, boost=_boost_segs(A, h)), dict(p=B, mat=mat, boost=_boost_segs(B, h))], arc=(0, h + 350, gap, h + 350, 500, 7))


def bridge(L=5200, w=5800, th=300, ramp=700, rails=420, mat='kick', name='BRIDGE'):
    """a flat deck at road height (12 over it) across a gap, thin, with rails; its ramps run down into the ground"""
    p = [(-L / 2 - ramp, 0), (-L / 2, th + 12), (L / 2, th + 12), (L / 2 + ramp, 0)]
    return dict(t='strip', c=(0, 0), ang=0, w=w, name=name, kind='bridge', L=L, base=-th, rails=rails,
                pieces=[dict(p=p, mat=mat, thick=th)], arc=(-L / 2 + 500, 60, L / 2 - 500, 60, 0, 5))


def spring(vy=80, l=1400, w=4600, name='spring'):
    """a spring pad (runtime/trickfx.c launches a kart on it up at vy); the 3600 after it kept clear for the landing"""
    vy *= 0.8                                    # (first test: 80-90 threw karts 2800 high for 150 frames)
    return dict(t='strip', c=(0, 0), ang=0, w=w, name=name, kind='spring', vy=vy, l=l, padtex='t7_spring', padtyp=0x100,
                pieces=[dict(p=[(-l / 2, 0), (l / 2 + 3600, 0)], mat='none')], pads=[(-l / 2, l / 2)], arc=(0, 1900, 3300, 1100, 1300, 6))


def marker(kind, L, w=6000, name=None, coins=5, y=150, **kw):
    """room on a leg for something placed after the layout (a tunnel, a geyser): no geometry, coins along it"""
    F = dict(t='strip', c=(0, 0), ang=0, w=w, name=name or kind, kind=kind, L=L, ghost=True,
             pieces=[dict(p=[(-L / 2, 0), (L / 2, 0)], mat='none')], arc=(-L / 2 + 600, y, L / 2 - 600, y, 0, coins))
    F.update(kw)
    return F


def skyway(h=2000, up=7000, deck=13000, w=6000, gap=4200, name='SKYWAY', vy=110, fw=200):
    """a ramp up onto an elevated deck (thin, rails, on pillars), a spring pad at its end (vy up, fw along: more than
    the gap needs) and a landing far below (2026-11-13: the kicker lip there before could not clear the gap)"""
    d2 = deck / 2; e = d2 + 860; lu = e + gap
    return dict(t='strip', c=(0, 0), ang=0, w=w, name=name, kind='skyway', h=h, deck=deck, rails=450,
                vy=vy * 0.8, fw=fw, padtex='t7_spring', padtyp=0x100, padh=h, pads=[(d2 - 1700, d2 - 300)],
                pieces=[dict(p=[(-d2 - up, 0), (-d2 - up * 0.5, h * 0.5), (-d2, h)], mat='kick'), dict(p=[(-d2, h), (-d2 + 260, 0)], mat='kick', nocol=True),
                        dict(p=[(-d2, h), (d2, h)], mat='kick', thick=400),
                        dict(p=[(lu - 900, 0), (lu, h * 0.6), (lu + 700, h * 0.6), (lu + 6700, 0)], mat='mound')],
                arc=(-d2 + 600, h + 160, d2, h + 160, 0, 9))


def overpass(h=1900, up=6000, D=10000, w=6400, name='OVERPASS', gap=4000, vy=100, fw=180):
    """a bridge carrying the road over itself (a figure eight): solid ramps, a thin deck on end pillars, a gap in the
    deck over the road below with a spring pad before it (2026-11-13: the tabletop on top did nothing)"""
    d2, g2 = D / 2, gap / 2
    return dict(t='strip', c=(0, 0), ang=0, w=w, name=name, kind='overpass', h=h, D=D, rails=450, railcol=False,
                vy=vy * 0.8, fw=fw, padtex='t7_spring', padtyp=0x100, padh=h, pads=[(-g2 - 1700, -g2 - 300)],
                # the ramps' backs under the deck ends are only seen (karts crossing onto the deck caught on them)
                pieces=[dict(p=[(-d2 - up, 0), (-d2 - up * 0.45, h * 0.55), (-d2, h)], mat='kick', boost=[1]), dict(p=[(-d2, h), (-d2 + 260, 0)], mat='kick', nocol=True),
                        dict(p=[(-d2, h), (-g2, h)], mat='kick', thick=400),
                        dict(p=[(g2, h), (d2, h)], mat='kick', thick=400),
                        dict(p=[(d2 - 260, 0), (d2, h)], mat='kick', nocol=True), dict(p=[(d2, h), (d2 + up, 0)], mat='kick')],
                arc=(-g2, h + 500, g2 + 800, h + 200, 1100, 9))


def boosted(berms):
    for b in berms: b['boost'] = True
    return berms


# ---------------- placement helpers ----------------
def find(F, name):
    for f in F:
        if f.get('name') == name: return f
    raise KeyError('feature %s was dropped (leg too short)' % name)


def fpos(F, u, v=0.0):
    a = math.radians(F['ang']); return (F['c'][0] + math.cos(a) * u - math.sin(a) * v, F['c'][1] + math.sin(a) * u + math.cos(a) * v)


def out_sign(F, centre=(0.0, 0.0)):
    """+1 when the feature's left normal (v > 0) points away from the arena's middle"""
    a = math.radians(F['ang']); nx, nz = -math.sin(a), math.cos(a)
    return 1.0 if nx * (F['c'][0] - centre[0]) + nz * (F['c'][1] - centre[1]) > 0 else -1.0


def crossing(F, u, out, inn, hw, hw_out=None, hw_in=None, centre=(0.0, 0.0), extra=()):
    """a band's points across the leg at feature F's u: from out outside the loop to inn inside (plus extra points
    beyond the inner end), half width hw at the road"""
    o = out_sign(F, centre)
    p = lambda s, w_: fpos(F, u, s * o) + (w_,)
    return [p(out, hw_out or hw), p(out * 0.4, hw), p(0, hw), p(-inn * 0.4, hw), p(-inn, hw_in or hw)] + list(extra)


def cached(f, step=250.0):
    """the terrain on a lattice, computed once a node, bilinear between (build7 asks for heights a great many times)"""
    memo = {}
    def node(i, j):
        k = (i, j)
        if k not in memo: memo[k] = f(i * step, j * step)
        return memo[k]
    def g(x, z):
        fx, fz = x / step, z / step; i, j = math.floor(fx), math.floor(fz); tx, tz = fx - i, fz - j
        return (node(i, j) * (1 - tx) * (1 - tz) + node(i + 1, j) * tx * (1 - tz) + node(i, j + 1) * (1 - tx) * tz + node(i + 1, j + 1) * tx * tz)
    return g


def graded(natural, route, corners, elev, half, blend=6000.0):
    """a road whose height runs between elev[i] at corner i (smoothly, round the loop), the ground beside it blending
    from the road's height to the natural one over blend"""
    n = len(route); cum = [0.0]
    for i in range(n): a, b = route[i], route[(i + 1) % n]; cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    S = cum[-1]
    cs = [cum[min(range(n), key=lambda i: math.hypot(route[i][0] - c[0], route[i][1] - c[1]))] for c in corners]
    order_ = sorted(range(len(cs)), key=lambda i: cs[i])
    ks = [(cs[i], elev[i]) for i in order_]
    def e(s):
        s %= S
        for i in range(len(ks)):
            (s0, e0), (s1, e1) = ks[i], ks[(i + 1) % len(ks)]
            if s1 <= s0: s1 += S
            ss_ = s if s >= s0 else s + S
            if s0 <= ss_ <= s1: return e0 + (e1 - e0) * ss(((ss_ - s0) / (s1 - s0)) if s1 > s0 else 0)
        return ks[0][1]
    segs = [(route[i], route[(i + 1) % n], cum[i]) for i in range(n)]
    def f(x, z):
        best, bs = 1e18, 0.0
        for (ax, az), (bx, bz), c0 in segs:
            vx, vz = bx - ax, bz - az; L2 = vx * vx + vz * vz or 1.0
            t = max(0.0, min(1.0, ((x - ax) * vx + (z - az) * vz) / L2))
            d = (x - ax - vx * t) ** 2 + (z - az - vz * t) ** 2
            if d < best: best, bs = d, c0 + math.sqrt(L2) * t
        w = 1 - ss((math.sqrt(best) - half) / blend)
        return e(bs) * w + natural(x, z) * (1 - w)
    return f


def finish(T, natural=None, raises=(), grade=None, props=(), centre=(0.0, 0.0)):
    """after the layout: markers become tunnels / geysers, springs and geysers the fx list, posts under overhanging
    ramps, the terrain (road flat or graded, then the zones' basins, raised ground), then the scenery"""
    F = T['features']; Z = T.setdefault('zones', []); fx = T.setdefault('fx', []); P = T.setdefault('props', [])
    for f in F:
        k = f.get('kind')
        if k == 'spring': fx.append(dict(t='pad', x=f['c'][0], z=f['c'][1], ang=f['ang'], hw=f['w'] / 2 - 150, hl=f['l'] / 2, vy=f['vy']))
        elif k in ('overpass', 'skyway'):        # its pad before the gap, up on the deck (fx y: the deck's)
            u0, u1 = f['pads'][0]; x, z = fpos(f, (u0 + u1) / 2)
            fx.append(dict(t='pad', x=x, z=z, ang=f['ang'], hw=f['w'] / 2 - 150, hl=(u1 - u0) / 2, vy=f['vy'], fw=f.get('fw', 0)))
        elif k == 'tunnel':
            T.setdefault('tunnels', []).append(dict(a=fpos(f, -f['L'] / 2), b=fpos(f, f['L'] / 2), w=T['road_w'] + 1600, h=f.get('th', 3400), kind=f.get('tk', 'rock')))
        elif k == 'geyser':
            x, z = f['c']; fx.append(dict(t='geyser', x=x, z=z, r=900, period=f.get('period', 300), on=f.get('on', 80), vy=f.get('vy', 75), kind=f.get('gk', 0), phase=f.get('phase', 0)))
            P.append((x, z, 'vent', 1050))
    for g in T.get('geysers', []):               # off the road
        x, z, gk, ph = g; fx.append(dict(t='geyser', x=x, z=z, r=900, period=300, on=80, vy=75, kind=gk, phase=ph)); P.append((x, z, 'vent', 1050))
    nat = natural or (lambda x, z: 0.0)
    base = graded(nat, T['route'], grade[0], grade[1], T['road_w'] / 2 + 1500) if grade else (t7.flat_road(nat, T['route'], T['road_w'] / 2 + 1500) if natural else nat)
    dz = [z['dh'] for z in Z if 'dh' in z]
    rs = list(raises)
    T['height'] = cached(lambda x, z: base(x, z) + sum(d(x, z) for d in dz) + sum(r(x, z) for r in rs))
    T['graded'] = True
    if grade: T['follow'] = True
    for f in F:                                  # posts under ramps that overhang a gap (canyon jumps), and under decks
        k = f.get('kind')
        if k == 'canyon':
            gap, run, w2 = f['gap'], f['run'], f['w'] / 2
            for u in [i * 650.0 for i in range(int(-run // 650), int((gap + run) // 650) + 1)]:
                if 0 < u < gap: continue
                for v in (-w2 + 380, w2 - 380):
                    x, z = fpos(f, u, v); h = T['height'](x, z)
                    if h < -300 and any(zz['in'](x, z) for zz in Z): P.append((x, z, 'pillar', -h - 30))
        elif k in ('skyway', 'overpass'):
            us = ([i * 3200.0 - f['deck'] / 2 + 900 for i in range(int(f['deck'] // 3200) + 1)] if k == 'skyway' else [-f['D'] / 2 + 500, f['D'] / 2 - 500])
            for u in us:
                for v in (-f['w'] / 2 + 600, f['w'] / 2 - 600):
                    x, z = fpos(f, u, v); P.append((x, z, 'pillar', f['h'] - 400 - T['height'](x, z) - 30))
    for p in props: scatter(T, *p)


def scatter(T, kind, n, size, seed, clear=1800.0, ok=(None,), region=None, gap=None):
    """n props of a kind round the arena, clear of the road (its edge + clear), the jumps, berms, tunnels, the other
    props, in the zones listed in ok (None: plain ground); region (x0, z0, x1, z1) limits where"""
    rr = random.Random(seed); out = T.setdefault('props', [])
    x0, z0, x1, z1, R = T['arena']
    if region: rx0, rz0, rx1, rz1 = region
    else: rx0, rz0, rx1, rz1 = x0 + 2500, z0 + 2500, x1 - 2500, z1 - 2500
    route = T['route']; segs = [(route[i], route[(i + 1) % len(route)]) for i in range(len(route))]
    half = T['road_w'] / 2 + T.get('berm', 900) + clear
    gap = gap or size * 0.8
    def road_d(x, z):
        best = 1e18
        for (ax, az), (bx, bz) in segs:
            vx, vz = bx - ax, bz - az; L2 = vx * vx + vz * vz or 1.0
            t = max(0.0, min(1.0, ((x - ax) * vx + (z - az) * vz) / L2))
            best = min(best, (x - ax - vx * t) ** 2 + (z - az - vz * t) ** 2)
        return math.sqrt(best)
    def in_arena(x, z):
        cx, cz = min(max(x, x0 + R), x1 - R), min(max(z, z0 + R), z1 - R)
        return math.hypot(x - cx, z - cz) < R - 2500 if (cx != x or cz != z) else (x0 + 2500 < x < x1 - 2500 and z0 + 2500 < z < z1 - 2500)
    def near_feat(x, z):
        for F in T['features']:
            if F['t'] == 'strip':
                a = math.radians(F['ang']); dx, dz = math.cos(a), math.sin(a); cx, cz = F['c']
                u, v = (x - cx) * dx + (z - cz) * dz, -(x - cx) * dz + (z - cz) * dx
                us = [p[0] for pc in F['pieces'] for p in pc['p']] + [q for pd in F.get('pads', ()) for q in pd]
                if abs(v) < F['w'] / 2 + 900 and min(us) - 1200 < u < max(us) + 1200: return True
            else:
                r = math.hypot(x - F['c'][0], z - F['c'][1])
                if F['r0'] - 800 < r < F['r1'] + 1200: return True
        for Tn in T.get('tunnels', []):
            (ax, az), (bx, bz) = Tn['a'], Tn['b']; vx, vz = bx - ax, bz - az; L2 = vx * vx + vz * vz
            t = max(0.0, min(1.0, ((x - ax) * vx + (z - az) * vz) / L2))
            if math.hypot(x - ax - vx * t, z - az - vz * t) < Tn['w'] / 2 + 4000: return True
        return False
    def zone(x, z):
        k = None
        for Z in T.get('zones', []):
            if Z['in'](x, z): k = Z['kind']
        return k
    placed = 0
    for _ in range(n * 60):
        if placed >= n: break
        x, z = rr.uniform(rx0, rx1), rr.uniform(rz0, rz1)
        if not in_arena(x, z) or zone(x, z) not in ok or road_d(x, z) < half or near_feat(x, z): continue
        if any(math.hypot(x - p[0], z - p[1]) < max(gap, p[3] * 0.5) for p in out): continue
        out.append((x, z, kind, size * rr.uniform(0.75, 1.25))); placed += 1


def theme_of(key, **kw):
    th = dict(TRACKS[key]['theme']); th.update(kw); return th


# =====================================================================================================
# CORAL COVE (Kicker Cup): a lagoon beach: the south road runs half in the surf, a pier over the inlet, a boat ramp
# jump over the channel, palms, umbrellas, a lighthouse, water spouts
# =====================================================================================================
C = [(-36000, 17000), (34000, 19000), (38000, -15000), (-34000, -18000)]
RAD = [8000] * 4
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [table2(1300, 2000, 2400, W_, mat='kick', name='boardwalk'), spring(80, name='beach bounce'), bridge(4800, name='PIER'),
             spine(1800, 2600, 400, W_, mat='mound', name='dune spine'), gap2(1900, 2700, 2100, W_, name='surf gap')]),
    (LG[1], [canyon_jump(1700, 2400, 2200, W_, name='BOAT RAMP')]),
    (LG[2], [rainbow(1600, 2400, 1200, W_, mat='mound', name='sandbar'), marker('geyser', 2600, name='spout', gk=0, phase=0),
             gap2(1700, 2400, 1900, W_, mat='mound', name='WAVE GAP'), spring(85, name='surf spring'), table2(1400, 2200, 2600, W_, mat='mound', name='dune table')]),
    (LG[3], [spine(1700, 2400, 300, W_, name='lifeguard'), gap2(1600, 2300, 1800, W_, mat='mound', name='cove gap')]),
], margin=1300, items=4)
F += boosted(corner_berms(C, RAD, 6400, 700))
T = TRACKS['coral'] = dict(
    slot='mini7', title='CORAL COVE', arena=(-44000, -28000, 44000, 28000, 9000), cell=2400,
    theme=theme_of('coral', sandcol=(236, 214, 160), watercol=(30, 170, 200), rockcol=(170, 150, 120), springcol=(240, 90, 40)),
    route=R, features=F, road_w=6000, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 11000, 44000, 0, 0, 0), nresp=10)
pier, ramp_ = find(F, 'PIER'), find(F, 'BOAT RAMP')
T['zones'] = [oval('sand', 0, -2500, 27000, 16000),
              oval('water', 0, -3000, 20000, 14000, depth=280, level=-90, ramp=2600),
              oval('deep', 0, -1500, 10000, 5500, depth=1300, ramp=1800),
              band('deep', crossing(pier, 0, 5500, 9000, 1700, hw_in=1900), depth=1300, level=-88, ramp=900),
              band('deep', crossing(ramp_, ramp_['gap'] / 2, 5000, 17000, 1700, hw_in=1900), depth=1300, level=-88, ramp=900)]
T['geysers'] = [(-8000, -9000, 0, 100), (9000, -8500, 0, 200)]
finish(T, props=[('palm', 34, 3000, 1, 1500, ('sand', None)), ('umbrella', 14, 1400, 2, 1200, ('sand',)), ('lifeguard', 3, 1900, 3, 2500, ('sand',)),
                 ('boat', 6, 1900, 4, 600, ('deep',)), ('buoy', 10, 450, 5, 300, ('deep', 'water')), ('rock', 10, 900, 6, 1500, (None,)),
                 ('shrub', 16, 700, 7, 1500, (None,))])
T['props'].append((-39500, 22500, 'lighthouse', 9000))

# =====================================================================================================
# FLOWLINE CANYON (Kicker Cup, long): a canyon winds through the middle; the road jumps it in the north and crosses a
# rope bridge in the south (fall in: Lakitu); flow jumps, an arch tunnel through a rock, boosted banked bends, pines
# =====================================================================================================
C = [(-48000, 25000), (-14000, 27000), (16000, 20000), (48000, 26000), (48000, -8000), (30000, -26000), (-2000, -27000), (-30000, -22000), (-48000, -4000)]
RAD = [7000, 7000, 7000, 7000, 8000, 7000, 7000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6200
F, I, Cn = course([
    (LG[0], [rainbow(1500, 2100, 1200, W_, name='flow 1'), spine(1600, 2200, 300, W_, mat='mound', name='flow 2'), spring(80, name='rim spring')]),
    (LG[1], [canyon_jump(1800, 2600, 2200, W_, name='CANYON JUMP')]),
    (LG[2], [table2(1300, 1900, 2000, W_, name='mesa table'), gap2(1800, 2500, 2000, W_, mat='mound', name='rim gap'), rainbow(1600, 2300, 1000, W_, name='crest')]),
    (LG[3], [spine(1700, 2400, 400, W_, mat='mound', name='hoodoo spine'), spring(85, name='canyon bounce')]),
    (LG[4], [gap2(1700, 2300, 1900, W_, mat='mound', name='wash gap')]),
    (LG[5], [bridge(5600, name='ROPE BRIDGE', mat='kick')]),
    (LG[6], [table2(1400, 2000, 2400, W_, mat='kick', name='kicker table'), marker('tunnel', 9000, name='ARCH TUNNEL', tk='rock')]),
    (LG[7], [rainbow(1700, 2400, 1300, W_, name='big flow')]),
    (LG[8], [gap2(1900, 2600, 2100, W_, mat='mound', name='FLOW GAP'), spring(80, name='west spring')]),
], margin=1100, items=2)
F += boosted(corner_berms(C, RAD, 6000, 900))
T = TRACKS['flowline'] = dict(
    slot='mini7', title='FLOWLINE CANYON', arena=(-56000, -34000, 56000, 34000, 10000), cell=2100,
    theme=dict(ground=((206, 150, 96), (164, 110, 66)), pebble=[(220, 170, 120), (130, 90, 60), (180, 120, 80)], road='clay', roadcol=(186, 128, 80),
               berm=(150, 96, 60), mound=(176, 118, 74), kick='wood', bar=((240, 240, 240), (200, 80, 30)), decor='fence', fencecol=(110, 76, 44),
               rockcol=(170, 92, 60), pinecol=(46, 104, 58), pillarcol=(110, 80, 50), railcol=(150, 110, 70),
               sky=((70, 130, 220), (250, 210, 160)), cloud=(255, 240, 220), sil=('mesa', (170, 90, 60))),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[0], 800), cam=(0, 16000, 60000, 0, 0, 0), cliffs=True, nresp=14)
cj, rb = find(F, 'CANYON JUMP'), find(F, 'ROPE BRIDGE')
A = fpos(cj, cj['gap'] / 2); B = fpos(rb, 0)
oa, ob = out_sign(cj), out_sign(rb)
na = (lambda f, o: (-math.sin(math.radians(f['ang'])) * o, math.cos(math.radians(f['ang'])) * o))
nA, nB = na(cj, oa), na(rb, ob)
lerp = lambda p, q, t, dx=0, dz=0: (p[0] + (q[0] - p[0]) * t + dx, p[1] + (q[1] - p[1]) * t + dz)
pts = [(A[0] + nA[0] * 6500, A[1] + nA[1] * 6500, 1800), (A[0] + nA[0] * 2000, A[1] + nA[1] * 2000, 2000), A + (2000,), (A[0] - nA[0] * 3000, A[1] - nA[1] * 3000, 2200),
       lerp(A, B, 0.3, 6000) + (3200,), lerp(A, B, 0.55, -3500) + (3600,), lerp(A, B, 0.8, 4500) + (3000,),
       (B[0] - nB[0] * 3000, B[1] - nB[1] * 3000, 2200), B + (2000,), (B[0] + nB[0] * 2000, B[1] + nB[1] * 2000, 2000), (B[0] + nB[0] * 4800, B[1] + nB[1] * 4800, 1800)]
T['zones'] = [band('pit', pts, depth=2600, ramp=1100), oval('sand', -30000, 6000, 9000, 6000), oval('sand', 28000, 6000, 8000, 5000)]
finish(T, natural=hills(hill(-30000, 6000, 8000, 6000, 900), hill(30000, 4000, 7000, 6000, 800)),
       props=[('pine', 46, 3800, 11, 1500, (None,)), ('hoodoo', 16, 3200, 12, 2500, (None, 'sand')), ('rock', 26, 1100, 13, 1200, (None, 'sand')),
              ('shrub', 20, 700, 14, 1200, (None,)), ('cactus', 8, 2200, 15, 2000, ('sand',))])

# =====================================================================================================
# DUST DEVIL MESA (Canyon Cup): desert: two tornadoes wander the middle and throw karts sky high; dunes of sand,
# cacti and rocks, a plateau, spring pads
# =====================================================================================================
C = [(-30000, 18000), (27000, 18000), (31000, -15000), (-5000, -19000), (-31000, -6000)]
RAD = [7000, 7000, 7000, 6000, 6000]
R, LG = rounded(C, RAD)
W_ = 6600
F, I, Cn = course([
    (LG[0], [gap2(1700, 2500, 2000, W_, name='east gap'), spring(85, name='dune spring'), spine(2000, 3000, 400, W_, mat='mound', name='dune spine'),
             gap2(2300, 3100, 2400, W_ + 800, name='MESA GAP')]),
    (LG[1], [spine(1800, 2800, 600, W_, shape='straight', name='south spine'), gap2(1600, 2400, 1900, W_, mat='mound', name='south gap')]),
    (LG[2], [table2(1500, 2400, 3000, W_, name='mesa top'), spring(90, name='canyon spring'), gap2(2400, 3200, 2400, W_ + 800, mat='mound', name='canyon gap')]),
    (LG[3], [gap2(1800, 2600, 2100, W_, shape='straight', name='north gap'), rainbow(2000, 3000, 1400, W_, name='dune')]),
    (LG[4], [spine(1700, 2600, 300, W_, name='west spine')]),
])
F += boosted(corner_berms(C, RAD, 6200, 750))
T = TRACKS['dustdevil'] = dict(
    slot='mini7', title='DUST DEVIL MESA', arena=(-38000, -27000, 38000, 27000, 9000), cell=2600,
    theme=theme_of('dustdevil', sandcol=(226, 186, 130), rockcol=(176, 110, 70), springcol=(240, 140, 30)),
    route=R, features=F, road_w=6000, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 9000, 46000, 0, 0, 0), nresp=10)
T['zones'] = [oval('sand', -14000, 2000, 9000, 6000), oval('sand', 12000, -2000, 8000, 5500), oval('sand', 30000, 22000, 9000, 4000), oval('sand', -30000, -20000, 7000, 5000)]
T['fx_extra'] = [dict(t='tornado', x=-1000, z=0, ax=16000, az=9000, px=1700, pz=1300, r=1500, h=7000, lift=50),
                 dict(t='tornado', x=2000, z=-1000, ax=13000, az=10000, px=1150, pz=1900, r=1300, h=6000, lift=46)]
finish(T, raises=[hill(-14000, 2000, 7000, 4500, 1300), hill(14000, -3000, 5000, 4000, 900)],
       props=[('cactus', 30, 2400, 21, 1500, (None, 'sand')), ('rock', 24, 1200, 22, 1500, (None, 'sand')), ('hoodoo', 6, 3000, 23, 3000, (None,)),
              ('shrub', 12, 600, 24, 1500, (None,))])
T['fx'] += T.pop('fx_extra')

# =====================================================================================================
# REDROCK RALLY (Canyon Cup): red sandstone: a slot canyon crossed by a jump and a plank bridge, an arch tunnel,
# hoodoos and buttes, step-down gaps
# =====================================================================================================
C = [(-31000, 4000), (-16000, 19000), (16000, 19000), (31000, 2000), (16000, -18000), (-16000, -18000)]
RAD = [6000, 7000, 7000, 6000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [gap2(1500, 2200, 1800, W_, mat='mound', name='rock hop')]),
    (LG[1], [canyon_jump(1700, 2500, 2200, W_, name='SLOT CANYON')]),
    (LG[2], [marker('tunnel', 8000, name='RED ARCH', tk='rock'), spring(80, name='butte spring')]),
    (LG[3], [spine(1800, 2600, 300, W_, shape='straight', name='needle')]),
    (LG[4], [bridge(5000, name='PLANK BRIDGE'), gap2(2300, 3100, 2400, W_ + 800, name='REDROCK GAP')]),
    (LG[5], [gap2(1700, 2400, 1900, W_, mat='mound', shape='straight', name='slot gap'), rainbow(1500, 2200, 1000, W_, name='wash')]),
], margin=1200, items=3)
F += boosted(corner_berms(C, RAD, 6000, 700))
T = TRACKS['redrock'] = dict(
    slot='mini7', title='REDROCK RALLY', arena=(-39000, -26000, 39000, 27000, 9000), cell=2000,
    theme=theme_of('redrock', rockcol=(190, 96, 60), pillarcol=(120, 80, 50), railcol=(150, 100, 60)),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[0], 900), cam=(0, 12000, 44000, 0, 0, 0), cliffs=True, nresp=10)
sc, pb = find(F, 'SLOT CANYON'), find(F, 'PLANK BRIDGE')
A = fpos(sc, sc['gap'] / 2); B = fpos(pb, 0)
nA, nB = na(sc, out_sign(sc)), na(pb, out_sign(pb))
pts = [(A[0] + nA[0] * 5000, A[1] + nA[1] * 5000, 1600), A + (1900,), lerp(A, B, 0.35, 3000) + (2400,), lerp(A, B, 0.65, -2000) + (2400,), B + (1900,),
       (B[0] + nB[0] * 4500, B[1] + nB[1] * 4500, 1600)]
T['zones'] = [band('pit', pts, depth=2400, ramp=900)]
finish(T, natural=hills(hill(-25000, -14000, 5000, 4000, 900), hill(26000, 14000, 5000, 4000, 900)),
       raises=[hill(-9000, 4000, 4500, 3500, 1500), hill(14000, -3000, 4000, 3500, 1300)],
       props=[('hoodoo', 30, 3600, 31, 2200, (None,)), ('rock', 24, 1300, 32, 1400, (None,)), ('cactus', 10, 2000, 33, 2000, (None,)), ('shrub', 12, 600, 34, 1500, (None,))])

# =====================================================================================================
# SUNBURST SPEEDWAY (Canyon Cup): a figure-eight speedway: the road crosses itself on an overpass; boosted banked
# bends, spring pads, flags and tents in the infields
# =====================================================================================================
C = [(-34000, 20000), (34000, -20000), (34000, 20000), (-34000, -20000)]
RAD = [8000] * 4
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [overpass(1900, 6000, 14000, W_, name='OVERPASS', vy=135)]),   # 2026-11-13 14000 (10000: the lower road's edges clipped the ramp ends)
    (LG[1], [spring(90, name='east spring')]),
    (LG[2], [table2(1400, 2200, 3000, W_, mat='kick', name='sun table'), marker('under', 15000, name='under the overpass', coins=0),
              gap2(2400, 3200, 2500, W_ + 800, name='SUNBURST GAP')]),   # 2026-11-13: the jumps clear of the crossing (CPUs caught at the gap's pad under the deck)
    (LG[3], [spring(90, name='west spring')]),
], margin=1500, items=4)
F += boosted(corner_berms(C, RAD, 6800, 1000))
T = TRACKS['sunburst'] = dict(
    slot='mini7', title='SUNBURST SPEEDWAY', arena=(-44000, -29000, 44000, 29000, 9000), cell=2600,
    theme=theme_of('sunburst', railcol=(240, 240, 240), pillarcol=(150, 146, 140), springcol=(240, 120, 20)),
    route=R, features=F, road_w=6200, berm=500, items=I, coins=Cn, start=_start(LG[1], 600), cam=(0, 22000, 40000, 0, 0, 0), nresp=8)
finish(T, props=[('flag', 26, 2200, 41, 1200, (None,)), ('umbrella', 10, 1500, 42, 1500, (None,)), ('cactus', 8, 2200, 43, 2000, (None,)),
                 ('rock', 8, 900, 44, 1500, (None,))])

# =====================================================================================================
# VOLCANO VENT (Canyon Cup): a volcano in the middle with a lava crater, a lava river crossed by a jump and a stone
# bridge, lava geysers that throw karts up, a lava pool
# =====================================================================================================
C = [(-30000, 17000), (6000, 21000), (31000, 8000), (24000, -18000), (-4000, -12000), (-30000, -18000)]
RAD = [7000, 8000, 7000, 7000, 6000, 7000]
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [canyon_jump(1700, 2500, 2200, W_, mat='kick', name='LAVA JUMP')]),
    (LG[1], [table2(1400, 2100, 2400, W_, name='lava table'), marker('geyser', 2600, name='vent 1', gk=1, phase=0)]),
    (LG[2], [rainbow(1900, 2700, 1300, W_, name='CALDERA'), marker('geyser', 2600, name='vent 2', gk=1, phase=150)]),
    (LG[3], [spine(1700, 2400, 300, W_, shape='straight', name='crater spine'), spring(85, name='magma spring')]),
    (LG[4], [bridge(4800, name='STONE BRIDGE', mat='mound')]),
    (LG[5], [gap2(2300, 3100, 2400, W_ + 800, name='ERUPTION')]),
], margin=1200, items=3)
F += boosted(corner_berms(C, RAD, 6000, 700))
T = TRACKS['volcano'] = dict(
    slot='mini7', title='VOLCANO VENT', arena=(-39000, -26000, 39000, 29000, 9000), cell=2200,
    theme=theme_of('volcano', rockcol=(120, 66, 52), pillarcol=(60, 50, 48), railcol=(90, 70, 60), springcol=(250, 120, 20), shrubcol=(70, 90, 50)),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[1], 900), cam=(-4000, 17000, 36000, 2000, 2500, 0), cliffs=True, nresp=10)
lj, sb = find(F, 'LAVA JUMP'), find(F, 'STONE BRIDGE')
A = fpos(lj, lj['gap'] / 2); B = fpos(sb, 0)
nA, nB = na(lj, out_sign(lj, (2000, 3000))), na(sb, out_sign(sb, (2000, 3000)))
pts = [(A[0] + nA[0] * 4500, A[1] + nA[1] * 4500, 1700), A + (1800,), lerp(A, B, 0.4, -4000) + (2200,), lerp(A, B, 0.7, -2000) + (2000,), B + (1800,),
       (B[0] + nB[0] * 4500, B[1] + nB[1] * 4500, 1700)]
T['zones'] = [oval('rock', 2000, 3000, 9800, 7800), band('lava', pts, depth=600, level=-260, ramp=700), oval('lava', 2000, 3000, 2000, 1700, level=5400),
              oval('lava', 17000, -3000, 3200, 2400, depth=500, level=-220, ramp=900)]
T['geysers'] = [(-6000, 9000, 1, 60), (9000, 9500, 1, 220), (-2000, -5000, 1, 120)]
finish(T, raises=[lambda x, z: 8000 * max(0.0, 1 - math.hypot((x - 2000) / 9500, (z - 3000) / 7500)) ** 0.7 - 2800 * ss(1 - math.hypot((x - 2000) / 2800, (z - 3000) / 2400))],
       props=[('rock', 30, 1300, 51, 1400, (None,)), ('shrub', 8, 600, 52, 1500, (None,)), ('crystal', 6, 1600, 53, 2000, (None,))])
T['theme']['crystalcol'] = (90, 40, 40)

# =====================================================================================================
# HARBOR HIGHWAY (City Cup, long): quays round a harbour basin: a drawbridge over the north canal, a boat ramp jump over
# the east one, a warehouse tunnel, containers, cranes, boats, a lighthouse
# =====================================================================================================
C = [(-42000, 21000), (40000, 21000), (42000, -20000), (-40000, -21000)]
RAD = [8000] * 4
R, LG = rounded(C, RAD)
W_ = 6200
F, I, Cn = course([
    (LG[0], [table2(1300, 2000, 2600, W_, mat='kick', name='dock table'), bridge(5400, name='DRAWBRIDGE', mat='kick'),
             marker('tunnel', 10000, name='WAREHOUSE', tk='shed', th=3200), spring(85, name='cargo spring')]),
    (LG[1], [canyon_jump(1700, 2400, 2200, W_, mat='kick', name='CANAL JUMP')]),
    (LG[2], [gap2(1500, 2000, 1800, W_, name='bus gap'), rainbow(1600, 2400, 1200, W_, mat='kick', name='flyover'), spring(85, name='crate spring'),
             table2(1200, 1900, 2400, W_, mat='kick', name='launch table')]),
    (LG[3], [spine(1700, 2400, 300, W_, name='west spine'), gap2(1700, 2300, 2000, W_, name='dock gap')]),
], margin=1100, items=3)
T = TRACKS['harbor'] = dict(
    slot='mini7', title='HARBOR HIGHWAY', arena=(-50000, -30000, 50000, 30000, 9000), cell=2400,
    theme=theme_of('harbor', watercol=(30, 90, 120), railcol=(220, 180, 30), pillarcol=(110, 110, 116), springcol=(220, 180, 30), rockcol=(120, 120, 124)),
    route=R, features=F, road_w=5600, berm=500, items=I, coins=Cn, start=_start(LG[0], 900), cam=(-6000, 14000, 30000, 2000, 1000, -4000), nresp=10)
db, cnl = find(F, 'DRAWBRIDGE'), find(F, 'CANAL JUMP')
T['zones'] = [oval('deep', 0, -1000, 24000, 10000, depth=1500, level=-180, ramp=700),
              band('deep', crossing(db, 0, 5000, 10000, 1800), depth=1500, level=-182, ramp=700),
              band('deep', crossing(cnl, cnl['gap'] / 2, 4500, 15000, 1700), depth=1500, level=-182, ramp=700)]
finish(T, props=[('crane', 6, 9000, 61, 3000, (None,)), ('boat', 8, 2200, 62, 900, ('deep',)), ('buoy', 10, 450, 63, 500, ('deep',)),
                 ('crate', 26, 700, 64, 1000, (None,)), ('lamp', 18, 1800, 65, 1600, (None,))])
T['props'].append((24500, 8500, 'lighthouse', 8000))
_bx = containers(T['route'], T['arena'], 6500, 3)
T['boxes'] = [b for b in _bx if not any(Z['q'](b[0], b[1]) < 2500 for Z in T['zones']) and
              not any(math.hypot(b[0] - p[0], b[1] - p[1]) < 2500 for p in T['props'])]

# =====================================================================================================
# STEELWORKS (City Cup): a steel mill: a long smelting shed, a catwalk over a molten pool, a jump over a molten
# channel, steam vents that blast karts up, tanks, smokestacks, containers
# =====================================================================================================
C = [(-32000, 15000), (30000, 15000), (30000, -2000), (6000, -2000), (6000, -17000), (-32000, -17000)]
RAD = [6000, 6000, 5000, 5000, 6000, 6000]
R, LG = rounded(C, RAD)
W_ = 6000
F, I, Cn = course([
    (LG[0], [gap2(1700, 2400, 2000, W_, name='crane gap'), marker('tunnel', 12000, name='SMELTING SHED', tk='shed', th=3400),
             spring(85, name='pressure pad'), gap2(2200, 3000, 2300, W_ + 600, name='FURNACE GAP')]),
    (LG[1], [marker('geyser', 2600, name='steam vent', gk=2, phase=0)]),
    (LG[2], [spine(1500, 2100, 300, W_, shape='straight', name='pipe hop')]),
    (LG[3], [table2(1300, 1900, 2200, W_, mat='kick', name='forklift')]),
    (LG[4], [bridge(4600, name='CATWALK', mat='kick')]),
    (LG[5], [canyon_jump(1700, 2300, 2200, W_, mat='kick', name='MOLTEN JUMP'), marker('geyser', 2600, name='steam vent 2', gk=2, phase=150)]),
], margin=1100, items=3)
T = TRACKS['steelworks'] = dict(
    slot='mini7', title='STEELWORKS', arena=(-40000, -25000, 38000, 23000, 8000), cell=2200,
    theme=theme_of('steelworks', railcol=(240, 190, 30), pillarcol=(90, 90, 96), rockcol=(90, 84, 80), springcol=(240, 190, 30)),
    route=R, features=F, road_w=5400, berm=500, fence_off=3100, items=I, coins=Cn, start=_start(LG[0], 900), cam=(-2000, 12000, 21000, 0, 1000, -4000), nresp=10, key_salt=2)
T['fence_off'] = None
cw, mj = find(F, 'CATWALK'), find(F, 'MOLTEN JUMP')
T['zones'] = [band('lava', crossing(cw, 0, 4000, 5000, 1700), depth=500, level=-220, ramp=600),
              band('lava', crossing(mj, mj['gap'] / 2, 4000, 5000, 1700), depth=500, level=-220, ramp=600),
              oval('lava', -12000, 0, 4000, 2500, depth=500, level=-220, ramp=800)]
T['geysers'] = [(-22000, 2000, 2, 80), (16000, 6000, 2, 200)]
finish(T, props=[('tank', 8, 3000, 71, 2500, (None,)), ('stack', 6, 12000, 72, 3000, (None,)), ('crate', 16, 800, 73, 1200, (None,)),
                 ('lamp', 14, 1800, 74, 1500, (None,))])
_bx = containers(T['route'], T['arena'], 6500, 11)
T['boxes'] = [b for b in _bx if not any(Z['q'](b[0], b[1]) < 2500 for Z in T['zones']) and not any(math.hypot(b[0] - p[0], b[1] - p[1]) < 2800 for p in T['props'])]

# =====================================================================================================
# NEON NIGHTS (City Cup): a night city: a skyway on pillars ends in a huge drop, an underpass, glowing spring pads,
# neon signs, towers and street lamps
# =====================================================================================================
C = [(-33000, 12000), (-12000, 19000), (12000, 12000), (32000, 18000), (32000, -16000), (-33000, -16000)]
RAD = [6000, 7000, 6000, 6000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6000
F, I, Cn = course([
    (LG[0], [gap2(1600, 2300, 1900, W_, name='neon gap')]),
    (LG[1], [spine(1800, 2500, 400, W_, name='billboard')]),
    (LG[2], [table2(1300, 2000, 2200, W_, mat='kick', name='plaza table'), spring(90, name='plaza spring')]),
    (LG[3], [gap2(1900, 2600, 2100, W_, shape='straight', name='tower gap'), spring(90, name='tower spring')]),
    (LG[4], [skyway(2000, 7000, 13000, W_, 4200, name='SKYWAY')]),
    (LG[5], [marker('tunnel', 9000, name='UNDERPASS', tk='shed', th=3200)]),
], margin=1100, items=3)
T = TRACKS['neon'] = dict(
    slot='mini7', title='NEON NIGHTS', arena=(-41000, -24000, 40000, 27000, 8000), cell=2600,
    theme=theme_of('neon', railcol=(40, 240, 255), pillarcol=(80, 76, 100), springcol=(230, 40, 200)),
    route=R, features=F, road_w=5400, berm=500, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 24000, 26000, 0, 0, -5000), nresp=8)
finish(T, props=[('building', 16, 4600, 81, 4000, (None,)), ('neon', 24, 2600, 82, 1500, (None,)), ('lamp', 24, 1800, 83, 1400, (None,))])

# =====================================================================================================
# FROSTBITE BIG AIR (Summit Cup): the snow park's big kickers, a frozen river across the park (ice), snow drifts,
# a frozen lake, snowy pines, snowmen, a ski lodge
# =====================================================================================================
C = [(-30000, 16000), (28000, 16000), (30000, -14000), (-28000, -16000)]
RAD = [9000] * 4
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [gap2(1900, 2800, 2100, W_, name='slope 1'), gap2(2300, 3200, 2400, W_, name='slope 2'), spine(2600, 3600, 500, W_, name='ICE PYRAMID')]),
    (LG[1], [table2(1500, 2200, 2000, W_, mat='kick', name='east table')]),
    (LG[2], [gap2(2800, 4000, 2600, W_ + 1200, name='MEGA KICKER'), spine(2000, 3000, 300, W_, shape='straight', name='north spine'),
             spring(90, name='snow spring')]),
    (LG[3], [rainbow(1800, 2800, 1400, W_, mat='kick', name='west rainbow')]),
])
F += boosted(corner_berms(C, RAD, 6600, 800))
T = TRACKS['frostbite'] = dict(
    slot='mini7', title='FROSTBITE BIG AIR', arena=(-38000, -26000, 38000, 26000, 9000), cell=2600,
    theme=theme_of('frostbite', pinecol=(40, 96, 70), rockcol=(150, 160, 176), springcol=(40, 120, 220)),
    route=R, features=F, road_w=6200, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 12000, 44000, 0, 0, 0), nresp=8, key_salt=3)
T['zones'] = [band('ice', [(-38000, 1500, 2600), (-20000, -1500, 2800), (0, 2000, 3000), (20000, -1000, 2800), (38000, 1500, 2600)]),
              oval('ice', 0, 1000, 12000, 6000), oval('snow', -33000, -21000, 6000, 4000), oval('snow', 33000, 21000, 6000, 4000), oval('snow', 30000, -21000, 5000, 3500)]
finish(T, natural=hills(hill(-24000, 2000, 5500, 7000, 1000), hill(24000, 1000, 5000, 6000, 900), hill(0, 21500, 9000, 2500, 600)),
       props=[('pine', 40, 3600, 91, 1500, (None, 'snow')), ('snowman', 8, 900, 92, 2000, (None,)), ('chalet', 2, 5000, 93, 4000, (None,)),
              ('flag', 12, 2200, 94, 1500, (None,)), ('rock', 8, 900, 95, 1500, (None, 'snow'))])

# =====================================================================================================
# GLACIER GAP (Summit Cup): an ice field split by a crevasse (a jump over it and an ice bridge across), an ice cave,
# ice spires, glacier ice in the middle
# =====================================================================================================
C = [(-31000, 18000), (31000, 14000), (24000, -4000), (31000, -18000), (-31000, -16000)]
RAD = [8000, 6000, 5000, 6000, 8000]
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [gap2(2000, 2900, 2200, W_, name='serac gap'), canyon_jump(1800, 2600, 2200, W_, mat='kick', name='CREVASSE')]),
    (LG[1], [table2(1300, 1900, 1800, W_, mat='kick', name='shelf')]),
    (LG[2], [spine(1500, 2100, 300, W_, shape='straight', name='nub')]),
    (LG[3], [bridge(5600, name='ICE BRIDGE', mat='kick'), gap2(2600, 3600, 2500, W_ + 1000, name='GLACIER GAP')]),
    (LG[4], [marker('tunnel', 10000, name='ICE CAVE', tk='ice', th=3400)]),
], margin=1300, items=4)
F += boosted(corner_berms(C, RAD, 6600, 800))
T = TRACKS['glacier'] = dict(
    slot='mini7', title='GLACIER GAP', arena=(-39000, -26000, 39000, 28000, 9000), cell=2100,
    theme=theme_of('glacier', rockcol=(150, 180, 210), pillarcol=(120, 150, 190), railcol=(220, 240, 255), crystalcol=(120, 200, 245), pinecol=(40, 90, 80)),
    route=R, features=F, road_w=6200, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 12000, 44000, 0, 0, 0), cliffs=True, nresp=10)
cv, ib = find(F, 'CREVASSE'), find(F, 'ICE BRIDGE')
A = fpos(cv, cv['gap'] / 2); B = fpos(ib, 0)
nA, nB = na(cv, out_sign(cv)), na(ib, out_sign(ib))
pts = [(A[0] + nA[0] * 5000, A[1] + nA[1] * 5000, 1700), A + (2000,), lerp(A, B, 0.35, -3000) + (2600,), lerp(A, B, 0.7, 2500) + (2400,), B + (2100,),
       (B[0] + nB[0] * 4500, B[1] + nB[1] * 4500, 1700)]
T['zones'] = [oval('ice', -8000, 2000, 9000, 5000), oval('ice', 12000, 0, 6000, 4500), band('pit', pts, depth=2600, ramp=900)]
finish(T, natural=hills(hill(-24000, -6000, 4000, 5000, 800)),
       props=[('crystal', 26, 2600, 101, 1800, (None, 'ice')), ('pine', 14, 3200, 102, 1800, (None,)), ('rock', 14, 1000, 103, 1500, (None,)),
              ('snowman', 3, 900, 104, 3000, (None,))])
T['theme']['snow'] = True

# =====================================================================================================
# ALPINE SUMMIT (Summit Cup, longest: about a lap in two minutes): a mountain road: the long climb from the valley
# (boost pads, springs), the summit, then switchbacks down through the pines with ski jumps on the slope
# =====================================================================================================
C = [(-56000, -32000), (56000, -32000), (56000, 26000), (20000, 10000), (0, 30000), (-24000, 10000), (-56000, 28000)]
RAD = [9000, 9000, 8000, 7000, 7000, 7000, 8000]
EL = [0, 1700, 3300, 2600, 1900, 1100, 400]
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [table2(1300, 2000, 2400, W_, name='valley table'), spring(85, name='valley spring'), gap2(1700, 2400, 1900, W_, mat='mound', name='meadow gap'),
             rainbow(1500, 2200, 1000, W_, mat='mound', name='cow hill'), spring(85, name='barn spring'), spine(1800, 2600, 400, W_, name='hay spine')]),
    (LG[1], [gap2(1800, 2600, 2000, W_, name='pass gap'), spring(90, name='pass spring'), rainbow(1600, 2300, 1100, W_, name='col')]),
    (LG[2], [table2(1600, 2400, 2800, W_, mat='kick', name='SKI JUMP'), spine(1700, 2400, 300, W_, mat='mound', name='mogul')]),
    (LG[3], [gap2(2000, 2800, 2200, W_, name='SUMMIT GAP')]),
    (LG[4], [table2(1500, 2300, 2600, W_, mat='kick', name='BIG SKI JUMP'), rainbow(1500, 2100, 1000, W_, mat='mound', name='chalet hop')]),
    (LG[5], [gap2(1800, 2600, 2000, W_, mat='mound', name='pine gap'), spine(1600, 2300, 300, W_, name='cairn')]),
    (LG[6], [gap2(1700, 2400, 1900, W_, mat='mound', name='valley gap'), marker('tunnel', 9000, name='AVALANCHE TUNNEL', tk='rock', th=3400),
             rainbow(1500, 2100, 900, W_, mat='mound', name='last hop')]),
], margin=1200, items=2)
F += boosted(corner_berms(C, RAD, 6400, 800))
T = TRACKS['alpine'] = dict(
    slot='mini7', title='ALPINE SUMMIT', arena=(-64000, -40000, 64000, 38000, 12000), cell=2600, wall_h=6000,
    theme=theme_of('alpine', pinecol=(36, 96, 56), rockcol=(130, 126, 120), springcol=(200, 30, 40)),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[0], 1200), cam=(0, 30000, 70000, 0, 1000, 0), nresp=14, key_salt=5)
T['zones'] = [oval('snow', 30000, 30000, 7000, 4000), oval('grass', -20000, -14000, 12000, 6000), oval('grass', 22000, -14000, 12000, 6000)]
finish(T, natural=hills(hill(0, -10000, 50000, 14000, 2600), hill(12000, 26000, 6000, 5000, 2200), hill(-12000, 26000, 6000, 5000, 1600)),
       grade=(C, EL),
       props=[('pine', 90, 4000, 111, 1300, (None, 'snow', 'grass')), ('chalet', 5, 5000, 112, 4000, (None, 'grass')), ('rock', 20, 1400, 113, 1500, (None, 'snow')),
              ('bale', 10, 700, 114, 1500, ('grass',)), ('flag', 10, 2200, 115, 2000, (None,))])
T['theme']['snow'] = False

# =====================================================================================================
# MEADOW MOTOPLEX keeps its layout (Kicker Cup opener): trees, hay bales and a barn fill its middle (props only: the
# collision gains trunks, the BOL and so its route / coin key stay the same)
# =====================================================================================================
M_ = TRACKS['meadow']
M_['props'] = []
scatter(M_, 'pine', 24, 3600, 121, 1500); scatter(M_, 'shrub', 26, 700, 122, 1300); scatter(M_, 'bale', 14, 700, 123, 1500)
M_['props'].append((-14000, 0, 'chalet', 5000))
