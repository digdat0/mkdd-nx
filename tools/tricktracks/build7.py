"""Trick Style tracks, generation 7: a data-driven builder for wholly original battle arenas (layouts in tracks7.py).
Same method as build6.py (own geometry, textures, collision, course data; from the user's disc only the slot archive's
objects/ folder and the BCO flag rule learnt from Waluigi.bco), plus: features at any heading (kickers, landings,
tables, rhythm lanes, step-ups), banked turns, terrain hills, track fences, themes (dirt, sand, clay, asphalt, snow).
usage: py -3.10 tools/tricktracks/build7.py <outdir> <track> [stem] [--disc GM4E01 image] [--superbmd SuperBMD.exe] [--work DIR]
(--disc once: gamefiles.py extracts the game's files it builds on into the work folder; SuperBMD is not in the repo)
writes <outdir>/Course/MiniN.arc, <outdir>/routes/<key>.txt, <outdir>/coins/<key>.txt, work/a7_<track>/ (dae, pngs, preview)"""
import sys, os, math, copy, io, json, random, subprocess, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bcolib, mkarc, tex7, tracks7, gamefiles
from lib.libbol import BOL, Rotation
from lib.vectors import Vector3
from PIL import Image, ImageDraw

W = gamefiles.flag(sys.argv, '--work', gamefiles.WORK)
SUPERBMD = gamefiles.flag(sys.argv, '--superbmd')
_disc = gamefiles.flag(sys.argv, '--disc')
if _disc: gamefiles.fetch(_disc, W, SUPERBMD)
gamefiles.need(W, 'Waluigi.arc', 'mini/Mini7.arc', 'wal_materials.json')
SUPERBMD = gamefiles.superbmd(SUPERBMD)
OUT, TRK = sys.argv[1], sys.argv[2]
T = tracks7.TRACKS[TRK]
# 2026-11-12 cups: [stem] builds the track onto Cookie Land's slot (mini7) as Course/<stem>.arc (all 16 share it)
STEM = sys.argv[3] if len(sys.argv) > 3 else None
if STEM: T['slot'] = 'mini7'
NAME = T['slot']
FILE = NAME[0].upper() + NAME[1:] + '.arc'
OFILE = STEM + '.arc' if STEM else FILE
A7 = W + '/a7_' + TRK
os.makedirs(A7, exist_ok=True)
BASE = 1000.0
# Tilt-A-Kart's slot (MINI_5): the game always draws the course model raised by Course::mPos (0, 10000, 0), but
# tests collision in that frame only while the course shakes (CrsData shaking != 0, Course::convWorldPosToMapCoord).
# 2026-11-11: shaking is now off (BOL below), so collision and course data (starts, boxes, route, coins) are in plain
# world space (WDY 0) and the model is built MODEL_DY lower so it lands where the collision is. (Before, with
# shaking on, the whole arena tilted up to 15 degrees every 15 s.)
WDY = 0.0
MODEL_DY = -10000.0 if T['slot'] == 'mini5' else 0.0
HF = T.get('height') or (lambda x, z: 0.0)
def hy(x, z): return BASE + HF(x, z)

# ---------------- BCO flag rule (learnt from a stock collision file) ----------------
wal = mkarc.load(W + '/Waluigi.arc')
d = wal.root.files['waluigi_course.bco']; d.seek(0)
orig = bcolib.read(d.read())
def sg(v): return 0 if abs(v) < 1e-6 else (1 if v > 0 else -1)
def key(p): return tuple((sg(p[(i + 1) % 3][0] - p[i][0]), sg(p[(i + 1) % 3][2] - p[i][2])) for i in range(3))
tab = collections.defaultdict(collections.Counter)
for t in orig.tris: tab[key([orig.verts[k] for k in t['v']])][t['flag']] += 1
def flag_for(p):
    c = tab.get(key(p))
    if not c: return None
    f, nn = c.most_common(1)[0]
    return f if nn >= 20 and nn >= 0.97 * sum(c.values()) else None
dirt_mat = [m for m in orig.mats if m[0] == 1 and m[1] == 0][0]
bco = bcolib.BCO()
bco.mats = [dirt_mat, (8, 0, 255, 0, 0), [m for m in orig.mats if m[0] == 2 and m[1] == 1][0], (10, 1, 255, 0, 0)]   # 0x0100 ground, 0x0800 boost, 0x0201 wall, 0x0a01 dead zone
# 2026-11-12 unique tracks: the game's own surface kinds, (attribute, index, material, two particle words) as the stock
# courses' collision tables have them: sand Dry Dry Desert's (attribute 12: a little slower, sand spray), ice Sherbet
# Land's (4, material 6), snow its off-road (0, material 22), shallow water attribute 12 with Peach Beach's water
# material (28). Hazards (lava, deep water, a canyon's floor) are dead zones 0x0aNN: the kart falls through and Lakitu
# brings it back to respawn point NN (the BOL's, added below), as Tilt-A-Kart's
SURF = {'sand': (0x0c00, 't7_sand', (0x0c, 0, 10, 4293276260, 4293276240)), 'ice': (0x0400, 't7_ice', (4, 0, 6, 0, 0)),
        'snow': (0x0000, 't7_snow', (0, 0, 22, 0, 0)), 'water': (0x0c01, 't7_shallow', (0x0c, 1, 28, 4294919740, 4294919710)),
        'grass': (0x0100, 't7_grass', None), 'rock': (0x0100, 't7_rock', None),
        'lava': (0x0a00, 't7_lavabed', None), 'deep': (0x0a00, 't7_deepbed', None), 'pit': (0x0a00, 't7_rock', None)}
HAZ = ('lava', 'deep', 'pit')
for k_, (_t, _m, mt) in SURF.items():
    if mt and mt not in bco.mats: bco.mats.append(mt)
ZONES = T.get('zones', [])
def band_sides(pl):
    """a band's (x, z, half width) polyline -> its left and right edges"""
    Lf, Rt = [], []
    for i, (x, z, hw) in enumerate(pl):
        a, b = pl[max(0, i - 1)], pl[min(len(pl) - 1, i + 1)]; L_ = math.hypot(b[0] - a[0], b[1] - a[1]) or 1.0
        nx_, nz_ = -(b[1] - a[1]) / L_, (b[0] - a[0]) / L_
        Lf.append((x + nx_ * hw, z + nz_ * hw)); Rt.append((x - nx_ * hw, z - nz_ * hw))
    return Lf, Rt
def zone_at(x, z):
    """the last zone (tracks9 zone dicts: 'in'(x, z) test, 'kind') holding (x, z), else None"""
    k = None
    for Z in ZONES:
        if Z['in'](x, z): k = Z['kind']
    return k
HAZ_TRIS = []                                    # bco.tris indices of hazard floor (their respawn ids set with the BOL)
bco.cx = orig.cx; bco.cz = orig.cz
def vtx(p): bco.verts.append(p); return len(bco.verts) - 1
def oriented(ids, pos, want):
    for flip in (0, 1):
        q0 = list(ids) if not flip else [ids[0], ids[2], ids[1]]
        if not want(bcolib.tri_normal(*[pos[i] for i in q0])): continue
        for rot in range(3):
            q = q0[rot:] + q0[:rot]
            f = flag_for([pos[i] for i in q])
            if f is not None: return q, f
    return None
up = lambda nn: nn[1] > 0.5
def J(i, j, s=1): return ((i * 73856093 ^ j * 19349663 ^ s * 83492791) % 2001) / 1000.0 - 1.0
SEAM = collections.Counter()                     # quads whose corners are shared with their neighbours' (seed 0) or not
def SJ(pts, amp=8.0):
    """seed 0 of every jittered piece: each corner's jitter a function of where it is, so neighbouring pieces keep
    their shared edges (2026-11-11: per-piece jitter left steps and slits at every seam: karts sank into jump tops
    and caught on the boost strips)"""
    out = []
    for x, y, z in pts:
        i, j = int(round(x / 4)), int(round(z / 4))
        out.append((x + J(i, j, 1) * amp, y, z + J(i, j, 2) * amp))
    return out
class Quad(list):
    """a quad's points and how its collision split it (1: along 1-3, so the visual fan starts at point 1)"""
    split = 0
def try_quad(make, want, typ=0x100, tries=120):
    for seed in range(tries):
        ps = make(seed); pos = dict(enumerate(ps))
        for sp, tris in enumerate((((0, 1, 2), (0, 2, 3)), ((0, 1, 3), (1, 2, 3)))):
            res = [oriented(t, pos, want) for t in tris]
            if all(res):
                ids = [vtx(p) for p in ps]
                for q, f in res: bco.tris.append(dict(v=tuple(ids[k] for k in q), typ=typ, flag=f, tail=0x50000000))
                ps = Quad(ps); ps.split = sp
                SEAM['shared' if seed == 0 else 'own'] += 1
                return ps
    return None
def try_tri(make, want, typ=0x100, tries=120):
    for seed in range(tries):
        ps = make(seed); pos = dict(enumerate(ps))
        r = oriented((0, 1, 2), pos, want)
        if r:
            ids = [vtx(p) for p in ps]; q, f = r
            bco.tris.append(dict(v=tuple(ids[k] for k in q), typ=typ, flag=f, tail=0x50000000))
            SEAM['shared' if seed == 0 else 'own'] += 1
            return ps
    return None
NOCOL = []                                       # collision pieces that found no flags (reported)

# ---------------- floor: jittered grid over the arena, y from the terrain ----------------
AX0, AZ0, AX1, AZ1, AR = T['arena']
FX0, FX1, FZ0, FZ1 = AX0 - 1500, AX1 + 1500, AZ0 - 1500, AZ1 + 1500
CELL = T.get('cell', 2600.0)
NX, NZ = max(8, int(round((FX1 - FX0) / CELL))), max(6, int(round((FZ1 - FZ0) / CELL)))
CW, CD = (FX1 - FX0) / NX, (FZ1 - FZ0) / NZ
JM = min(260.0, CW * 0.1)
sys.setrecursionlimit(20000)
rng = random.Random(7)
CAND_IN = [(rng.choice((-1, 1)) * rng.uniform(JM * .25, JM), rng.choice((-1, 1)) * rng.uniform(JM * .25, JM)) for _ in range(48)]
CAND_X = [(rng.choice((-1, 1)) * rng.uniform(JM * .25, JM), rng.choice((-1, 1)) * rng.uniform(3, 6)) for _ in range(24)]
CAND_Z = [(rng.choice((-1, 1)) * rng.uniform(3, 6), rng.choice((-1, 1)) * rng.uniform(JM * .25, JM)) for _ in range(24)]
order = [(i, j) for i in range(NX + 1) for j in range(NZ + 1)]
nodepos = {}
def cell_tris(i, j):
    pos = dict(enumerate([nodepos[(i, j)], nodepos[(i + 1, j)], nodepos[(i + 1, j + 1)], nodepos[(i, j + 1)]]))
    for tris in (((0, 1, 2), (0, 2, 3)), ((0, 1, 3), (1, 2, 3))):
        res = [oriented(t, pos, up) for t in tris]
        if all(res): return res
    return None
def node_cands(k):
    i, j = order[k]
    if i in (0, NX) and j in (0, NZ): cands = [(0.0, 0.0)]
    elif j in (0, NZ): cands = CAND_X
    elif i in (0, NX): cands = CAND_Z
    else: cands = CAND_IN
    idx = list(range(len(cands))); random.Random(k).shuffle(idx)
    return [cands[ci] for ci in idx]
def place():
    """depth-first over the nodes' candidate offsets, backtracking (2026-11-12: a loop, was recursion: the big
    arenas have thousands of nodes); same order and choices as the recursive search"""
    k, left = 0, [None] * len(order)
    while k < len(order):
        i, j = order[k]
        if left[k] is None: left[k] = node_cands(k)
        x, z = FX0 + CW * i, FZ0 + CD * j
        ok = False
        while left[k]:
            ox, oz = left[k].pop(0)
            px, pz = x + ox, z + oz
            nodepos[(i, j)] = (px, hy(px, pz), pz)
            if i > 0 and j > 0 and not cell_tris(i - 1, j - 1): continue
            ok = True; break
        if ok: k += 1; continue
        left[k] = None; nodepos.pop((i, j), None); k -= 1
        if k < 0: return False
    return True
