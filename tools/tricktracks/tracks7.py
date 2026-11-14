"""Layouts for build7.py: five original Trick Style tracks (world units; karts reach ~55 a frame and fly ~2500 off a
1550-high kicker in ~60 frames, measured 2026-11-10 on the stadium). Each track: slot archive, title, theme, arena
(x0, z0, x1, z1, corner radius), route (the computers' loop, also the main road), features (strips: kickers, landings,
tables, rhythm lanes, step-ups; berms), items, coins, start (x, z, heading in degrees: 0 = +x, 90 = +z)."""
import math

# ---------------- path helpers ----------------
def rounded(corners, radii):
    """closed polygon -> dense points: straight legs joined by tangent arcs (radius per corner). Also returns each
    leg's straight part (start point, end point, heading) for placing features."""
    n = len(corners); pts = []; legs = []
    arcs = []
    for i in range(n):
        A, B, C = corners[i - 1], corners[i], corners[(i + 1) % n]
        d1 = norm((B[0] - A[0], B[1] - A[1])); d2 = norm((C[0] - B[0], C[1] - B[1]))
        cr = d1[0] * d2[1] - d1[1] * d2[0]; dot = max(-1, min(1, d1[0] * d2[0] + d1[1] * d2[1]))
        th = math.acos(dot); r = radii[i] if isinstance(radii, (list, tuple)) else radii
        t = r * math.tan(th / 2)
        s = (B[0] - d1[0] * t, B[1] - d1[1] * t); e = (B[0] + d2[0] * t, B[1] + d2[1] * t)
        sg = 1 if cr > 0 else -1
        c = (s[0] - d1[1] * r * sg, s[1] + d1[0] * r * sg)
        a0 = math.atan2(s[1] - c[1], s[0] - c[0]); a1 = math.atan2(e[1] - c[1], e[0] - c[0])
        da = a1 - a0
        while sg > 0 and da < 0: da += 2 * math.pi
        while sg < 0 and da > 0: da -= 2 * math.pi
        k = max(2, int(abs(da) * r / 1200))
        arcs.append((s, e, [(c[0] + r * math.cos(a0 + da * j / k), c[1] + r * math.sin(a0 + da * j / k)) for j in range(k + 1)]))
    for i in range(n):
        s, e, ap = arcs[i]
        pts += ap
        ns = arcs[(i + 1) % n][0]
        L = math.hypot(ns[0] - e[0], ns[1] - e[1]); k = max(1, int(L // 1500))
        pts += [(e[0] + (ns[0] - e[0]) * j / k, e[1] + (ns[1] - e[1]) * j / k) for j in range(1, k)]
        legs.append((e, ns, math.degrees(math.atan2(ns[1] - e[1], ns[0] - e[0]))))
    return pts, legs
def norm(v):
    L = math.hypot(*v) or 1.0
    return (v[0] / L, v[1] / L)
def at(leg, s):
    """point s along a leg's straight part (negative: from its end)"""
    (a, b, ang) = leg; L = math.hypot(b[0] - a[0], b[1] - a[1])
    if s < 0: s += L
    return (a[0] + (b[0] - a[0]) * s / L, a[1] + (b[1] - a[1]) * s / L)
def leglen(leg): return math.hypot(leg[1][0] - leg[0][0], leg[1][1] - leg[0][1])
def hill(cx, cz, rx, rz, h):
    def f(x, z):
        d = math.hypot((x - cx) / rx, (z - cz) / rz)
        return h * math.cos(d * math.pi / 2) ** 2 if d < 1 else 0.0
    return f
def hills(*fs): return lambda x, z: sum(f(x, z) for f in fs)

# ---------------- features (u along the travel direction, 0 at the feature's reference point) ----------------
def kicker_gap(c, ang, w=5000, h=1550, run=1850, gap=1500, hl=None, deck=500, runl=4200, back=280, backl=900, mat='kick', name='gap'):
    """FMX: a kicker (boost on its upper part) and a separate dirt landing: knuckle deck then a long downslope"""
    hl = hl or h * 0.72
    return dict(t='strip', c=c, ang=ang, w=w, name=name, pieces=[
        dict(p=[(-run, 0), (-run * 0.45, h * 0.55), (0, h), (back, 0)], mat=mat, boost=[1]),
        dict(p=[(gap - backl, 0), (gap, hl), (gap + deck, hl), (gap + deck + runl, 0)], mat='mound')])
def table(c, ang, w=5000, h=900, run=1500, top=1800, runl=2200, mat='mound', boost=False, name='table'):
    return dict(t='strip', c=c, ang=ang, w=w, name=name, pieces=[
        dict(p=[(-top / 2 - run, 0), (-top / 2 - run * 0.45, h * 0.55), (-top / 2, h), (top / 2, h), (top / 2 + runl, 0)], mat=mat, boost=[1] if boost else [])])
def humps(c, ang, n, h, s, w=5000, top=0, name='rhythm', mat='mound'):
    """n rollers / rhythm humps, s apart (centre to centre), optional flat tops"""
    p = []
    u0 = -n * s / 2
    for i in range(n):
        a = u0 + i * s
        if not p: p.append((a, 0))
        p += [(a + s / 2 - top / 2, h)] + ([(a + s / 2 + top / 2, h)] if top else []) + [(a + s, 0)]
    return dict(t='strip', c=c, ang=ang, w=w, name=name, pieces=[dict(p=p, mat=mat)])
def stepup(c, ang, w=5000, h1=1100, h2=1500, run=1500, gap=1300, face=1000, deck=3200, runl=3000, mat='kick', name='stepup'):
    """kick up onto a higher deck (the deck's face rideable if short), long deck, roll down"""
    return dict(t='strip', c=c, ang=ang, w=w, name=name, pieces=[
        dict(p=[(-run, 0), (-run * 0.45, h1 * 0.55), (0, h1), (260, 0)], mat=mat, boost=[1]),
        dict(p=[(gap - face, 0), (gap, h2), (gap + deck, h2), (gap + deck + runl, 0)], mat='mound')])
def mesa(c, ang, w=6000, h=1500, up=6500, deck=3500, drop=300, gap=1400, hl=900, runl=3600, name='mesa'):
    """a long climb onto a plateau, a drop off its edge onto a landing below (step-down)"""
    return dict(t='strip', c=c, ang=ang, w=w, name=name, pieces=[
        dict(p=[(-deck - up, 0), (-deck, h), (-deck * 0.25, h), (0, h), (drop, 0)], mat='mound', boost=[]),
        dict(p=[(gap - 700, 0), (gap, hl), (gap + 400, hl), (gap + 400 + runl, 0)], mat='mound')])
def berm(c, r, a0, a1, w=5600, h=650, name='berm'):
    """banked turn: outer half of the road rises to h at its outer edge (r = the road's centre radius)"""
    return dict(t='berm', c=c, r0=r, r1=r + w / 2 + 300, a0=a0, a1=a1, h=h, name=name)
def boost_pad(c, ang, l=1400, w=4400, name='pad'):
    """a flat speed-boost pad across the road (no ramp)"""
    return dict(t='strip', c=c, ang=ang, w=w, name=name, pieces=[], pads=[(-l / 2, l / 2)])
def items_across(p, ang, n=4): return (p[0], p[1], ang, n)
def coins_along(leg, s0, s1, n):
    """2026-11-13: at most one coin per 150 of run (a short gap between jumps packed 3 or 5 coins into 30-60: stacked)"""
    a, b = at(leg, s0), at(leg, s1)
    L = math.hypot(b[0] - a[0], b[1] - a[1]) if s1 > s0 else 0
    return (a, b, max(1, min(n, int(L // 150) + 1)))

# ---------------- the two-way jump kit (2026-11-10 redesign: fast, big air, no small bumps) ----------------
# Every jump rides the same both ways (the arena is open: karts and the computers come at them from either side),
# has a boost strip on the top of each face, and is wider than the road. Profiles run along u (0 the centre);
# 'arc' gives coins over the flight (u0, y0, u1, y1, bump, n: heights above the track's base).
def _face(run, h, shape):
    """an up-face from (-run, 0) to the lip (0, h): 'kick' concave (steepest at the lip), 'straight', 'steep' (short)"""
    if shape == 'straight': return [(-run, 0), (-run * 0.5, h * 0.5), (0, h)]
    return [(-run, 0), (-run * 0.62, h * 0.18), (-run * 0.3, h * 0.5), (0, h)]
def _mirror(pts): return [(-u, h) for u, h in reversed(pts)]
def _shift(pts, du): return [(u + du, h) for u, h in pts]
def _boost_segs(pr, h, lo=0.45):
    """the faces' top segments (both ends at least lo * h, not level): boost"""
    return [i for i in range(len(pr) - 1) if min(pr[i][1], pr[i + 1][1]) >= lo * h - 1 and abs(pr[i + 1][1] - pr[i][1]) > 1]
def _jump(kind, pr, h, w, mat, arc, name, lo=0.45):
    return dict(t='strip', c=(0, 0), ang=0, w=w, name=name, kind=kind, h=h, arc=arc,
                pieces=[dict(p=pr, mat=mat, boost=_boost_segs(pr, h, lo))])
def gap2(h=1800, run=2600, gap=None, w=6400, mat='kick', shape='kick', name='gap'):
    """two kickers back to back with no gap between them (2026-11-10: the gaps were closed up): launch off the shared
    lip, land on the far face. gap is kept for the layouts' calls and ignored."""
    a = _shift(_face(run, h, shape), -100)
    return _jump('gap', a + _mirror(a), h, w, mat, (-100, h + 350, run * 0.5, h * 0.5 + 150, 600, 6), name)
def spine(h=2000, run=3000, top=400, w=6400, mat='kick', shape='kick', name='spine'):
    """a pyramid: up one face, over a short top, land on the other"""
    a = _shift(_face(run, h, shape), -top / 2)
    return _jump('spine', a + _mirror(a), h, w, mat, (-top / 2 - run * 0.1, h * 0.92 + 300, run * 0.55, h * 0.45 + 150, 700, 6), name)
def table2(h=1400, run=2200, top=2600, w=6400, mat='mound', shape='straight', name='table'):
    """was a tabletop; now a jump in its place (2026-11-10): straight faces to a sharp lip, 25 % taller, as long"""
    run = run + top / 2
    a = _face(run, h * 1.25, shape)
    return _jump('kicker', a + _mirror(a), h * 1.25, w, mat, (0, h * 1.25 + 300, run * 0.5, h * 0.6 + 150, 650, 6), name)
def rainbow(h=1700, run=2600, top=1400, w=6400, mat='mound', name='rainbow'):
    """a rounded crest: kick faces up to 85 %, an arc over the top"""
    a = _shift(_face(run, h * 0.85, 'kick'), -top / 2)
    crest = [(-top / 2 + top * i / 6, h * (0.85 + 0.15 * math.sin(math.pi * i / 6))) for i in range(1, 6)]
    return _jump('rainbow', a + crest + _mirror(a), h, w, mat, (-top / 2, h + 300, top / 2 + run * 0.5, h * 0.45 + 150, 900, 7), name, lo=0.4)
def _extent(F):
    us = [p[0] for pc in F['pieces'] for p in pc['p']]
    return min(us), max(us)
def line_up(leg, feats, margin=1500, items=4):
    """features spread evenly along a leg's straight; in each gap between them (and at the ends) a boost pad and a row
    of item boxes, coins along the longer gaps; coin arcs over each jump. Returns (features, items, coins)."""
    L = leglen(leg); ang = leg[2]
    ext = [_extent(F) for F in feats]
    free = L - 2 * margin - sum(b - a for a, b in ext)
    assert free > 0, ('leg too short', L, [F['name'] for F in feats])
    g = free / (len(feats) + 1)
    out, its, cs = [], [], []
    s = margin
    gaps = [(s, s + g)]
    for F, (a, b) in zip(feats, ext):
        s += g
        F = dict(F); F['c'] = at(leg, s - a); F['ang'] = ang
        u0, y0, u1, y1, bump, n = F.pop('arc')
        F['coinarc'] = (u0, y0, u1, y1, bump, n)
        out.append(F)
        s += b - a
        gaps.append((s, s + g))
    a_, b_ = math.radians(ang), None
    dx, dz = math.cos(a_), math.sin(a_)
    for i, (g0, g1) in enumerate(gaps):
        gl = g1 - g0
        mid = (g0 + g1) / 2
        if gl >= 2600:                                   # a pad by each jump that this gap leads to or from
            if i < len(feats): out.append(boost_pad(at(leg, g1 - 900), ang, l=1200, name='pad'))
            if i > 0: out.append(boost_pad(at(leg, g0 + 900), ang, l=1200, name='pad'))
        elif gl >= 1000: out.append(boost_pad(at(leg, mid), ang, l=min(1200, gl - 300), name='pad'))   # one in a short gap
        its.append(items_across(at(leg, mid), ang, items + 2))   # 2026-11-12: coins and items very common
        if gl >= 4200: cs.append(coins_along(leg, g0 + 1300, mid - 450, 5)); cs.append(coins_along(leg, mid + 450, g1 - 1300, 5))
        elif gl >= 2000: cs.append(coins_along(leg, g0 + 500, mid - 450, 3)); cs.append(coins_along(leg, mid + 450, g1 - 500, 3))
    for F in out:
        if 'coinarc' not in F: continue
        u0, y0, u1, y1, bump, n = F.pop('coinarc')
        cx, cz = F['c']
        cs.append(((cx + dx * u0, cz + dz * u0), (cx + dx * u1, cz + dz * u1), n + 3, y0, y1, bump))
    return out, its, cs
def course(legs_feats, margin=1500, items=4):
    F, I, Cn = [], [], []
    for leg, feats in legs_feats:
        f, i, c = line_up(leg, feats, margin, items); F += f; I += i; Cn += c
    return F, I, Cn
def corner_berms(C, radii, w, h=600):
    """berms on every bend of 30 degrees or more: the outside of each rounded corner"""
    out = []
    n = len(C)
    for i in range(n):
        A, B, Cc = C[i - 1], C[i], C[(i + 1) % n]
        d1 = norm((B[0] - A[0], B[1] - A[1])); d2 = norm((Cc[0] - B[0], Cc[1] - B[1]))
        cr = d1[0] * d2[1] - d1[1] * d2[0]; th = math.acos(max(-1, min(1, d1[0] * d2[0] + d1[1] * d2[1])))
        if th < math.radians(30): continue
        r = radii[i]; t = r * math.tan(th / 2); s = (B[0] - d1[0] * t, B[1] - d1[1] * t); sg = 1 if cr > 0 else -1
        c = (s[0] - d1[1] * r * sg, s[1] + d1[0] * r * sg)
        e = (B[0] + d2[0] * t, B[1] + d2[1] * t)
        a0 = math.degrees(math.atan2(s[1] - c[1], s[0] - c[0])); a1 = math.degrees(math.atan2(e[1] - c[1], e[0] - c[0]))
        da = a1 - a0
        while sg > 0 and da < 0: da += 360
        while sg < 0 and da > 0: da -= 360
        out.append(berm(c, r, a0, a0 + da, w=w, h=h, name='bend %d' % i))
    return out
def flat_road(height, route, half):
    """terrain flattened to 0 within half of the route, rising back over 3000 (jumps and berms stand on level ground)"""
    segs_ = [(route[i], route[(i + 1) % len(route)]) for i in range(len(route))]
    def dist(x, z):
        best = 1e18
        for (ax, az), (bx, bz) in segs_:
            vx, vz = bx - ax, bz - az; L2 = vx * vx + vz * vz or 1.0
            t = max(0.0, min(1.0, ((x - ax) * vx + (z - az) * vz) / L2))
            best = min(best, (x - ax - vx * t) ** 2 + (z - az - vz * t) ** 2)
        return math.sqrt(best)
    def f(x, z):
        h = height(x, z)
        if h == 0: return 0.0
        t = max(0.0, min(1.0, (dist(x, z) - half) / 3000.0))
        return h * t * t * (3 - 2 * t)
    return f

TRACKS = {}

# =====================================================================================================
# 0. TRICK STADIUM (Cookie Land slot, Mini7): the showcase: a big stadium oval, every kind of jump, banked ends
# =====================================================================================================
C = [(-30000, 15000), (30000, 15000), (30000, -15000), (-30000, -15000)]
RAD = [8000] * 4
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [gap2(1800, 2600, 2000, W_, name='opener'), spine(2200, 3200, 500, W_, name='pyramid'),
             table2(1400, 2200, 2600, W_, mat='kick', name='table'), gap2(2400, 3200, 2400, W_ + 800, name='MEGA GAP')]),
    (LG[1], [rainbow(1700, 2600, 1400, W_, mat='kick', name='rainbow east')]),
    (LG[2], [table2(1300, 2000, 3000, W_, mat='kick', name='long table'), gap2(2000, 2800, 2200, W_, shape='straight', name='double'),
             spine(1900, 2800, 300, W_, shape='straight', name='peak'), gap2(1700, 2500, 1900, W_, name='closer')]),
    (LG[3], [gap2(1600, 2200, 1900, W_, name='west gap')]),
])
F += corner_berms(C, RAD, 6400, 700)
TRACKS['stadium'] = dict(
    slot='mini7', title='TRICK STADIUM', arena=(-38000, -23000, 38000, 23000, 8000), cell=2600,
    theme=dict(ground=((84, 150, 70), (60, 118, 52)), pebble=[(100, 170, 80), (70, 130, 60)], pebbles=60, grass=[(110, 180, 90), (70, 130, 60)],
               road='clay', roadcol=(196, 120, 70), berm=(170, 100, 60), mound=(190, 118, 70), kick='steel', kickcol=(70, 130, 220),
               bar=((245, 245, 245), (30, 90, 210)), bartext='TRICK STADIUM', decor='stands', track_fence='barrier', fence_h=300,
               sky=((60, 130, 230), (210, 230, 255)), cloud=(255, 255, 255)),
    route=R, features=F, road_w=6000, berm=500, fence_off=3500, items=I, coins=Cn,
    start=(at(LG[0], 1200)[0], at(LG[0], 1200)[1], LG[0][2]), cam=(0, 24000, 34000, 0, 0, 0))

# =====================================================================================================
# 1. DUST DEVIL MESA (GameCube slot, Mini2): desert freeride loop: wooden gaps, big dirt spines, a mega gap
# =====================================================================================================
C = [(-30000, 18000), (27000, 18000), (31000, -15000), (-5000, -19000), (-31000, -6000)]
RAD = [7000, 7000, 7000, 6000, 6000]
R, LG = rounded(C, RAD)
W_ = 6600
F, I, Cn = course([
    (LG[0], [gap2(1700, 2500, 2000, W_, name='east gap'), spine(2000, 3000, 400, W_, mat='mound', name='dune spine'),
             gap2(2300, 3100, 2400, W_ + 800, name='MESA GAP'), table2(1300, 2100, 2400, W_, name='east table')]),
    (LG[1], [spine(1800, 2800, 600, W_, shape='straight', name='south spine'), gap2(1600, 2400, 1900, W_, mat='mound', name='south gap')]),
    (LG[2], [table2(1500, 2400, 3000, W_, name='mesa top'), gap2(2400, 3200, 2400, W_ + 800, mat='mound', name='canyon gap')]),
    (LG[3], [gap2(1800, 2600, 2100, W_, shape='straight', name='north gap'), rainbow(2000, 3000, 1400, W_, name='dune')]),
    (LG[4], [spine(1700, 2600, 300, W_, name='west spine')]),
])
F += corner_berms(C, RAD, 6200, 650)
TRACKS['dustdevil'] = dict(
    slot='mini2', title='DUST DEVIL MESA', arena=(-38000, -27000, 38000, 27000, 9000), cell=2600,
    height=hills(hill(-15000, 6000, 7500, 6000, 1100), hill(22000, 4000, 5000, 6500, 900), hill(-26000, -19000, 6000, 4000, 700)),
    theme=dict(ground=((214, 162, 104), (168, 118, 70)), pebble=[(230, 190, 140), (140, 96, 56), (200, 150, 96)], road='clay', roadcol=(196, 132, 80),
               berm=(170, 112, 66), mound=(186, 126, 76), kick='wood', bar=((240, 240, 240), (205, 90, 28)), decor='fence', fencecol=(110, 76, 44),
               sky=((70, 130, 210), (250, 214, 160)), cloud=(255, 240, 220), sil=('mesa', (176, 98, 64))),
    route=R, features=F, road_w=6000, items=I, coins=Cn,
    start=(at(LG[0], 1000)[0], at(LG[0], 1000)[1], LG[0][2]), cam=(0, 9000, 46000, 0, 0, 0))

# =====================================================================================================
# 2. RHYTHM RIDGE SX (Block City slot, Mini3): night supercross: big doubles, tabletops and spines, bermed 180s
# =====================================================================================================
C = [(-31000, 16000), (28000, 16000), (28000, 6000), (-14000, 6000), (-14000, -4000), (28000, -4000), (28000, -16000), (-31000, -16000)]
RAD = [4500, 5000, 5000, 5000, 5000, 6000, 6000, 4500]
R, LG = rounded(C, RAD)
L0, L2, L4, L6, L7 = LG[0], LG[2], LG[4], LG[6], LG[7]
W_ = 5600
F, I, Cn = course([
    (L0, [table2(1400, 2200, 2600, W_, name='start table'), gap2(1900, 2800, 2100, W_, mat='mound', name='big double 1'),
          spine(2000, 3000, 400, W_, mat='mound', name='spine'), gap2(1700, 2600, 1900, W_, mat='mound', name='double 2')]),
    (L2, [gap2(2200, 3000, 2300, W_, mat='mound', name='triple'), rainbow(1700, 2600, 1400, W_, name='rainbow')]),
    (L4, [spine(1800, 2700, 300, W_, mat='mound', shape='straight', name='peak'), gap2(1800, 2600, 2000, W_, mat='mound', name='double 3')]),
    (L6, [gap2(1700, 2500, 1900, W_, mat='mound', name='double 4'), table2(1500, 2300, 3000, W_, mat='kick', name='finish table'),
          gap2(2300, 3100, 2400, W_, mat='mound', name='BIG DOUBLE'), spine(1700, 2600, 400, W_, mat='mound', name='spine 2')]),
    (L7, [gap2(1700, 2500, 1900, W_, mat='mound', name='west double'), spine(1600, 2500, 300, W_, mat='mound', name='west spine')]),
], margin=1300, items=3)
F += [berm((28000 - 5000, 11000), 5000, 90, -90, name='berm 1'), berm((-14000 + 5000, 1000), 5000, 90, 270, name='berm 2'),
      berm((28000 - 6000, -10000), 6000, 90, -90, w=5000, name='berm 3')]
TRACKS['rhythmridge'] = dict(
    slot='mini3', title='RHYTHM RIDGE SX', arena=(-37000, -23000, 36000, 23000, 7000), cell=2600,
    theme=dict(ground=((92, 70, 52), (62, 46, 34)), pebble=[(120, 96, 72), (50, 38, 28)], road='clay', roadcol=(150, 104, 66), berm=(96, 68, 44),
               mound=(140, 98, 62), kick='wood', bar=((235, 235, 235), (30, 90, 200)), bartext='RHYTHM RIDGE', decor='stands', track_fence='barrier', fence_h=300,
               sky=((8, 12, 34), (40, 46, 90)), stars=True, nofog=True),
    route=R, features=F, road_w=5000, berm=500, fence_off=2850, items=I, coins=Cn,
    start=(at(L0, 1000)[0], at(L0, 1000)[1], L0[2]), cam=(-2000, 26000, 30000, -2000, 0, 0))

# =====================================================================================================
# 3. HARBOR HIGHWAY (Pipe Plaza slot, Mini8): sunset street circuit: steel ramps on every straight, an overpass table
# =====================================================================================================
C = [(-32000, 15000), (2000, 19000), (30000, 13000), (34000, -3000), (22000, -16000), (4000, -9000), (-12000, -19000), (-31000, -15000)]
RAD = [7000, 9000, 7000, 7000, 7000, 7000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6200
F, I, Cn = course([
    (LG[0], [gap2(1700, 2500, 2000, W_, name='harbor gap'), table2(1300, 2000, 2600, W_, mat='kick', name='dock table')]),
    (LG[1], [spine(1900, 2800, 600, W_, name='bridge spine'), gap2(1600, 2300, 1900, W_, shape='straight', name='side gap')]),
    (LG[2], [spine(1500, 2200, 300, W_, shape='straight', name='kicker pair')]),
    (LG[3], [gap2(1500, 2000, 1800, W_, name='bus gap')]),
    (LG[4], [table2(1200, 1900, 2400, W_, mat='kick', name='launch table')]),
    (LG[5], [rainbow(1600, 2400, 1200, W_, mat='kick', name='flyover')]),
    (LG[6], [gap2(1700, 2300, 2000, W_, name='dock gap')]),
    (LG[7], [table2(1400, 2300, 3200, W_, mat='kick', name='OVERPASS'), spine(1700, 2400, 300, W_, name='west spine')]),
], margin=1100, items=3)
TRACKS['harbor'] = dict(
    slot='mini8', title='HARBOR HIGHWAY', arena=(-40000, -25000, 40000, 25000, 8000), cell=2600,
    theme=dict(ground=((128, 128, 132), (96, 96, 102)), pebble=[(150, 150, 150), (80, 80, 84)], pebbles=60, road='asphalt', roadcol=(70, 72, 78),
               berm=(110, 110, 114), mound=(150, 150, 155), kick='steel', kickcol=(150, 156, 170), bar=((238, 238, 238), (220, 180, 30)),
               decor='city', track_fence='barrier', fence_h=360, concrete=(150, 146, 140),
               sky=((70, 60, 130), (255, 150, 90)), cloud=(255, 190, 150), sil=('skyline', (60, 50, 80))),
    route=R, features=F, road_w=5600, berm=500, fence_off=3200, items=I, coins=Cn,
    start=(at(LG[0], 900)[0], at(LG[0], 900)[1], LG[0][2]), cam=(-6000, 12000, 21000, 2000, 1000, -4000))

def containers(route, arena, clear, seed, step=3600):
    """stacks of shipping containers wherever the ground is at least clear from the route (visual scenery)"""
    import random
    rr = random.Random(seed); out = []
    cols = [(0.85, 0.25, 0.2), (0.2, 0.45, 0.8), (0.25, 0.6, 0.3), (0.9, 0.6, 0.15), (0.6, 0.6, 0.62), (0.75, 0.2, 0.5)]
    x0, z0, x1, z1, _ = arena
    x = x0 + 2500
    while x < x1 - 2500:
        z = z0 + 2500
        while z < z1 - 2500:
            if min(math.hypot(x - p[0], z - p[1]) for p in route) > clear and rr.random() < 0.55:
                c = rr.choice(cols); h = 520 * rr.randint(1, 3)
                if rr.random() < 0.5: out.append((x, z, 1250, 520, h, c))
                else: out.append((x, z, 520, 1250, h, c))
            z += step
        x += step
    return out
TRACKS['harbor']['boxes'] = containers(TRACKS['harbor']['route'], TRACKS['harbor']['arena'], 6500, 3)

# =====================================================================================================
# 4. FLOWLINE CANYON (Tilt-a-Kart slot, Mini5): flow trail through green hills: rainbows and tables, bermed bends
# =====================================================================================================
C = [(-32000, 14000), (-8000, 19000), (12000, 9000), (31000, 16000), (33000, -6000), (14000, -17000), (-6000, -8000), (-30000, -18000)]
RAD = [6000, 7000, 7000, 6000, 7000, 7000, 7000, 6000]
R, LG = rounded(C, RAD)
W_ = 6200
F, I, Cn = course([
    (LG[0], [rainbow(1500, 2100, 1200, W_, name='flow 1'), spine(1600, 2200, 300, W_, mat='mound', name='flow 2')]),
    (LG[1], [table2(1300, 1900, 2000, W_, name='flow table'), gap2(1700, 2200, 1900, W_, mat='mound', name='flow gap')]),
    (LG[2], [rainbow(1600, 2300, 1000, W_, name='crest')]),
    (LG[3], [spine(1700, 2400, 400, W_, mat='mound', name='hill spine')]),
    (LG[4], [gap2(2000, 2700, 2200, W_, mat='mound', name='canyon gap')]),
    (LG[5], [table2(1400, 2000, 2400, W_, mat='kick', name='kicker table'), spine(1600, 2100, 300, W_, mat='mound', name='flow 3')]),
    (LG[6], [gap2(1700, 2300, 1900, W_, mat='mound', shape='straight', name='step gap'), rainbow(1400, 1900, 900, W_, name='flow 4')]),
    (LG[7], [rainbow(1700, 2400, 1300, W_, name='big flow'), gap2(1900, 2600, 2100, W_, mat='mound', name='FLOW GAP')]),
], margin=1100, items=3)
F += corner_berms(C, RAD, 6000, 650)
TRACKS['flowline'] = dict(
    slot='mini5', title='FLOWLINE CANYON', arena=(-40000, -26000, 40000, 26000, 9000), cell=2600,
    height=hills(hill(-14000, 2000, 9000, 6500, 1400), hill(20000, 2000, 7000, 5500, 1100), hill(-34000, -3000, 4500, 7000, 900), hill(4000, -22000, 8000, 3500, 700)),
    theme=dict(ground=((104, 150, 70), (66, 108, 46)), pebble=[(120, 160, 80), (90, 80, 60)], grass=[(130, 180, 90), (70, 120, 50)], road='clay',
               roadcol=(176, 122, 78), berm=(120, 96, 60), mound=(170, 116, 72), kick='wood', bar=((240, 240, 240), (60, 140, 60)), decor='fence',
               fencecol=(110, 80, 52), sky=((60, 130, 220), (200, 230, 250)), sil=('hills', (70, 120, 80))),
    route=R, features=F, road_w=5800, items=I, coins=Cn,
    start=(at(LG[0], 800)[0], at(LG[0], 800)[1], LG[0][2]), cam=(0, 12000, 44000, 0, 0, 0))

# =====================================================================================================
# 5. FROSTBITE BIG AIR (Luigi's Mansion slot, Mini1): snow park: the tallest ice kickers, a mega gap, a big spine
# =====================================================================================================
C = [(-30000, 16000), (28000, 16000), (30000, -14000), (-28000, -16000)]
RAD = [9000] * 4
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [gap2(1900, 2800, 2100, W_, name='slope 1'), gap2(2300, 3200, 2400, W_, name='slope 2'), spine(2600, 3600, 500, W_, name='ICE PYRAMID')]),
    (LG[1], [table2(1500, 2200, 2000, W_, mat='kick', name='east table')]),
    (LG[2], [gap2(2800, 4000, 2600, W_ + 1200, name='MEGA KICKER'), spine(2000, 3000, 300, W_, shape='straight', name='north spine'),
             gap2(1800, 2600, 2000, W_, shape='straight', name='north gap')]),
    (LG[3], [rainbow(1800, 2800, 1400, W_, mat='kick', name='west rainbow')]),
])
F += corner_berms(C, RAD, 6600, 700)
TRACKS['frostbite'] = dict(
    slot='mini1', title='FROSTBITE BIG AIR', arena=(-38000, -26000, 38000, 26000, 9000), cell=2600,
    height=hills(hill(-24000, 2000, 5500, 7000, 1000), hill(24000, 1000, 5000, 6000, 900), hill(0, 21500, 9000, 2500, 600)),
    theme=dict(ground=((236, 242, 250), (196, 210, 228)), pebble=[(250, 252, 255), (180, 196, 216)], pebbles=90, road='clay', roadcol=(214, 224, 238),
               berm=(200, 212, 230), mound=(226, 234, 246), snow=True, kick='ice', kickcol=(110, 190, 240), bar=((245, 245, 250), (40, 120, 220)),
               decor='fence', fencecol=(70, 90, 130), sky=((90, 150, 220), (225, 236, 250)), sil=('mountains', (120, 140, 175))),
    route=R, features=F, road_w=6200, items=I, coins=Cn,
    start=(at(LG[0], 1000)[0], at(LG[0], 1000)[1], LG[0][2]), cam=(0, 12000, 44000, 0, 0, 0))

import tracks8  # noqa: E402,F401  the cups' ten more (2026-11-12)
import tracks9  # noqa: E402,F401  2026-11-12 unique tracks: twelve rebuilt with places, hazards and scenery (replace tracks8's)

# every track: the terrain level under the road (and 3000 beyond its edge)
for _T in TRACKS.values():
    if _T.get('height') and not _T.get('graded'): _T['height'] = flat_road(_T['height'], _T['route'], _T['road_w'] / 2 + 1500)