assert place(), 'floor search failed'
nid = {p: vtx(nodepos[p]) for p in order}
FLOOR = []                                      # (3 points) for floor_y and the visual floor
FLOORK = []                                     # ... and each one's zone kind (None: the ground)
BUCK = collections.defaultdict(list)
for i in range(NX):
    for j in range(NZ):
        c = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
        res = cell_tris(i, j)
        assert res, ('floor cell', i, j)
        for q, f in res:
            tri = [nodepos[c[k]] for k in q]
            zk = zone_at(sum(p[0] for p in tri) / 3, sum(p[2] for p in tri) / 3) if ZONES else None
            if zk in HAZ: HAZ_TRIS.append(len(bco.tris))
            bco.tris.append(dict(v=tuple(nid[c[k]] for k in q), typ=SURF[zk][0] if zk else 0x100, flag=f, tail=0x50000000))
            FLOOR.append(tri); BUCK[(i, j)].append(tri); FLOORK.append(zk)
print('floor', NX, 'x', NZ, 'tris', len(bco.tris))
def floor_y(x, z):
    i, j = int((x - FX0) // CW), int((z - FZ0) // CD)
    for di in (0, -1, 1):
        for dj in (0, -1, 1):
            for (a, b, c) in BUCK.get((i + di, j + dj), ()):
                d_ = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
                if abs(d_) < 1e-9: continue
                l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d_
                l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d_
                l3 = 1 - l1 - l2
                if min(l1, l2, l3) >= -1e-4: return l1 * a[1] + l2 * b[1] + l3 * c[1]
    return hy(x, z)

# ---------------- visual mesh store ----------------
LIGHT = (0.42, 0.82, 0.38)
class Mesh:
    def __init__(s, mat): s.mat = mat; s.pos = []; s.uv = []; s.col = []; s.tri = []
    def v(s, p, uv, c=(1, 1, 1)):
        s.pos.append(tuple(p)); s.uv.append(tuple(uv)); s.col.append(tuple(min(1.0, max(0.0, x)) for x in c)); return len(s.pos) - 1
    def poly(s, ps, uvs, c=(1, 1, 1), shade=True):
        if shade:
            n = bcolib.tri_normal(*ps[:3]); L = math.sqrt(sum(x * x for x in n)) or 1.0
            k = 0.72 + 0.34 * abs(sum(n[i] / L * LIGHT[i] for i in range(3)))
            c = tuple(x * k for x in c)
        i = [s.v(p, t, c) for p, t in zip(ps, uvs)]
        if len(i) == 4 and getattr(ps, 'split', 0): i = i[1:] + i[:1]     # the collision's diagonal (Quad)
        for k in range(1, len(i) - 1): s.tri.append((i[0], i[k], i[k + 1]))
MESH = collections.OrderedDict()
def mesh(mat):
    if mat not in MESH: MESH[mat] = Mesh(mat)
    return MESH[mat]

# ---------------- arena wall ----------------
def wall_line():
    R = AR; pts = []
    for cx, cz, a0 in ((AX1 - R, AZ0 + R, -90), (AX1 - R, AZ1 - R, 0), (AX0 + R, AZ1 - R, 90), (AX0 + R, AZ0 + R, 180)):
        for k in range(7): a = math.radians(a0 + 90 * k / 6); pts.append((cx + R * math.cos(a), cz + R * math.sin(a)))
    return pts
WL = wall_line()
CX, CZ = (AX0 + AX1) / 2, (AZ0 + AZ1) / 2
def segs(pts, maxlen, closed=True, inward=(CX, CZ)):
    out = []
    for i in range(len(pts) if closed else len(pts) - 1):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        if L < 1: continue
        nx, nz = -(b[1] - a[1]) / L, (b[0] - a[0]) / L
        mx, mz = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
        if inward is not None and nx * (inward[0] - mx) + nz * (inward[1] - mz) < 0: nx, nz = -nx, -nz
        k = max(1, int(math.ceil(L / maxlen)))
        for s in range(k):
            out.append(((a[0] + (b[0] - a[0]) * s / k, a[1] + (b[1] - a[1]) * s / k),
                        (a[0] + (b[0] - a[0]) * (s + 1) / k, a[1] + (b[1] - a[1]) * (s + 1) / k), (nx, nz)))
    return out
WALL_H, WALL_RUN = T.get('wall_h', 4000.0), 120.0
# 2026-11-11: each wall piece had its own jittered corners and leaned out along its own normal, so neighbours left
# slits at every joint, and the wall is only WALL_RUN thick seen from above: a kart driven into a corner at speed got
# through and fell under the floor. Now the pieces share their corners (seed 0: jitter keyed on the position, tops
# leaning along the joint's mean normal).
WSEG = segs(WL, 2500)
def joint_n(si):                                 # the mean of the normals of the pieces meeting at piece si's start
    a, b = WSEG[si - 1][2], WSEG[si][2]; L = math.hypot(a[0] + b[0], a[1] + b[1]) or 1.0
    return ((a[0] + b[0]) / L, (a[1] + b[1]) / L)
for ring, off in enumerate((0.0,)):
    for si, (p0, p1, (ix, iz)) in enumerate(WSEG):
        n0, n1 = joint_n(si), joint_n((si + 1) % len(WSEG))
        def make(seed, p0=p0, p1=p1, ix=ix, iz=iz, si=si, n0=n0, n1=n1, off=off, ring=ring):
            def P(p, n, top, sd):
                x = p[0] - n[0] * (off + (WALL_RUN if top else 0)); z = p[1] - n[1] * (off + (WALL_RUN if top else 0))
                if seed == 0:
                    i, j = int(round(x / 4)), int(round(z / 4))
                    return (x + J(i, j, 1) * 8, BASE - 300 + (WALL_H if top else 0), z + J(i, j, 2) * 8)
                return (x + J(si * 7 + seed + ring * 5000, top, sd) * 18, BASE - 300 + (WALL_H if top else 0),
                        z + J(si * 7 + seed + ring * 5000, top + 2, sd + 5) * 18)
            return [P(p0, n0, 0, 1), P(p1, n1, 0, 2), P(p1, n1, 1, 3), P(p0, n0, 1, 4)]
        face = lambda nrm, ix=ix, iz=iz: nrm[0] * ix + nrm[2] * iz > 0.5 and nrm[1] > 0.005
        assert try_quad(make, face, typ=0x201), ('wall flags', ring, si)   # the game's wall type: bounces, never driven up
# a flat band of the wall type along the wall's foot, BAND_IN inside it to BAND_OUT outside, a little over the floor:
# the stock courses' walls are mostly such flat triangles (the game stops a kart from driving onto wall ground). With
# only the steep wall a kart pushed along it rode up its face and over the top (2026-11-11)
BAND_IN, BAND_OUT, BAND_Y = 150.0, 1300.0, 6.0
for si, (p0, p1, (ix, iz)) in enumerate(WSEG):
    n0, n1 = joint_n(si), joint_n((si + 1) % len(WSEG))
    def mkb(seed, p0=p0, p1=p1, n0=n0, n1=n1, si=si):
        def B(p, n, d, sd):
            x, z = p[0] - n[0] * d, p[1] - n[1] * d
            if seed == 0:
                i, j = int(round(x / 4)), int(round(z / 4))
                return (x + J(i, j, 1) * 8, BASE + BAND_Y, z + J(i, j, 2) * 8)
            return (x + J(si * 11 + seed + 9000, d > 0, sd) * 18, BASE + BAND_Y, z + J(si * 11 + seed + 9000, (d > 0) + 2, sd + 5) * 18)
        return [B(p0, n0, -BAND_IN, 1), B(p1, n1, -BAND_IN, 2), B(p1, n1, BAND_OUT, 3), B(p0, n0, BAND_OUT, 4)]
    assert try_quad(mkb, up, typ=0x201) or try_quad(lambda s, f=mkb: f(s)[::-1], up, typ=0x201), ('wall band flags', si)
# a dead zone under everything: a kart that gets under the floor or off its edge is put back on the course
DZ = 1500.0
def dzq(seed):
    return [(FX0 - 30000 + J(seed, 1, 1) * 900, BASE - DZ, FZ0 - 30000 + J(seed, 1, 2) * 900), (FX1 + 30000 + J(seed, 2, 1) * 900, BASE - DZ, FZ0 - 30000 + J(seed, 2, 2) * 900),
            (FX1 + 30000 + J(seed, 3, 1) * 900, BASE - DZ, FZ1 + 30000 + J(seed, 3, 2) * 900), (FX0 - 30000 + J(seed, 4, 1) * 900, BASE - DZ, FZ1 + 30000 + J(seed, 4, 2) * 900)][::-1]
assert try_quad(dzq, up, typ=0xa01) or try_quad(lambda s: dzq(s)[::-1], up, typ=0xa01), 'dead zone flags'
print('with walls', len(bco.tris))

# ---------------- features ----------------
LIFT0 = 1.5 if not T.get('height') else 8.0     # the roads' visual lift over the floor (hilly tracks: more)
PAD_LIFT = LIFT0 + 2.5                           # flat pads drawn this far over the floor: above every road layer
E = 120.0                                        # side inset at the top of a strip (steep sides, like build6's jump ends)
def strip_base(F):
    """a strip's base height along u: the terrain at its centre (level tracks), the terrain along its axis ('follow':
    jumps on a graded road ride its slope), or a fixed height ('base': a bridge's deck over a canyon or water)"""
    cx, cz = F['c']; a = math.radians(F['ang']); dx, dz = math.cos(a), math.sin(a); lift = F.get('lift', 0.0)
    if F.get('base') is not None: return lambda u: F['base'] + lift
    if F.get('follow', T.get('follow', False)): return lambda u: HF(cx + dx * u, cz + dz * u) + lift
    h0 = HF(cx, cz) + lift
    return lambda u: h0
def strip(F, k):
    cx, cz = F['c']; a = math.radians(F['ang']); dx, dz = math.cos(a), math.sin(a); nx, nz = -dz, dx
    hbu = strip_base(F); hb = hbu(0); w2 = F['w'] / 2
    def P(u, v, h): return (cx + dx * u + nx * v, BASE + hbu(u) + h, cz + dz * u + nz * v)
    def jq(pts, s, base): return SJ(pts) if s == 0 else [(x + J(k * 31 + s, base + n, 1) * 8, y, z + J(k * 31 + s, base + n, 2) * 8) for n, (x, y, z) in enumerate(pts)]
    for pi, pc in enumerate(F['pieces']):
        pr = pc['p']; mat = pc.get('mat', 'mound'); boost = set(pc.get('boost', ())); th = pc.get('thick')
        if F.get('rails'):                       # railings along a deck's edges (low walls facing in)
            for si in range(len(pr) - 1):
                (u0, h0), (u1, h1) = pr[si], pr[si + 1]
                if u1 - u0 < 1 or min(h0, h1) <= 0.5: continue
                for s_ in (-1, 1):
                    v0, v1 = s_ * (w2 - 60), s_ * (w2 - 20); rh = F['rails']
                    mk = lambda s, u0=u0, u1=u1, h0=h0, h1=h1, v0=v0, v1=v1, rh=rh, s_=s_, si=si: jq([P(u0, v0, h0 - 30), P(u1, v0, h1 - 30), P(u1, v1, h1 + rh), P(u0, v1, h0 + rh)], s, pi * 40 + si * 4 + 500 + s_)
                    face = lambda n, s_=s_: -(n[0] * nx + n[2] * nz) * s_ > 0.5 and n[1] > 0.005
                    # (railcol False: seen only. 2026-11-13: a wall overhead holds karts below it too: the Sunburst overpass's
                    # rails stopped karts on the road beneath at the deck's edge line)
                    if F.get('railcol', True) and not try_quad(mk, face, typ=0x201, tries=80): NOCOL.append((F.get('name', k), 'rail', si))
                    L_ = (u1 - u0) / 1200
                    mesh('t7_rail').poly([P(u0, v0, h0 - 30), P(u1, v0, h1 - 30), P(u1, v0, h1 + rh), P(u0, v0, h0 + rh)], [(0, 0), (L_, 0), (L_, 1), (0, 1)], shade=False)
        acc = 0.0
        for si in range(len(pr) - 1):
            (u0, h0), (u1, h1) = pr[si], pr[si + 1]
            if u1 - u0 < 1: continue
            ve = lambda h: w2 - (E if h > 0.5 else 0.0)
            quad = lambda s, u0=u0, u1=u1, h0=h0, h1=h1, si=si: jq([P(u0, -ve(h0), h0), P(u0, ve(h0), h0), P(u1, ve(h1), h1), P(u1, -ve(h1), h1)], s, pi * 40 + si * 4)
            sl = (h1 - h0) / (u1 - u0)
            if abs(sl) <= 1.6: want = up
            else:
                sd = -1 if sl > 0 else 1
                want = lambda n, sd=sd: (n[0] * dx + n[2] * dz) * sd > 0.5 and n[1] > 0.005
            if h0 <= 0.5 and h1 <= 0.5: continue    # on the floor: nothing to add
            typ = 0x800 if si in boost else 0x100
            ps = None if pc.get('nocol') else try_quad(quad, want, typ=typ)   # (nocol: seen only, 2026-11-13)
            if not ps and not pc.get('nocol'): NOCOL.append((F.get('name', k), pi, si))
            if not ps: ps = quad(0)
            L = math.hypot(u1 - u0, h1 - h0)
            if si in boost:
                nb = max(1, round(F['w'] / 900))
                uvs = [(0, 0), (nb, 0), (nb, 1), (0, 1)] if sl >= 0 else [(nb, 1), (0, 1), (0, 0), (nb, 0)]
                mesh('t7_boost').poly(ps, uvs)
            else:
                tm = 't7_kick' if mat == 'kick' else 't7_mound'
                uvs = [(-w2 / 600, acc / 600), (w2 / 600, acc / 600), (w2 / 600, (acc + L) / 600), (-w2 / 600, (acc + L) / 600)]
                mesh(tm).poly(ps, uvs, (1, 1, 1) if sl >= 0 else (0.93, 0.93, 0.93))
            acc += L
            if th and h0 > 0.5 and h1 > 0.5:     # a deck's underside (seen from the road it bridges)
                mesh('t7_kickside' if mat == 'kick' else 't7_moundside').poly([P(u0, -w2, h0 - th), P(u1, -w2, h1 - th), P(u1, w2, h1 - th), P(u0, w2, h0 - th)],
                                                                              [(0, 0), (L / 900, 0), (L / 900, F['w'] / 900), (0, F['w'] / 900)], (0.55, 0.55, 0.55), shade=False)
            for s_ in (-1, 1):                  # the sides (a deck's: only its thickness)
                A_, B_ = P(u0, s_ * w2, max(0.0, h0 - th) if th else 0), P(u1, s_ * w2, max(0.0, h1 - th) if th else 0)
                C_, D_ = P(u1, s_ * ve(h1), h1), P(u0, s_ * ve(h0), h0)
                if h0 <= 0.5: pts = [A_, B_, C_]
                elif h1 <= 0.5: pts = [A_, B_, D_]
                else: pts = [A_, B_, C_, D_]
                face = lambda n, s_=s_: (n[0] * nx + n[2] * nz) * s_ > 0.3 and n[1] > 0.005
                mk = lambda s, pts=pts, s_=s_, si=si: jq(pts, s, pi * 40 + si * 4 + (2 if s_ > 0 else 1) + 200)
                # 2026-11-13: an elevated deck's edge is only seen: its steep side faces (up-facing, as collision needs) held
                # karts driving out from under the Sunburst overpass at the deck's edge line (the rails keep karts on top)
                lifted = th and min(h0, h1) - th > 300
                got = None if pc.get('nocol') or lifted else (try_tri if len(pts) == 3 else try_quad)(mk, face, tries=80)
                vis = got or pts
                sm = 't7_kickside' if mat == 'kick' else 't7_moundside'
                mesh(sm).poly(vis, [((p[0] * dx + p[2] * dz) / 900.0, (p[1] - BASE - hb) / 900.0) for p in vis], (0.82, 0.82, 0.82))
    def PF(u, v, dy):                            # on the floor as built (the grid's triangles, not the hill function:
        x, z = cx + dx * u + nx * v, cz + dz * u + nz * v   # a pad flat at the centre's height sank into a rising
        return (x, floor_y(x, z) + dy, z)        # floor or stood out of a falling one, 2026-11-11)
    if F.get('padh') is not None: PF = lambda u, v, dy: P(u, v, F['padh'] + dy)   # a pad up on a deck (2026-11-13)
    for pad in F.get('pads', ()):   # flat boost pads (u0, u1): collision 3 over the floor, drawn over the roads
        u0, u1 = pad
        mk = lambda s, u0=u0, u1=u1: jq([PF(u0, -w2 + 150, 3), PF(u0, w2 - 150, 3), PF(u1, w2 - 150, 3), PF(u1, -w2 + 150, 3)], s, 300)
        ps = try_quad(mk, up, typ=F.get('padtyp', 0x800)) or NOCOL.append((F.get('name', k), 'pad')) or mk(0)
        nb = max(1, round((F['w'] - 300) / 800))
        vis = Quad((x, y - 3 + PAD_LIFT, z) for x, y, z in ps); vis.split = getattr(ps, 'split', 0)
        mesh(F.get('padtex', 't7_boost')).poly(vis, [(0, 0), (nb, 0), (nb, 1), (0, 1)] if u1 > u0 else [(nb, 1), (0, 1), (0, 0), (nb, 0)], shade=False)

def berm_base(F):
    """a berm's base height round its arc: the terrain at its centre, or ('follow') at the road under each angle"""
    cx, cz = F['c']
    if F.get('follow', T.get('follow', False)):
        return lambda a: HF(cx + F['r0'] * math.cos(math.radians(a)), cz + F['r0'] * math.sin(math.radians(a)))
    h0 = HF(cx, cz)
    return lambda a: h0
def berm(F, k):
    """banked turn; the bank ramps up from the road over the first TP degrees and back down over the last (smoothstep),
    so the turn's ends are smooth transitions (no end face to run into)"""
    cx, cz = F['c']; r0, r1, H = F['r0'], F['r1'], F['h']; a0, a1 = F['a0'], F['a1']
    hba = berm_base(F); n = max(6, int(abs(a1 - a0) / 6))
    TP = min(40.0, abs(a1 - a0) / 3)
    def hh(a):
        t = max(0.0, min(1.0, min(abs(a - a0), abs(a1 - a)) / TP))
        return H * t * t * (3 - 2 * t)
    def P(r, a, h): return (cx + r * math.cos(math.radians(a)), BASE + hba(a) + h, cz + r * math.sin(math.radians(a)))
    def jq(pts, s, base): return SJ(pts) if s == 0 else [(x + J(k * 57 + s, base + m, 1) * 8, y, z + J(k * 57 + s, base + m, 2) * 8) for m, (x, y, z) in enumerate(pts)]
    for i in range(n):
        aa, ab = a0 + (a1 - a0) * i / n, a0 + (a1 - a0) * (i + 1) / n
        ha, hb_ = hh(aa), hh(ab)
        bst = F.get('boost') and n // 4 <= i < n - n // 4 and i % 3 == 1      # boost strips round the bank's middle
        mk = lambda s, aa=aa, ab=ab, i=i, ha=ha, hb_=hb_: jq([P(r0, aa, 0), P(r1, aa, ha), P(r1, ab, hb_), P(r0, ab, 0)], s, i * 8)
        ps = try_quad(mk, up, typ=0x800 if bst else 0x100) or NOCOL.append((F.get('name', k), 'berm', i)) or mk(0)
        arc = r1 * math.radians(abs(ab - aa)) / 600
        if bst: mesh('t7_boost').poly(ps, [(0, 1), (3, 1), (3, 0), (0, 0)] if a1 > a0 else [(3, 0), (0, 0), (0, 1), (3, 1)])
        else: mesh('t7_mound').poly(ps, [(0, i * arc), (ha / 400, i * arc), (hb_ / 400, (i + 1) * arc), (0, (i + 1) * arc)])
        if ha + hb_ < 1: continue
        am = math.radians((aa + ab) / 2); rx, rz = math.cos(am), math.sin(am)
        side = [P(r1 + 320, aa, 0), P(r1 + 320, ab, 0)] + ([P(r1, ab, hb_)] if hb_ > 0.5 else []) + ([P(r1, aa, ha)] if ha > 0.5 else [])
        mk = lambda s, side=side, i=i: jq(side, s, i * 8 + 4)
        face = lambda nn, rx=rx, rz=rz: nn[0] * rx + nn[2] * rz > 0.5 and nn[1] > 0.005
        ps = (try_tri if len(side) == 3 else try_quad)(mk, face, tries=80) or mk(0)
        mesh('t7_moundside').poly(ps, [(0, 0), (arc, 0), (arc, H / 600), (0, H / 600)][:len(ps)], (0.8, 0.8, 0.8))

for k, F in enumerate(T['features']):
    (strip if F['t'] == 'strip' else berm)(F, k)
for F in T['features']:
    if F['t'] == 'strip': F['_hb'] = strip_base(F)
def feat_y(x, z, ang=None):
    """the highest feature surface over (x, z), None off them (strips' profiles, berms' banks). ang: only the strips
    running that way (within 50 degrees, either direction): a road passing under a bridge isn't on it"""
    best = None
    for F in T['features']:
        cx, cz = F['c']
        if F.get('ghost'): continue
        if F['t'] == 'strip':
            a = math.radians(F['ang']); dx, dz = math.cos(a), math.sin(a)
            if ang is not None and abs(math.cos(a - ang)) < 0.85: continue
            u, v = (x - cx) * dx + (z - cz) * dz, -(x - cx) * dz + (z - cz) * dx
            if abs(v) > F['w'] / 2: continue
            hb = F['_hb'](u)
            for pc in F['pieces']:
                pr = pc['p']
                for (u0, h0), (u1, h1) in zip(pr, pr[1:]):
                    if u0 <= u <= u1 and u1 - u0 > 1:
                        y = BASE + hb + h0 + (h1 - h0) * (u - u0) / (u1 - u0)
                        best = y if best is None else max(best, y)
        else:
            r = math.hypot(x - cx, z - cz)
            if not F['r0'] <= r <= F['r1']: continue
            a0, a1 = F['a0'], F['a1']; ang = math.degrees(math.atan2(z - cz, x - cx))
            lo, hi = min(a0, a1), max(a0, a1)
            while ang < lo: ang += 360
            while ang > hi + 1e-6 and ang - 360 >= lo - 1e-6: ang -= 360
            if not lo <= ang <= hi: continue
            TP = min(40.0, abs(a1 - a0) / 3); t = max(0.0, min(1.0, min(abs(ang - a0), abs(a1 - ang)) / TP))
            y = BASE + berm_base(F)(ang) + F['h'] * t * t * (3 - 2 * t) * (r - F['r0']) / (F['r1'] - F['r0'])
            best = y if best is None else max(best, y)
    return best
def surf_y(x, z, ang=None):
    f = feat_y(x, z, ang); y = floor_y(x, z)
    return y if f is None else max(y, f)
print('features', len(T['features']), 'collision tris', len(bco.tris), 'missing flags', NOCOL[:20], len(NOCOL))

# ---------------- route, fences ----------------
def smooth(pl, closed, step=700.0):
    n = len(pl); sm = []
    for i in range(n if closed else n - 1):
        P0 = pl[(i - 1) % n] if closed or i > 0 else pl[0]; P1 = pl[i]; P2 = pl[(i + 1) % n]
        P3 = pl[(i + 2) % n] if closed or i + 2 < n else pl[-1]
        k = max(2, int(math.hypot(P2[0] - P1[0], P2[1] - P1[1]) // step))
        for s in range(k):
            t = s / k
            sm.append(tuple(0.5 * (2 * P1[c] + (P2[c] - P0[c]) * t + (2 * P0[c] - 5 * P1[c] + 4 * P2[c] - P3[c]) * t * t
                                   + (3 * P1[c] - P0[c] - 3 * P2[c] + P3[c]) * t ** 3) for c in (0, 1)))
    if not closed: sm.append(pl[-1])
    return sm
def _dedup(pl, mind=200.0):
    """closed polyline without points within mind of the one before (rounded() leaves a doubled point where two arcs
    meet with no straight between: smoothed, the route stepped 20 forward then 20 back and CPUs turned round there)"""
    out = []
    for p in pl:
        if not out or math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) >= mind: out.append(p)
    while len(out) > 2 and math.hypot(out[0][0] - out[-1][0], out[0][1] - out[-1][1]) < mind: out.pop()
    return out
ROUTE = smooth(_dedup(T['route']), True, 1500)
FH, FRUN = 700.0, 40.0
def fence(pl, closed, k, vis=True):
    pl = smooth(pl, closed, 900) if len(pl) > 2 else list(pl)
    for si, (p0, p1, (fx, fz)) in enumerate(segs(pl, 1400, closed, None)):
        mx, mz = (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2
        q = min(ROUTE, key=lambda r: (r[0] - mx) ** 2 + (r[1] - mz) ** 2)
        if fx * (q[0] - mx) + fz * (q[1] - mz) < 0: fx, fz = -fx, -fz       # face the track
        y0, y1 = floor_y(*p0), floor_y(*p1)
        def make(seed, p0=p0, p1=p1, fx=fx, fz=fz, si=si, y0=y0, y1=y1):
            j = lambda a, b: J(k * 1000 + si * 3 + seed, a, b) * 10
            return [(p0[0] + j(0, 1), y0 - 60, p0[1] + j(0, 2)), (p1[0] + j(1, 1), y1 - 60, p1[1] + j(1, 2)),
                    (p1[0] - fx * FRUN + j(2, 1), y1 + FH, p1[1] - fz * FRUN + j(2, 2)), (p0[0] - fx * FRUN + j(3, 1), y0 + FH, p0[1] - fz * FRUN + j(3, 2))]
        face = lambda nrm, fx=fx, fz=fz: nrm[0] * fx + nrm[2] * fz > 0.5 and nrm[1] > 0.005
        if not try_quad(make, face, tries=80): NOCOL.append(('fence', k, si))
        if not vis: continue
        L = math.hypot(p1[0] - p0[0], p1[1] - p0[1]) / 1600
        hv = T['theme'].get('fence_h', 320.0)
        ps = [(p0[0], y0 - 20, p0[1]), (p1[0], y1 - 20, p1[1]), (p1[0], y1 + hv, p1[1]), (p0[0], y0 + hv, p0[1])]
        mesh('t7_barrier' if T['theme'].get('track_fence', 'barrier') == 'barrier' else 't7_fence').poly(ps, [(0, 0), (L, 0), (L, 1), (0, 1)], shade=False)
FENCES = list(T.get('fences', []))
if T.get('fence_off'):                          # both sides of the route, as closed loops
    # the route without repeated points: two corners with no straight between them (a hairpin) repeat one, and
    # there the tangent was zero, so the offset fell back onto the centre line: a fence across the road (2026-11-11)
    RF = []
    for p in ROUTE:
        if not RF or math.hypot(p[0] - RF[-1][0], p[1] - RF[-1][1]) > 50.0: RF.append(p)
    while len(RF) > 2 and math.hypot(RF[0][0] - RF[-1][0], RF[0][1] - RF[-1][1]) <= 50.0: RF.pop()
    n_ = len(RF); offs = ([], [])
    for i in range(n_):
        a_, b_ = RF[i - 1], RF[(i + 1) % n_]
        tx, tz = b_[0] - a_[0], b_[1] - a_[1]; L = math.hypot(tx, tz)
        assert L > 1.0, ('fence tangent', i, RF[i])
        for s_, o in ((1, offs[0]), (-1, offs[1])):
            o.append((RF[i][0] - tz / L * T['fence_off'] * s_, RF[i][1] + tx / L * T['fence_off'] * s_))
    FENCES += [(offs[0], True), (offs[1], True)]
for k, (pl, closed) in enumerate(FENCES): fence(pl, closed, k)

# ---------------- 2026-11-12 unique tracks: tunnels, props (scenery with solid trunks), zone surfaces ----------------
def tunnel(Tn, k):
    """a covered stretch of road: (a, b) its axis, w its inside width, h its height, kind 'rock' (a hill over it),
    'shed' (a steel building) or 'ice'. Walls collide (fence() lines, facing the road); roof and shell are visual"""
    (ax, az), (bx, bz) = Tn['a'], Tn['b']; w2 = Tn['w'] / 2; H = Tn.get('h', 3000.0); kind = Tn.get('kind', 'rock')
    L = math.hypot(bx - ax, bz - az); dx, dz = (bx - ax) / L, (bz - az) / L; nx, nz = -dz, dx
    n = max(2, int(L // 1500))
    for s_ in (-1, 1):
        fence([(ax + nx * w2 * s_, az + nz * w2 * s_), (bx + nx * w2 * s_, bz + nz * w2 * s_)], False, 900 + k * 2 + (s_ > 0), vis=False)
    inner = {'rock': 't7_rock', 'shed': 't7_concrete', 'ice': 't7_ice'}[kind]
    shell = {'rock': 't7_rock', 'shed': 't7_roof', 'ice': 't7_snow'}[kind]
    th = Tn.get('shell', 2600.0)
    def Q(u, v, y): x, z = ax + dx * u + nx * v, az + dz * u + nz * v; return (x, floor_y(x, z) + y, z)
    ARC = [(-1.0, 0.0), (-1.0, 0.62), (-0.82, 0.86), (-0.45, 0.98), (0.0, 1.0), (0.45, 0.98), (0.82, 0.86), (1.0, 0.62), (1.0, 0.0)]
    for i in range(n):
        u0, u1 = L * i / n, L * (i + 1) / n
        for j in range(len(ARC) - 1):
            (v0, y0), (v1, y1) = ARC[j], ARC[j + 1]
            dark = 0.5 + 0.12 * math.sin(j)
            mesh(inner).poly([Q(u0, v0 * w2, y0 * H), Q(u1, v0 * w2, y0 * H), Q(u1, v1 * w2, y1 * H), Q(u0, v1 * w2, y1 * H)],
                             [(u0 / 1500, j), (u1 / 1500, j), (u1 / 1500, j + 1), (u0 / 1500, j + 1)], (dark, dark, dark * (1.15 if kind == 'ice' else 1)), shade=False)
            ow = w2 + th; oh = H + th * 0.7
            mesh(shell).poly([Q(u0, v0 * ow, y0 * oh - (300 if j in (0, len(ARC) - 2) else 0)), Q(u1, v0 * ow, y0 * oh - (300 if j in (0, len(ARC) - 2) else 0)),
                              Q(u1, v1 * ow, y1 * oh), Q(u0, v1 * ow, y1 * oh)], [(u0 / 2000, j), (u1 / 2000, j), (u1 / 2000, j + 1), (u0 / 2000, j + 1)], (0.9, 0.9, 0.9))
        if i % 2 == 0:                            # ceiling lights
            um = (u0 + u1) / 2
            mesh('t7_light').poly([Q(um - 300, -250, H * 0.99), Q(um + 300, -250, H * 0.99), Q(um + 300, 250, H * 0.99), Q(um - 300, 250, H * 0.99)], [(0, 0), (1, 0), (1, 1), (0, 1)], (1, 1, 1), shade=False)
    for u in (0.0, L):                           # the portals: the band between the arch and the shell
        for j in range(len(ARC) - 1):
            (v0, y0), (v1, y1) = ARC[j], ARC[j + 1]; ow = w2 + th; oh = H + th * 0.7
            mesh(shell).poly([Q(u, v0 * w2, y0 * H), Q(u, v1 * w2, y1 * H), Q(u, v1 * ow, y1 * oh), Q(u, v0 * ow, y0 * oh)], [(0, 0), (1, 0), (1, 1), (0, 1)], (0.8, 0.8, 0.8))
for k, Tn in enumerate(T.get('tunnels', [])): tunnel(Tn, k)

PROPS = []                                       # (x, z, kind, size): set out by tracks9's scatter / placed lists
def solid(x, z, r, h, k):
    """a prop's trunk or body: four leaning wall quads round a square (half size r), facing out"""
    y = floor_y(x, z)
    for side in range(4):
        a = math.pi / 2 * side + 0.31; ox, oz = math.cos(a), math.sin(a); tx, tz = -oz, ox
        def mk(seed, side=side, ox=ox, oz=oz, tx=tx, tz=tz):
            j = lambda m, q: J(k * 13 + side * 3 + seed, m, q) * 6
            c0 = (x + ox * r - tx * r, z + oz * r - tz * r); c1 = (x + ox * r + tx * r, z + oz * r + tz * r)
            return [(c0[0] + j(0, 1), y - 40, c0[1] + j(0, 2)), (c1[0] + j(1, 1), y - 40, c1[1] + j(1, 2)),
                    (c1[0] - ox * 40 + j(2, 1), y + h, c1[1] - oz * 40 + j(2, 2)), (c0[0] - ox * 40 + j(3, 1), y + h, c0[1] - oz * 40 + j(3, 2))]
        face = lambda nrm, ox=ox, oz=oz: nrm[0] * ox + nrm[2] * oz > 0.5 and nrm[1] > 0.005
        if not try_quad(mk, face, typ=0x201, tries=60): NOCOL.append(('prop', k, side))
import props9
for k, (x, z, kind, size) in enumerate(T.get('props', [])):
    y = floor_y(x, z)
    col = props9.build(mesh, kind, x, y, z, size, k, T['theme'])
    if col: solid(x, z, col[0], col[1], k)
# water and lava surfaces over their basins (visual: the floor under them carries the kind)
for Z in ZONES:
    if Z.get('level') is None: continue
    m = mesh({'water': 't7_water', 'deep': 't7_water', 'lava': 't7_lava', 'ice': 't7_ice'}.get(Z['kind'], 't7_water'))
    y = BASE + Z['level']
    if Z['kind'] == 'lava': m.nofog = True      # glows through the haze
    if Z.get('band'):                            # a river / moat: quads along a polyline of (x, z, half width)
        Lf, Rt = band_sides(Z['band'])
        for i in range(len(Lf) - 1):
            q = [(Lf[i][0], y, Lf[i][1]), (Lf[i + 1][0], y, Lf[i + 1][1]), (Rt[i + 1][0], y, Rt[i + 1][1]), (Rt[i][0], y, Rt[i][1])]
            m.poly(q, [(p[0] / 3000, p[2] / 3000) for p in q], shade=False)
        continue
    if not Z.get('poly'): continue
    pts = Z['poly']; cxz = (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        m.poly([(cxz[0], y, cxz[1]), (a[0], y, a[1]), (b[0], y, b[1])], [(cxz[0] / 3000, cxz[1] / 3000), (a[0] / 3000, a[1] / 3000), (b[0] / 3000, b[1] / 3000)], shade=False)
print('collision tris', len(bco.tris), 'verts', len(bco.verts), 'missing flags', len(NOCOL))
bco.gx = 0; bco.gz = 0

# ---------------- visual: floor, roads, decor ----------------
TH = T['theme']
TEXFMT = tex7.make(A7, TH)
from tex6 import _noise
lowf = _noise(64, 4, 31)
fl0 = mesh('t7_ground')
for tri, zk in zip(FLOOR, FLOORK):
    cols = []
    for (x, y, z) in tri:
        nn = lowf[int((z - FZ0) / (FZ1 - FZ0) * 63) % 64, int((x - FX0) / (FX1 - FX0) * 63) % 64]
        cols.append(0.9 + 0.18 * nn)
    n = bcolib.tri_normal(*tri); L = math.sqrt(sum(x * x for x in n)) or 1.0
    k = 0.75 + 0.3 * abs(sum(n[i] / L * LIGHT[i] for i in range(3))) / 0.82
    fl = mesh(SURF[zk][1]) if zk else fl0
    if T.get('cliffs') and abs(n[1] / L) < 0.82 and zk not in ('water', 'ice'): fl = mesh('t7_rock')   # canyon walls, crater sides
    base = len(fl.pos)
    for (x, y, z), cc in zip(tri, cols): fl.v((x, y, z), (x / 1400.0, z / 1400.0), (cc * k, cc * k, cc * k * 0.98))
    fl.tri.append((base, base + 1, base + 2))
# the far ground around the arena (visual only, no overlap with the floor)
FAR = 260000.0; yfar = BASE - 4
for (x0, z0, x1, z1) in ((-FAR, -FAR, FAR, FZ0), (-FAR, FZ1, FAR, FAR), (-FAR, FZ0, FX0, FZ1), (FX1, FZ0, FAR, FZ1)):
    ps = [(x0, yfar, z0), (x0, yfar, z1), (x1, yfar, z1), (x1, yfar, z0)]
    mesh('t7_ground').poly(ps, [(p[0] / 1400.0, p[2] / 1400.0) for p in ps], (0.8, 0.8, 0.8), shade=False)

def ribbon(pl, closed, lift, mat, half):
    m = mesh(mat)
    if closed and math.hypot(pl[0][0] - pl[-1][0], pl[0][1] - pl[-1][1]) < 1: pl = pl[:-1]
    pl = smooth(pl, closed, 350); n = len(pl)
    def dirn(a, b):
        L = math.hypot(b[0] - a[0], b[1] - a[1]) or 1.0
        return ((b[0] - a[0]) / L, (b[1] - a[1]) / L)
    side = []
    for i in range(n):
        if closed or 0 < i < n - 1: d0 = dirn(pl[i - 1], pl[i]); d1 = dirn(pl[i], pl[(i + 1) % n])
        else: d0 = d1 = dirn(pl[0], pl[1]) if i == 0 else dirn(pl[-2], pl[-1])
        tx, tz = d0[0] + d1[0], d0[1] + d1[1]; L = math.hypot(tx, tz) or 1.0; tx, tz = tx / L, tz / L
        cs = max(0.6, tx * d1[0] + tz * d1[1])
        side.append((-tz / cs * half, tx / cs * half))
    acc, prev, vs, hz = 0.0, None, [], []
    for i in (range(n + 1) if closed else range(n)):
        p = pl[i % n]
        if prev is not None: acc += math.hypot(p[0] - prev[0], p[1] - prev[1])
        prev = p; ox, oz = side[i % n]; v = acc / 2800.0
        a_, b_ = (p[0] - ox, p[1] - oz), (p[0] + ox, p[1] + oz)
        vs.append((m.v((a_[0], floor_y(*a_) + lift, a_[1]), (0, v), (0.95, 0.95, 0.95)), m.v((b_[0], floor_y(*b_) + lift, b_[1]), (1, v), (0.95, 0.95, 0.95))))
        hz.append(bool(ZONES) and (zone_at(*a_) in HAZ or zone_at(*b_) in HAZ or zone_at(*p) in HAZ))
    for c in range(len(vs) - 1):
        if hz[c] or hz[c + 1]: continue          # no road painted down into a canyon or under lava (a bridge or jump spans it)
        (a, b), (c_, d_) = vs[c], vs[c + 1]
        m.tri += [(a, b, d_), (a, d_, c_)]
ROADS = [(T['route'], True, T.get('road_w', 5600.0))] if T.get('route_road', True) else []
ROADS += [(pl, False, w) for pl, w in T.get('paths', [])]
lift0 = LIFT0
for ri, (pl, closed, w) in enumerate(ROADS): ribbon(pl, closed, lift0 - 1.0 + 0.15 * ri, 't7_berm', w / 2 + T.get('berm', 900.0))
for ri, (pl, closed, w) in enumerate(ROADS): ribbon(pl, closed, lift0 + 0.5 + 0.15 * ri, 't7_road', w / 2)

# decor around the wall line
VP = [(p0, nrm) for (p0, p1, nrm) in segs(WL, 1500)]
VN = []
for i, (p, n0) in enumerate(VP):
    n1 = VP[i - 1][1]; mx, mz = n0[0] + n1[0], n0[1] + n1[1]; L = math.hypot(mx, mz) or 1.0; cs = (mx / L) * n0[0] + (mz / L) * n0[1]
    VN.append((p, (mx / L / max(cs, 0.5), mz / L / max(cs, 0.5))))
DECOR = TH.get('decor', 'stands')
acc = 0.0
for i in range(len(VN)):
    (p0, n0), (p1, n1) = VN[i], VN[(i + 1) % len(VN)]
    L = math.hypot(p1[0] - p0[0], p1[1] - p0[1]); u0, u1 = acc / 1600.0, (acc + L) / 1600.0; c0, c1 = acc / 2600.0, (acc + L) / 2600.0; acc += L
    def off(p, n, d_, y): return (p[0] - n[0] * d_, BASE + y, p[1] - n[1] * d_)
    def band(mat, d0, y0, d1, y1, uu, v1, col=(1, 1, 1)):
        mesh(mat).poly([off(p0, n0, d0, y0), off(p1, n1, d0, y0), off(p1, n1, d1, y1), off(p0, n0, d1, y1)], [(uu[0], 0), (uu[1], 0), (uu[1], v1), (uu[0], v1)], col, shade=False)
    if DECOR == 'stands':
        band('t7_barrier', 0, 0, 0, 380, (u0, u1), 1)
        band('t7_concrete', 0, 380, 900, 380, (u0, u1), .5, (0.9, 0.9, 0.9))
        band('t7_concrete', 900, 380, 900, 700, (u0, u1), .2, (0.7, 0.7, 0.7))
        band('t7_crowd', 900, 700, 7400, 4600, (c0, c1), 3.0)
        band('t7_concrete', 7400, 4600, 7400, 6600, (u0, u1), .8, (0.75, 0.75, 0.8))
        band('t7_roof', 7400, 6600, 4600, 7300, (u0 * 3, u1 * 3), 1, (0.65, 0.65, 0.7))
    elif DECOR == 'city':
        band('t7_barrier', 0, 0, 0, 420, (u0, u1), 1)
        band('t7_concrete', 0, 420, 300, 420, (u0, u1), .2, (0.85, 0.85, 0.85))
    else:                                        # open country: a rustic fence on the wall line
        band('t7_fence', 0, -20, 0, 340, (u0 * 2, u1 * 2), 1)
def box(x, z, sx, sz, h, mat='t7_windows', roofm='t7_concrete', col=(0.9, 0.9, 0.95)):
    q = [(x - sx, z - sz), (x + sx, z - sz), (x + sx, z + sz), (x - sx, z + sz)]
    for i in range(4):
        a, b = q[i], q[(i + 1) % 4]; L = math.hypot(b[0] - a[0], b[1] - a[1]) / 1500
        mesh(mat).poly([(a[0], BASE - 10, a[1]), (b[0], BASE - 10, b[1]), (b[0], BASE + h, b[1]), (a[0], BASE + h, a[1])], [(0, 0), (L, 0), (L, h / 1500), (0, h / 1500)], col)
    mesh(roofm).poly([(p[0], BASE + h, p[1]) for p in q], [(0, 0), (1, 0), (1, 1), (0, 1)], tuple(c * 0.8 for c in col))
def tower(x, z, ix, iz):
    s = 220.0; top = 9500.0
    q = [(x - s, z - s), (x + s, z - s), (x + s, z + s), (x - s, z + s)]
    for i in range(4):
        a, b = q[i], q[(i + 1) % 4]
        mesh('t7_concrete').poly([(a[0], BASE, a[1]), (b[0], BASE, b[1]), (b[0], BASE + top, b[1]), (a[0], BASE + top, a[1])], [(0, 0), (.5, 0), (.5, 8), (0, 8)], (0.8, 0.8, 0.85))
    px_, pz_ = -iz, ix; c = (x + ix * 260, z + iz * 260); w = 1100.0
    ps = [(c[0] - px_ * w, BASE + top - 300, c[1] - pz_ * w), (c[0] + px_ * w, BASE + top - 300, c[1] + pz_ * w),
          (c[0] + px_ * w, BASE + top + 1100, c[1] + pz_ * w), (c[0] - px_ * w, BASE + top + 1100, c[1] - pz_ * w)]
    mesh('t7_light').poly(ps, [(0, 0), (1, 0), (1, 1), (0, 1)], (1.2, 1.2, 1.2), shade=False)
rr = random.Random(5)
if DECOR == 'stands':
    for i in (3, 10, 17, 24):
        x, z = WL[i]; dx, dz = CX - x, CZ - z; L = math.hypot(dx, dz)
        tower(x - dx / L * 5200, z - dz / L * 5200, dx / L, dz / L)
elif DECOR == 'city':
    for i in range(0, len(VN), 2):
        (p, n0) = VN[i]; dd = rr.uniform(5000, 14000)
        box(p[0] - n0[0] * dd, p[1] - n0[1] * dd, rr.uniform(1500, 3000), rr.uniform(1500, 3000), rr.uniform(4000, 16000))
    for i in (3, 10, 17, 24):
        x, z = WL[i]; dx, dz = CX - x, CZ - z; L = math.hypot(dx, dz)
        tower(x - dx / L * 2500, z - dz / L * 2500, dx / L, dz / L)
for (x, z, sx, sz, h, col) in T.get('boxes', []): box(x, z, sx, sz, h, 't7_container', 't7_container', col)    # scenery (visual only)

def write_dae(path, meshes, dy=0.0):
    f = lambda v: ' '.join(('%.4f' % x).rstrip('0').rstrip('.') or '0' for x in v)
    L = ['<?xml version="1.0"?>', '<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">',
         '<asset><unit name="meter" meter="1" /><up_axis>Y_UP</up_axis></asset>', '<library_images>']
    for i, m in enumerate(meshes):
        L.append('<image id="m%d%s-diffuse-image"><init_from>%s</init_from></image>' % (i, m.mat, os.path.abspath(A7 + '/' + m.mat + '.png').replace(':', '%3a')))
    L.append('</library_images><library_effects>')
    for i, m in enumerate(meshes):
        n = 'm%d%s' % (i, m.mat)
        L.append('<effect id="%s-fx" name="%s"><profile_COMMON><newparam sid="%s-diffuse-surface"><surface type="2D"><init_from>%s-diffuse-image</init_from></surface></newparam>'
                 '<newparam sid="%s-diffuse-sampler"><sampler2D><source>%s-diffuse-surface</source></sampler2D></newparam><technique sid="standard"><phong>'
                 '<diffuse><texture texture="%s-diffuse-sampler" texcoord="CHANNEL0" /></diffuse></phong></technique></profile_COMMON></effect>' % (n, n, n, n, n, n, n))
    L.append('</library_effects><library_materials>')
    for i, m in enumerate(meshes):
        n = 'm%d%s' % (i, m.mat); L.append('<material id="%s" name="%s"><instance_effect url="#%s-fx"/></material>' % (n, n, n))
    L.append('</library_materials><library_geometries>')
    for i, m in enumerate(meshes):
        g = 'meshId%d' % i; nv = len(m.pos)
        L.append('<geometry id="%s" name="%s_name"><mesh>' % (g, g))
        for nm, arr, params in (('positions', [c for p in m.pos for c in (p[0], p[1] + dy, p[2])], 'XYZ'), ('tex0', [c for t in m.uv for c in t], 'ST'), ('color0', [c for t in m.col for c in t], 'RGB')):
            L.append('<source id="%s-%s" name="%s-%s"><float_array id="%s-%s-array" count="%d">%s</float_array><technique_common><accessor count="%d" offset="0" source="#%s-%s-array" stride="%d">%s</accessor></technique_common></source>'
                     % (g, nm, g, nm, g, nm, len(arr), f(arr), nv, g, nm, len(params), ''.join('<param name="%s" type="float" />' % c for c in params)))
        L.append('<vertices id="%s-vertices"><input semantic="POSITION" source="#%s-positions" /><input semantic="TEXCOORD" source="#%s-tex0" /><input semantic="COLOR" source="#%s-color0" /></vertices>' % (g, g, g, g))
        L.append('<polylist count="%d" material="theresonlyone"><input offset="0" semantic="VERTEX" source="#%s-vertices" /><vcount>%s</vcount><p>%s</p></polylist></mesh></geometry>'
                 % (len(m.tri), g, '3 ' * len(m.tri), ' '.join('%d %d %d' % t for t in m.tri)))
    L.append('</library_geometries><library_controllers>')
    for i, m in enumerate(meshes):
        c = 'mesh-%d-skin' % i; nv = len(m.pos)
        L.append('<controller id="%s" name="mesh-%dSkin"><skin source="#meshId%d"><bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>'
                 '<source id="%s-joints-array"><Name_array id="%s-joints-array" count="1">world_root</Name_array><technique_common><accessor source="#%s-joints-array" count="1" stride="1"><param name="JOINT" type="Name"></param></accessor></technique_common></source>'
                 '<source id="%s-bind_poses-array"><float_array id="%s-bind_poses-array" count="16">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</float_array><technique_common><accessor source="#%s-bind_poses-array" count="1" stride="16"><param name="TRANSFORM" type="float4x4"></param></accessor></technique_common></source>'
                 '<source id="%s-weights-array"><float_array id="%s-weights-array" count="%d">%s</float_array><technique_common><accessor source="#%s-weights-array" count="%d" stride="1"><param name="WEIGHT" type="float"></param></accessor></technique_common></source>'
                 '<joints><input semantic="JOINT" source="#%s-joints-array"></input><input semantic="INV_BIND_MATRIX" source="#%s-bind_poses-array"></input></joints>'
                 '<vertex_weights count="%d"><input semantic="JOINT" source="#%s-joints-array" offset="0"></input><input semantic="WEIGHT" source="#%s-weights-array" offset="1"></input><vcount>%s</vcount><v>%s</v></vertex_weights></skin></controller>'
                 % (c, i, i, c, c, c, c, c, c, c, c, nv, '1 ' * nv, c, nv, c, c, nv, c, c, '1 ' * nv, '0 0 ' * nv))
    L.append('</library_controllers><library_visual_scenes><visual_scene id="RootNode" name="RootNode"><node id="skeleton_root" name="skeleton_root"><matrix sid="matrix">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>'
             '<node id="world_root" name="world_root" sid="world_root" type="JOINT"><matrix sid="matrix">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix></node></node>')
    for i, m in enumerate(meshes):
        n = 'm%d%s' % (i, m.mat)
        L.append('<node id="mesh-%d" name="mesh-%d" type="NODE"><instance_controller url="#mesh-%d-skin"><skeleton>#skeleton_root</skeleton><bind_material><technique_common>'
                 '<instance_material symbol="m%s" target="#%s" /></technique_common></bind_material></instance_controller></node>' % (i, i, i, n, n))
    L.append('</visual_scene></library_visual_scenes><scene><instance_visual_scene url="#RootNode" /></scene></COLLADA>')
    open(path, 'w', encoding='utf-8').write('\n'.join(L))

TEMPLATE = [m for m in json.load(open(W + '/wal_materials.json')) if m['Name'] == 'wl_dirt_v'][0]
def superbmd(meshes, stem, nofog=False):
    write_dae(A7 + '/' + stem + '.dae', meshes, MODEL_DY if stem == 'course' else 0.0)
    mats, heads, seen = [], [], set()
    for i, m in enumerate(meshes):
        mt = copy.deepcopy(TEMPLATE); mt['Name'] = m.mat; mt['TextureNames'] = [m.mat] + [''] * 7
        mt['CullMode'] = 'None'
        if (nofog or TH.get('nofog') or getattr(m, 'nofog', False)) and isinstance(mt.get('FogInfo'), dict): mt['FogInfo']['Enable'] = False
        mats.append(mt)
        if m.mat not in seen:
            seen.add(m.mat)
            heads.append(dict(Name=m.mat, Format=TEXFMT[m.mat], AlphaSetting=0, WrapS='Repeat', WrapT='Repeat', PaletteFormat='IA8', MipMap=0,
                              EdgeLOD=False, BiasClamp=False, MaxAniso=0, MinFilter='Linear', MagFilter='Linear', MinLOD=0.0, MaxLOD=0.0, LodBias=0.0))
    json.dump(mats, open(A7 + '/' + stem + '_materials.json', 'w'), indent=1)
    json.dump(heads, open(A7 + '/' + stem + '_tex_headers.json', 'w'), indent=1)
    out = A7 + '/' + stem + '.bmd'
    if os.path.exists(out): os.remove(out)
    r = subprocess.run([SUPERBMD, A7 + '/' + stem + '.dae', out, '-m', A7 + '/' + stem + '_materials.json',
                        '-x', A7 + '/' + stem + '_tex_headers.json'], capture_output=True, text=True)
    assert os.path.exists(out), r.stdout[-1500:] + r.stderr[-500:]
    return open(out, 'rb').read()

base = mkarc.load(W + '/mini/' + FILE)
HAS_SKY = (NAME + '_sky.bmd') in base.root.files
MS = [m for m in MESH.values() if m.tri]
sky = Mesh('t7_sky'); R_ = 120000.0; NA, NB = 32, 8   # 2026-11-12: was 200000 with a sky model: the course intro's cameras (far off centre) clipped its far side (black)
ids = {}
# 2026-11-12: a skirt below the dome's foot (rows -2, -1: straight down, the texture's bottom row): seen from the course
# intro's high cameras the foot was above the far ground, the clear colour (black) showing under it
SK = -2
for a in range(NA + 1):
    for b in range(SK, NB + 1):
        th_, ph = 2 * math.pi * a / NA, (math.pi / 2) * max(b, 0) / NB
        y = BASE - 9000 + R_ * 0.45 * math.sin(ph) + (R_ * 0.25 * b if b < 0 else 0)
        ids[(a, b)] = sky.v((R_ * math.cos(ph) * math.cos(th_), y, R_ * math.cos(ph) * math.sin(th_)), (a / NA * 3.0, max(b, 0) / NB))
for a in range(NA):
    for b in range(SK, NB):
        sky.tri += [(ids[(a, b)], ids[(a + 1, b)], ids[(a + 1, b + 1)]), (ids[(a, b)], ids[(a + 1, b + 1)], ids[(a, b + 1)])]
if HAS_SKY: SKY = superbmd([sky], 'sky', nofog=True)
else: sky.nofog = True; MS.append(sky)              # the slot has no sky model: the dome goes into the course model
print('visual meshes', [(m.mat, len(m.tri)) for m in MS], 'tris', sum(len(m.tri) for m in MS))
BMD = superbmd(MS, 'course')

# ---------------- BOL ----------------
bf = base.root.files[NAME + '_course.bol']; bf.seek(0)
mb = BOL.from_file(bf)
# the header's first byte (libbol 'roll') is the course's shaking: Tilt-A-Kart's slot has 2, so Flowline Canyon tilted
# up to 15 degrees every 15 s of a battle (Course::calcShaking): karts rode up the edges and fell out (2026-11-11)
mb.roll = 0
box_ = [o for o in mb.objects.objects if o.objectid == 1]
if not box_:                                     # (Pipe Plaza has its own box kind): the generic item box from Cookie Land's
    cb = mkarc.load(W + '/mini/Mini7.arc').root.files['mini7_course.bol']; cb.seek(0)
    box_ = [o for o in BOL.from_file(cb).objects.objects if o.objectid == 1]
assert box_, 'no item box to copy settings from'
tmpl = box_[0]
mb.objects.objects = []
for (x, z, ang, n) in T['items']:
    a = math.radians(ang); nx, nz = -math.sin(a), math.cos(a)
    for i in range(n):
        t = (i - (n - 1) / 2) * 520.0
        ob = copy.deepcopy(tmpl); px, pz = x + nx * t, z + nz * t
        ob.position = Vector3(px, surf_y(px, pz) + WDY, pz)
        mb.objects.objects.append(ob)
print('item boxes', len(mb.objects.objects))
wb = wal.root.files['waluigi_course.bol']; wb.seek(0)
wr = BOL.from_file(wb).kartpoints.positions[0].rotation.mtx
fw = (wr[0][0], -wr[0][1]); lw = (wr[1][0], -wr[1][1])          # Waluigi's first start: forward, left (x, z)
SX, SZ, SANG = T['start']
dl = math.radians(SANG) - math.atan2(fw[1], fw[0])
rot2 = lambda v: (v[0] * math.cos(dl) - v[1] * math.sin(dl), v[0] * math.sin(dl) + v[1] * math.cos(dl))
f2, l2 = rot2(fw), rot2(lw)
rot = Rotation(Vector3(f2[0], 0, f2[1]), Vector3(0, 1, 0), Vector3(l2[0], 0, l2[1]))
fa = math.radians(SANG); fx, fz = math.cos(fa), math.sin(fa); nx, nz = -fz, fx
dense = []
for i in range(len(ROUTE)):
    a, b = ROUTE[i], ROUTE[(i + 1) % len(ROUTE)]
    L = math.hypot(b[0] - a[0], b[1] - a[1]); k = max(1, int(L // 1500))
    for s in range(k): dense.append((a[0] + (b[0] - a[0]) * s / k, a[1] + (b[1] - a[1]) * s / k))
# 2026-11-12 (coins and items very common): a row of boxes and a run of coins round the inside of every bend, on level
# ground only (never on a berm or jump). Not item rows for the starts.
def _hd(i): (x0, z0), (x1, z1) = dense[i % len(dense)], dense[(i + 1) % len(dense)]; return math.atan2(z1 - z0, x1 - x0)
def _turn(i, w=1):
    xa, za = dense[(i - w) % len(dense)]; xb, zb = dense[i % len(dense)]; xc, zc = dense[(i + w) % len(dense)]
    d = math.atan2(zc - zb, xc - xb) - math.atan2(zb - za, xb - xa); return (d + math.pi) % (2 * math.pi) - math.pi
flat = lambda x, z: (lambda f: f is None or f <= floor_y(x, z) + 1)(feat_y(x, z))
# (dense splits each smoothed segment into straight pieces: judge the turn over 3 points either side)
st = next((i for i in range(len(dense)) if abs(_turn(i, 3)) < math.radians(6)), 0)
bends, run = [], []
for k in range(len(dense) + 1):
    i = (st + k) % len(dense)
    if k < len(dense) and abs(_turn(i, 3)) >= math.radians(6): run.append(i); continue
    if run and abs(sum(_turn(j) for j in run)) >= math.radians(30): bends.append(run)
    run = []
CB = 0
for run in bends:
    sg = 1 if sum(_turn(j) for j in run) > 0 else -1           # left (+) or right bend: the inside is that way
    m = run[len(run) // 2]; h = _hd(m); nx_, nz_ = -math.sin(h) * sg, math.cos(h) * sg
    x0, z0 = dense[m]
    for k in range(5):
        t = 600 + (k - 2) * 520.0
        px, pz = x0 + nx_ * t, z0 + nz_ * t
        if not flat(px, pz): continue
        ob = copy.deepcopy(tmpl); ob.position = Vector3(px, surf_y(px, pz) + WDY, pz)
        mb.objects.objects.append(ob); CB += 1
    for j in [run[len(run) * q // 6] for q in range(1, 6)]:       # five coins round each bend
        h = _hd(j); px, pz = dense[j][0] - math.sin(h) * sg * 900, dense[j][1] + math.cos(h) * sg * 900
        if flat(px, pz): T['coins'].append(((px, pz), (px, pz), 1))
print('bends', len(bends), 'corner boxes', CB, 'item boxes now', len(mb.objects.objects))
# eight starts, one per kart (playerid = kart): kart 0 (the first player) on the start line, the others spread
# round the loop an eighth apart, each on level road (off the jumps), facing along the route
ks_t = mb.kartpoints.positions[0]
def rot_at(deg):
    d_ = math.radians(deg) - math.atan2(fw[1], fw[0])
    r2 = lambda v: (v[0] * math.cos(d_) - v[1] * math.sin(d_), v[0] * math.sin(d_) + v[1] * math.cos(d_))
    f_, l_ = r2(fw), r2(lw)
    return Rotation(Vector3(f_[0], 0, f_[1]), Vector3(0, 1, 0), Vector3(l_[0], 0, l_[1]))
# (the game hands the starts out in its own order, the first player's too: every one is a good one) the start
# line, then the item rows on the straights spread evenly round the loop: 900 before a row, facing along it
rows = [(x, z, ang) for (x, z, ang, n) in T['items']]
def rpos(p):
    return min(range(len(dense)), key=lambda i: math.hypot(dense[i][0] - p[0], dense[i][1] - p[1]))
i0 = rpos((SX, SZ))
rows.sort(key=lambda r: (rpos(r) - i0) % len(dense))
def on_pad(x, z):
    for F in T['features']:
        if not F.get('pads'): continue
        a = math.radians(F['ang']); u = (x - F['c'][0]) * math.cos(a) + (z - F['c'][1]) * math.sin(a)
        v = -(x - F['c'][0]) * math.sin(a) + (z - F['c'][1]) * math.cos(a)
        if abs(v) <= F['w'] / 2 and any(min(p) - 150 <= u <= max(p) + 150 for p in F['pads']): return True
    return False
def fx_near(x, z, m=300):
    """(x, z) on or within m of a runtime effect that throws karts (trickfx: spring pads, geysers)"""
    for e in T.get('fx', ()):
        dx, dz = x - e['x'], z - e['z']
        if e['t'] == 'geyser' and math.hypot(dx, dz) <= e['r'] + m: return True
        if e['t'] == 'pad':
            a = math.radians(e['ang']); u = dx * math.cos(a) + dz * math.sin(a); v = -dx * math.sin(a) + dz * math.cos(a)
            if abs(u) <= e['hl'] + m and abs(v) <= e['hw'] + 150 + m: return True
    return False
# 2026-11-13: starts are judged on the real collision too (every surface built so far): karts started right in front of
# boost pads, springs, rails and hazards that the feature list above doesn't know about and caught on them
COLIDX = None
def col_index():
    global COLIDX
    COLIDX = collections.defaultdict(list)
    for ti, t in enumerate(bco.tris):
        ps = [bco.verts[k] for k in t['v']]
        x0, x1 = min(p[0] for p in ps), max(p[0] for p in ps); z0, z1 = min(p[2] for p in ps), max(p[2] for p in ps)
        for i in range(int(x0 // 500), int(x1 // 500) + 1):
            for j in range(int(z0 // 500), int(z1 // 500) + 1): COLIDX[(i, j)].append(ti)
def col_bad(x, z, y0):
    """what a kart driving at height y0 meets at (x, z) on the collision: None (plain level ground) or why not"""
    if COLIDX is None: col_index()
    for ti in COLIDX.get((int(x // 500), int(z // 500)), ()):
        t = bco.tris[ti]; a, b, c = [bco.verts[k] for k in t['v']]
        d_ = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if abs(d_) < 1e-9:
            if max(a[1], b[1], c[1]) > y0 - 50 and min(a[1], b[1], c[1]) < y0 + 400: return 'wall'
            continue
        l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d_
        l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d_
        l3 = 1 - l1 - l2
        if min(l1, l2, l3) < -1e-4: continue
        y = l1 * a[1] + l2 * b[1] + l3 * c[1]
        if y < y0 - 60 or y > y0 + 600: continue
        n = bcolib.tri_normal(a, b, c)
        if abs(n[1]) < 0.7: return 'wall'
        if abs(y - y0) > 12: return 'step'
        at = t['typ'] >> 8
        if at == 8: return 'boost'
        if at in (2, 10): return 'hazard'
    return None
def start_col(px, pz, deg, ahead=1800):
    a = math.radians(deg); ca, sa = math.cos(a), math.sin(a)
    for s in range(-300, ahead + 1, 100):
        for v in (-450, 0, 450):
            x, z = px + ca * s - sa * v, pz + sa * s + ca * v
            if col_bad(x, z, floor_y(x, z)): return max(0, s)
    return 3000
def start_check(px, pz, deg):
    return min(start_check0(px, pz, deg), start_col(px, pz, deg))
def start_check0(px, pz, deg):
    """the clear run ahead of a kart standing at (px, pz) facing deg: 0 when it stands on (or under) a jump or berm,
    else how far ahead the first jump or berm surface is (3000: none that close). 2026-11-11: starts 900 before
    an item row could fall on the previous jump's tail or a berm: the kart started inside the ramp's collision and
    stuttered and caught until it got out"""
    a = math.radians(deg); ca, sa = math.cos(a), math.sin(a)
    for s in range(-250, 3001, 50):
        for v in (-600, 0, 600):
            x, z = px + ca * s - sa * v, pz + sa * s + ca * v
            f = feat_y(x, z)
            if f is not None and f > floor_y(x, z) + 1: return max(0, s)
            if s <= 400 and on_pad(x, z): return 0   # 2026-11-12: not on or right against a flat boost pad (karts caught on its edge); one further ahead is fine
            if s <= 600 and fx_near(x, z): return 0  # 2026-11-13: nor on or against a spring pad or geyser (runtime effects, not in the collision)
    return 3000
def clear_start(x, z, ang, backs=tuple(sorted(range(-1500, 3501, 100), key=lambda b: abs(b - 900)))):   # nearest 900 back first; 2026-11-12 wider (pads)
    """the first spot behind (x, z) with 1500 of clear run, else the one with the longest"""
    a = math.radians(ang); best = None
    for b in backs:
        p = (x - b * math.cos(a), z - b * math.sin(a), ang); c = start_check(*p)
        if c >= 1500: return p, c
        if best is None or c > best[1]: best = (p, c)
    return best
picks = [clear_start(SX, SZ, SANG, (0, -400, 400, -800, 800, -1200, 1200, -1600))[0]]
cands = [clear_start(x, z, ang) for x, z, ang in rows]
# 2026-11-13: every start its own spot. Two karts could pick the same row (identical starts on 7 tracks) or one
# right behind the start line (Volcano 0/1 220 apart): a row is used once and a start keeps START_SEP from the others
START_SEP = 2500
def sep(p): return min(math.hypot(p[0] - q[0], p[1] - q[1]) for q in picks)
used = set()
for k in range(1, 8):
    if not rows: break
    i = min(len(rows) - 1, k * len(rows) // 8)
    free = [j for j in range(len(rows)) if j not in used and sep(cands[j][0]) >= START_SEP]
    if not free: break
    near = sorted(free, key=lambda j: (abs(j - i), j))[:5]       # the row there, or a neighbour with a better run
    j = max(near, key=lambda j: (min(cands[j][1], 1500), -abs(j - i)))
    used.add(j); picks.append(cands[j][0])
# too few rows: the rest along the loop, the dense route point farthest from every start so far
while len(picks) < 8:
    best = max((clear_start(dense[d][0], dense[d][1], math.degrees(math.atan2(dense[(d + 1) % len(dense)][1] - dense[d][1],
                dense[(d + 1) % len(dense)][0] - dense[d][0]))) for d in range(0, len(dense), max(1, len(dense) // 48))),
               key=lambda c: (min(c[1], 1500) >= 1500, sep(c[0])))
    picks.append(best[0])
dmin = min(math.hypot(a[0] - b[0], a[1] - b[1]) for n_, a in enumerate(picks) for b in picks[n_ + 1:])
print('CHECK starts: closest two', round(dmin), 'OK' if dmin >= START_SEP else 'TOO CLOSE')
print('start clearance', [start_check(*p) for p in picks])
starts = []
for k in range(8):
    px, pz, deg = picks[k]
    kq = copy.deepcopy(ks_t)
    kq.position = Vector3(px, floor_y(px, pz) - 2.5 + (3 if on_pad(px, pz) else 0) + WDY, pz)
    kq.rotation = rot_at(deg); kq.playerid = k
    starts.append(kq)
mb.kartpoints.positions = starts
RS_BAD = 0
for k, jp in enumerate(mb.respawnpoints):
    idx = (k * len(dense)) // max(1, len(mb.respawnpoints))
    def rs_ok(i):
        (x0, z0), (x1, z1) = dense[i % len(dense)], dense[(i + 1) % len(dense)]
        return start_check(x0, z0, math.degrees(math.atan2(z1 - z0, x1 - x0))) >= 800
    idx = next((idx + d for d in sorted(range(-6, 7), key=abs) if rs_ok(idx + d)), idx) % len(dense)   # not on a jump
    RS_BAD += not rs_ok(idx)
    x, z = dense[idx]
    jp.position = Vector3(x, floor_y(x, z) - 2.5 + WDY, z)
print('respawns', len(mb.respawnpoints), 'on jumps', RS_BAD)
# 2026-11-12 unique tracks: Cookie Land's slot has no respawn points, so a fall (a hazard zone, the dead zone under the
# arena) had nowhere to go. Tracks with hazards get T['nresp'] (10) round the loop, on level road off the hazards,
# facing along it (ids 1..n, as Tilt-A-Kart's: unk1 0xffff, unk2/3 -1); each hazard triangle falls to the nearest
# 2026-11-13: every track (the dead zone under the arena, 0x0a01, sends a fall to respawn 1: stadium, meadow,
# rhythmridge and megaramp had none); a spot with no good point within +-10 took the bad one (Frostbite 3 on a boost)
if True:
    from lib.libbol import JugemPoint
    N_ = T.get('nresp', 8); RS = []
    def rs_run(i):
        (x0, z0), (x1, z1) = dense[i % len(dense)], dense[(i + 1) % len(dense)]
        return start_check(x0, z0, math.degrees(math.atan2(z1 - z0, x1 - x0))) if zone_at(x0, z0) is None else -1
    span = max(10, len(dense) // (2 * N_))
    for q in range(N_):
        idx = q * len(dense) // N_
        near = sorted(range(-span, span + 1), key=abs)
        good = next((idx + d for d in near if rs_run(idx + d) >= 900), None)
        def spot_ok(i):   # (no clear run anywhere near: at least plain level ground under the kart itself)
            x_, z_ = dense[i % len(dense)]; f_ = feat_y(x_, z_)
            return zone_at(x_, z_) is None and col_bad(x_, z_, floor_y(x_, z_)) is None and not on_pad(x_, z_)                 and not fx_near(x_, z_) and (f_ is None or f_ <= floor_y(x_, z_) + 1)
        idx = good if good is not None else max((idx + d for d in near), key=lambda i: (spot_ok(i), rs_run(i), -abs(i - idx)))
        idx %= len(dense)
        if rs_run(idx) < 900: print('respawn', q + 1, 'best run only', rs_run(idx))
        (x0, z0), (x1, z1) = dense[idx], dense[(idx + 1) % len(dense)]
        jp = JugemPoint(Vector3(x0, floor_y(x0, z0) + 20 + WDY, z0)); jp.rotation = rot_at(math.degrees(math.atan2(z1 - z0, x1 - x0)))
        jp.respawn_id = q + 1; jp.unk1 = 0xffff; jp.unk2 = -1; jp.unk3 = -1
        mb.respawnpoints.append(jp); RS.append((x0, z0))
        if (10, q + 1, 255, 0, 0) not in bco.mats: bco.mats.append((10, q + 1, 255, 0, 0))
    for ti in HAZ_TRIS:
        p = [bco.verts[v] for v in bco.tris[ti]['v']]; mx, mz = sum(v[0] for v in p) / 3, sum(v[2] for v in p) / 3
        bco.tris[ti]['typ'] = 0x0a00 | (1 + min(range(len(RS)), key=lambda i: (RS[i][0] - mx) ** 2 + (RS[i][1] - mz) ** 2))
    print('respawn points', len(RS), 'hazard tris', len(HAZ_TRIS))
BCO_DATA = bcolib.write(bco)
# the route/coins key hashes the BOL header (mostly object counts): two tracks with the same counts would share a key
# (2026-11-12 cups: frostbite / megaramp, rhythmridge / alpine), so a track can nudge its fog end (invisible)
mb.fog_endz += T.get('key_salt', 0)
buf = io.BytesIO(); mb.write(buf); BOL_DATA = buf.getvalue()
head = BOL_DATA[:0x7c]; hsh = 2166136261
for ch in head: hsh = ((hsh ^ ch) * 16777619) & 0xffffffff
KEY = '%02x_%08x' % (mb.music_id & 0xff, hsh)
os.makedirs(OUT + '/routes', exist_ok=True); os.makedirs(OUT + '/coins', exist_ok=True)
def map_frame():
    """map_bti's mapping: texture x = 4 + (z - zmin) * sc, y = 4 + (xmax - x) * sc over 128 x 256: Race2D orientation 3
    (getMapPos: texture x from (z - z0) / (z1 - z0), y from (x1 - x) / (x1 - x0)), the box the whole texture shows"""
    xs = [p[0] for p in WL]; zs = [p[1] for p in WL]
    sc = min((128 - 8) / (max(zs) - min(zs)), (256 - 8) / (max(xs) - min(xs)))
    z0 = min(zs) - 4 / sc; x1 = max(xs) + 4 / sc
    return (x1 - 256 / sc, z0, x1, z0 + 128 / sc, 3)
MAPF = map_frame()
with open(OUT + '/routes/' + KEY + '.txt', 'w', newline='\n') as f:
    f.write('# MKDD-NX trick battle route (MKDD_TrickTracks tools/build7.py %s): %s loop\n' % (TRK, T['title']))
    f.write('# map %.1f %.1f %.1f %.1f %d\n> 0 0\n' % MAPF)
    for i_, (x, z) in enumerate(dense + dense[:1]):
        (xa, za), (xb, zb) = dense[(i_ - 1) % len(dense)], dense[(i_ + 1) % len(dense)]
        f.write('%.1f %.1f %.1f\n' % (x, surf_y(x, z, math.atan2(zb - za, xb - xa)) + WDY, z))   # (the strips running its way: under a bridge, the road)
if T.get('fx'):                                  # the runtime's effects (runtime/trickfx.c): spring pads, geysers, tornadoes
    os.makedirs(OUT + '/fx', exist_ok=True)
    with open(OUT + '/fx/' + KEY + '.txt', 'w', newline='\n') as f:
        f.write('# %s effects (MKDD_TrickTracks tools/build7.py %s): runtime/trickfx.c reads these\n' % (T['title'], TRK))
        for e in T['fx']:
            y = surf_y(e['x'], e['z']) + WDY
            if e['t'] == 'pad': f.write('pad %.1f %.1f %.1f %.2f %.0f %.0f %.1f%s\n' % (e['x'], y, e['z'], e['ang'], e['hw'], e['hl'], e['vy'], ' %.1f' % e['fw'] if e.get('fw') else ''))
            elif e['t'] == 'geyser': f.write('geyser %.1f %.1f %.1f %.0f %d %d %.1f %d %d\n' % (e['x'], y, e['z'], e['r'], e['period'], e['on'], e['vy'], e['kind'], e['phase']))
            elif e['t'] == 'tornado': f.write('tornado %.1f %.1f %.1f %.0f %.0f %d %d %.0f %.0f %.1f\n' % (e['x'], y, e['z'], e['ax'], e['az'], e['px'], e['pz'], e['r'], e['h'], e['lift']))
    print('fx', len(T['fx']), 'effects')
coins = []
for cn in T['coins']:
    a, b, n = cn[:3]
    for i in range(n):
        t = i / (n - 1) if n > 1 else 0.5
        x, z = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        y = surf_y(x, z) + 150
        if len(cn) == 6:                         # an arc over a jump (heights above the base), never under the surface
            y0, y1, bump = cn[3:]
            y = max(y, BASE + y0 + (y1 - y0) * t + bump * 4 * t * (1 - t))
        coins.append((x, y + WDY, z))
with open(OUT + '/coins/' + KEY + '.txt', 'w', newline='\n') as f:
    f.write('# %s coins (MKDD_TrickTracks tools/build7.py %s)\n' % (T['title'], TRK))
    for c in coins: f.write('%.1f %.1f %.1f\n' % c)
print('route/coins key', KEY, 'route points', len(dense), 'coins', len(coins))

# ---------------- 2026-11-13 defect check (on the finished collision): prints CHECK lines, never changes the track ----------------
col_index()
def tops(x, z):
    """every walkable surface over (x, z): (y, attribute)"""
    out = []
    for ti in COLIDX.get((int(x // 500), int(z // 500)), ()):
        t = bco.tris[ti]; a, b, c = [bco.verts[k] for k in t['v']]
        d_ = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if abs(d_) < 1e-9 or abs(bcolib.tri_normal(a, b, c)[1]) < 0.7: continue
        l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d_
        l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d_
        if min(l1, l2, 1 - l1 - l2) < -1e-4: continue
        out.append((l1 * a[1] + l2 * b[1] + (1 - l1 - l2) * c[1], t['typ'] >> 8))
    return out
def ground(x, z, y):
    """the surface a kart at height y stands on or falls to"""
    h = [t for t in tops(x, z) if t[0] <= y + 80]
    return max(h) if h else None
def seg_wall(x0, z0, x1, z1, y, over=False):
    """a wall a kart driving from (x0, z0) to (x1, z1) at height y would hit (over: one hanging above it, which holds
    karts below it too, 2026-11-13)"""
    ks = set()
    for i in range(int(min(x0, x1) // 500), int(max(x0, x1) // 500) + 1):
        for j in range(int(min(z0, z1) // 500), int(max(z0, z1) // 500) + 1): ks.update(COLIDX.get((i, j), ()))
    for ti in ks:
        t = bco.tris[ti]; ps = [bco.verts[k] for k in t['v']]
        nn = bcolib.tri_normal(*ps)
        if abs(nn[1]) >= 0.35: continue      # (steep banks and ramp faces are driven over)
        if over:
            if min(p[1] for p in ps) <= y + 300 or not (t['flag'] or t['typ'] >> 8 == 2): continue
        else:
            if nn[0] * (x1 - x0) + nn[2] * (z1 - z0) >= 0: continue   # (facing away: the back of a lip the kart flies off)
            if max(p[1] for p in ps) < y + 60 or min(p[1] for p in ps) > y + 300: continue
        for e in range(3):
            ax, az, bx, bz = ps[e][0], ps[e][2], ps[(e + 1) % 3][0], ps[(e + 1) % 3][2]
            d1 = (x1 - x0) * (az - z0) - (z1 - z0) * (ax - x0); d2 = (x1 - x0) * (bz - z0) - (z1 - z0) * (bx - x0)
            d3 = (bx - ax) * (z0 - az) - (bz - az) * (x0 - ax); d4 = (bx - ax) * (z1 - az) - (bz - az) * (x1 - ax)
            if d1 * d2 < 0 and d3 * d4 < 0: return ti
    return None
NCHK = collections.Counter()
def chk(kind, msg):
    NCHK[kind] += 1
    if NCHK[kind] <= 6: print('CHECK', kind, msg)
hw_ = T.get('road_w', 6000) / 2 - 500
for i in range(len(dense)):
    (x0, z0), (x1, z1) = dense[i], dense[(i + 1) % len(dense)]
    h = math.atan2(z1 - z0, x1 - x0); nx_, nz_ = -math.sin(h), math.cos(h)
    for v in (-hw_, 0, hw_):
        ax, az, bx, bz = x0 + nx_ * v, z0 + nz_ * v, x1 + nx_ * v, z1 + nz_ * v
        y = surf_y(ax, az, h)
        nearc = any(f.get('kind') == 'canyon' and math.hypot(f['c'][0] - ax, f['c'][1] - az) < f.get('gap', 0) + 3000 for f in T['features'])
        w = None if nearc else seg_wall(ax, az, bx, bz, max(y, surf_y(bx, bz, h)))
        wo = None if nearc else seg_wall(ax, az, bx, bz, min(y, surf_y(bx, bz, h)), over=True)
        if wo is not None: chk('route-overhead', 'at (%.0f %.0f) v %.0f: tri %d typ %#x above the road' % (ax, az, v, wo, bco.tris[wo]['typ']))
        if w is not None: chk('route-wall', 'at (%.0f %.0f) v %.0f: tri %d typ %#x' % (ax, az, v, w, bco.tris[w]['typ']))
        g = ground(ax, az, y)
        if g and g[1] == 10 and v == 0 and not nearc:
            chk('route-hazard', 'at (%.0f %.0f): the road runs over a hazard (no jump there)' % (ax, az))
for ob in mb.objects.objects:
    x, y, z = ob.position.x, ob.position.y - WDY, ob.position.z
    g = ground(x, z, y + 200)
    if g is None or g[1] == 10: chk('box-hazard', 'item box at (%.0f %.0f) over %s' % (x, z, 'nothing' if g is None else 'a hazard'))
    elif y < g[0] - 50: chk('box-buried', 'item box at (%.0f %.0f %.0f) under the surface %.0f' % (x, y, z, g[0]))
for (x, y, z) in coins:
    y -= WDY; t_ = tops(x, z)
    if any(abs(ty - y) < 60 for ty, _ in t_): chk('coin-buried', 'coin at (%.0f %.0f %.0f) in a surface' % (x, y, z))
    g = ground(x, z, y)
    if g and g[1] == 10 and y - g[0] < 500 and not any(ty > y for ty, a in t_ if a != 10): chk('coin-hazard', 'coin at (%.0f %.0f) just over a hazard' % (x, z))
for jp in mb.respawnpoints:
    x, y, z = jp.position.x, jp.position.y - WDY, jp.position.z
    g = ground(x, z, y + 100)
    if g is None or g[1] in (2, 8, 10): chk('respawn', 'respawn %d at (%.0f %.0f) on %s' % (jp.respawn_id, x, z, g))
for e in T.get('fx', ()):
    if e['t'] != 'pad': continue
    g = ground(e['x'], e['z'], surf_y(e['x'], e['z']) + 50)
    if g is None or g[1] == 10: chk('pad', 'spring pad at (%.0f %.0f) over %s' % (e['x'], e['z'], g))
print('CHECK totals', dict(NCHK) or 'none')

# ---------------- minimap (RGB5A3 128 x 256 like the slot's) ----------------
ZCOL = {'water': (70, 170, 220), 'deep': (30, 90, 170), 'lava': (240, 110, 20), 'pit': (40, 30, 30), 'sand': (230, 206, 150),
        'ice': (190, 230, 250), 'snow': (245, 248, 255), 'grass': (90, 160, 70), 'rock': (130, 110, 100)}
def zones_draw(d, P, alpha, sc):
    """the zones on a map (polygons, bands) in their colours"""
    for Z in ZONES:
        c = ZCOL.get(Z['kind'], (128, 128, 128)) + ((alpha,) if alpha else ())
        if Z.get('band'):
            Lf, Rt = band_sides(Z['band']); d.polygon([P(x, z) for x, z in Lf + Rt[::-1]], fill=c)
        elif Z.get('poly'): d.polygon([P(x, z) for x, z in Z['poly']], fill=c)
def struct_u16(b, o): return int.from_bytes(b[o:o + 2], 'big')
def map_bti():
    W_, H_ = 128, 256
    im = Image.new('RGBA', (W_, H_), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    xs = [p[0] for p in WL]; zs = [p[1] for p in WL]
    sc = min((W_ - 8) / (max(zs) - min(zs)), (H_ - 8) / (max(xs) - min(xs)))
    P = lambda x, z: (4 + (z - min(zs)) * sc, 4 + (max(xs) - x) * sc)
    d.polygon([P(x, z) for x, z in WL], fill=tuple(TH['ground'][0]) + (255,), outline=(250, 250, 250, 255))
    zones_draw(d, P, 255, sc)
    for pl, closed, w in ROADS:
        pts = [P(x, z) for x, z in smooth(pl, closed, 700)]
        d.line(pts + (pts[:1] if closed else []), fill=tuple(TH['roadcol']) + (255,), width=max(2, int(w * sc)))
    for F in T['features']:
        if F['t'] != 'strip' or F.get('ghost'): continue
        cx, cz = F['c']; a = math.radians(F['ang']); dx, dz = math.cos(a), math.sin(a)
        us = [p[0] for pc in F['pieces'] for p in pc['p'] if pc.get('mat') != 'none'] + [u for pad in F.get('pads', ()) for u in pad]
        if not us: continue
        d.line([P(cx + dx * min(us), cz + dz * min(us)), P(cx + dx * max(us), cz + dz * max(us))], fill=(240, 130, 30, 255), width=max(2, int(F['w'] * sc)))
    im.save(A7 + '/map_preview.png')
    tm = base.root.files[NAME + '_map.bti']; tm.seek(0); t = bytearray(tm.read())
    assert t[0] == 5 and struct_u16(t, 2) == W_ and struct_u16(t, 4) == H_, 'unexpected map format'
    off = int.from_bytes(t[0x1c:0x20], 'big'); px = im.load(); o = bytearray()
    for by in range(0, H_, 4):
        for bx in range(0, W_, 4):
            for y in range(4):
                for x in range(4):
                    r, g, b, a = px[bx + x, by + y]
                    v = (0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)) if a >= 0xE0 else ((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4))
                    o += v.to_bytes(2, 'big')
    t[off:off + len(o)] = o
    return bytes(t)
MAP = map_bti()

mkarc.set_file(base, NAME + '_course.bco', BCO_DATA)
mkarc.set_file(base, NAME + '_course.bmd', BMD)
mkarc.set_file(base, NAME + '_course.bol', BOL_DATA)
if HAS_SKY: mkarc.set_file(base, NAME + '_sky.bmd', SKY)
mkarc.set_file(base, NAME + '_map.bti', MAP)
for k in (NAME + '_course.btk', NAME + '_course.brk', NAME + '_sky.btk', NAME + '_sky.brk'): base.root.files.pop(k, None)
os.makedirs(OUT + '/Course', exist_ok=True)
tmp = OUT + '/Course/_tmp_' + OFILE
mkarc.save(base, tmp)
os.replace(tmp, OUT + '/Course/' + OFILE)

# top-down preview (and the thumbnail camera's data)
S = max((AX1 - AX0) / 760, (AZ1 - AZ0) / 500)
pv = Image.new('RGB', (800, 540), (40, 40, 40)); d = ImageDraw.Draw(pv)
Pp = lambda x, z: ((x - CX) / S + 400, (z - CZ) / S + 270)
d.polygon([Pp(x, z) for x, z in WL], fill=tuple(TH['ground'][0]))
zones_draw(d, Pp, None, 1 / S)
for pl, closed, w in ROADS:
    pts = [Pp(x, z) for x, z in smooth(pl, closed, 700)]
    d.line(pts + (pts[:1] if closed else []), fill=tuple(TH['roadcol']), width=max(2, int(w / S)))
for F in T['features']:
    if F.get('ghost'): continue
    if F['t'] == 'strip':
        cx, cz = F['c']; a = math.radians(F['ang']); dx, dz = math.cos(a), math.sin(a); nx_, nz_ = -dz, dx; w2 = F['w'] / 2
        for pc in F['pieces']:
            if pc.get('mat') == 'none': continue
            us = [p[0] for p in pc['p']]; u0, u1 = min(us), max(us)
            col = (240, 130, 30) if pc.get('mat') == 'kick' else (120, 80, 50)
            d.polygon([Pp(cx + dx * u + nx_ * v, cz + dz * u + nz_ * v) for u, v in ((u0, -w2), (u1, -w2), (u1, w2), (u0, w2))], fill=col)
    else:
        cx, cz = F['c']
        pts = [Pp(cx + r * math.cos(math.radians(a)), cz + r * math.sin(math.radians(a))) for r, a in [(F['r0'], F['a0'] + (F['a1'] - F['a0']) * i / 12) for i in range(13)] + [(F['r1'], F['a1'] - (F['a1'] - F['a0']) * i / 12) for i in range(13)]]
        d.polygon(pts, fill=(150, 100, 60))
for pl, closed in FENCES:
    pts = [Pp(x, z) for x, z in smooth(pl, closed, 900)]
    d.line(pts + (pts[:1] if closed else []), fill=(230, 230, 230), width=2)
for o in mb.objects.objects: x, z = o.position.x, o.position.z; d.rectangle((*Pp(x - 150, z - 150), *Pp(x + 150, z + 150)), fill=(230, 30, 30))
for c in coins: x, z = c[0], c[2]; d.ellipse((*Pp(x - 150, z - 150), *Pp(x + 150, z + 150)), fill=(250, 220, 40))
for kq in mb.kartpoints.positions: x, z = kq.position.x, kq.position.z; d.rectangle((*Pp(x - 200, z - 200), *Pp(x + 200, z + 200)), fill=(40, 200, 60))
d.line([Pp(x, z) for x, z in dense + dense[:1]], fill=(255, 255, 255), width=1)
d.text((8, 8), T['title'], fill=(255, 255, 255))
pv.save(A7 + '/preview.png')
json.dump(dict(arena=T['arena'], cam=T.get('cam'), title=T['title']), open(A7 + '/meta.json', 'w'))
print('saved', OUT + '/Course/' + OFILE, 'missing flags', len(NOCOL), NOCOL[:12], 'seams', dict(SEAM))
