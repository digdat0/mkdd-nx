"""Checks Trick Style track data against invariants (starts, respawns, dead zones, boxes, coins, fx, routes vs. the
collision). usage: py -3.10 trackval.py <app folder holding mods/... | build7 out folder holding Course/>
Exits 1 on a hard problem (anything but the informational route/coin notes)."""
import sys, io, glob, os, math, collections, re, itertools
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from lib import rarc, libbol
import bcolib

APP = sys.argv[1]
NAMES = dict(l.split() for l in open(os.path.join(HERE, "cups8.txt")) if l.strip())
issues = collections.Counter()

def say(t, msg):
    issues[t] += 1
    print(f"  {t}: {msg}")

def arcfiles(p):
    a = rarc.Archive.from_file(open(p, "rb")); out = {}
    def w(d):
        for n, f in d.files.items(): f.seek(0); out[n] = f.read()
        for s in d.subdirs.values(): w(s)
    w(a.root); return out

# route/coins/fx by track name (headers name the build7 track)
keyfiles = {}
OUTL = os.path.isdir(os.path.join(APP, "Course"))
for kind, d in ((("routes", "routes"), ("coins", "coins"), ("fx", "fx")) if OUTL else (("routes", "mods/routes"), ("coins", "mods/coins"), ("fx", "mods/trick/fx"))):
    for p in glob.glob(os.path.join(APP, d, "3a_*.txt")):
        m = re.search(r"build7\.py (\w+)", open(p).readline())
        if m: keyfiles.setdefault(m.group(1), {})[kind] = p

class Col:
    def __init__(s, b):
        s.b = b; s.idx = collections.defaultdict(list)
        for ti, t in enumerate(b.tris):
            ps = t['p0']
            for i in range(int(min(p[0] for p in ps) // 500), int(max(p[0] for p in ps) // 500) + 1):
                for j in range(int(min(p[2] for p in ps) // 500), int(max(p[2] for p in ps) // 500) + 1):
                    s.idx[(i, j)].append(ti)
    def hits(s, x, z):
        """[(y, typ, normal_y)] of every triangle over (x, z)"""
        out = []
        for ti in s.idx.get((int(x // 500), int(z // 500)), ()):
            t = s.b.tris[ti]; a, b, c = t['p0']
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-9: continue
            l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) < -1e-4: continue
            out.append((l1 * a[1] + l2 * b[1] + l3 * c[1], t['typ'], bcolib.tri_normal(a, b, c)[1]))
        return out
    def ground(s, x, y, z, up=400, down=3000):
        """the highest walkable surface from y+up down to y-down: (y, typ) or None"""
        c = [(gy, typ) for gy, typ, ny in s.hits(x, z) if abs(ny) >= 0.7 and y - down <= gy <= y + up]
        return max(c) if c else None
    def wall_near(s, x, y, z, r):
        """nearest steep triangle vertex-span within r at kart height (rough: samples a ring)"""
        for k in range(16):
            a = k * math.pi / 8
            for gy, typ, ny in s.hits(x + r * math.cos(a), z + r * math.sin(a)):
                if abs(ny) < 0.7 and y - 50 < gy < y + 300: return True
        return False

def kind(typ):
    h = typ >> 8
    return {8: 'boost', 2: 'wall', 10: 'dead', 3: 'offroad?', 0x0c: 'sand/water', 4: 'ice', 1: 'ground', 0: 'snow'}.get(h, hex(typ))

for p in sorted(glob.glob(os.path.join(APP, "Course/T*.arc" if OUTL else "mods/trick/tracks/Course/T*.arc"))):
    T = os.path.basename(p)[:3]; name = NAMES[T]
    print(f"== {T} {name}")
    F = arcfiles(p)
    bol = libbol.BOL.from_file(io.BytesIO(next(v for k, v in F.items() if k.endswith("_course.bol"))))
    col = Col(bcolib.read(next(v for k, v in F.items() if k.endswith("_course.bco"))))
    rids = {r.respawn_id for r in bol.respawnpoints}
    # dead zones reference respawn ids that exist
    dz = collections.Counter(t['typ'] & 0xff for t in col.b.tris if t['typ'] >> 8 == 0x0a)
    for nn, cnt in dz.items():
        if nn and nn not in rids: say("deadzone", f"{cnt} dead-zone tris send karts to respawn {nn}, BOL has {sorted(rids) or 'none'}")
    if dz and not rids: say("deadzone", f"dead zones but no respawn points")
    # starts
    for k in bol.kartpoints.positions:
        x, y, z = k.position.x, k.position.y, k.position.z
        g = col.ground(x, y, z)
        if not g: say("start", f"start {k.playerid} ({x:.0f},{z:.0f}) has no ground under it")
        elif abs(g[0] - y) > 60: say("start", f"start {k.playerid} {y - g[0]:+.0f} off the ground")
        elif kind(g[1]) in ('boost', 'dead'): say("start", f"start {k.playerid} on {kind(g[1])}")
        if col.wall_near(x, y, z, 500): say("start", f"start {k.playerid} ({x:.0f},{z:.0f}) within 500 of a wall")
    # respawns
    for r in bol.respawnpoints:
        x, y, z = r.position.x, r.position.y, r.position.z
        g = col.ground(x, y, z, up=600, down=6000)
        if not g: say("respawn", f"respawn {r.respawn_id} ({x:.0f},{z:.0f}) has no ground under it")
        elif kind(g[1]) in ('boost', 'dead'): say("respawn", f"respawn {r.respawn_id} over {kind(g[1])}")
    rp = [(r.position.x, r.position.z) for r in bol.respawnpoints]
    for (i, a), (j, b) in itertools.combinations(enumerate(rp), 2):
        if math.hypot(a[0] - b[0], a[1] - b[1]) < 1500: say("respawn", f"respawns {i+1} and {j+1} only {math.hypot(a[0]-b[0], a[1]-b[1]):.0f} apart")
    # item boxes
    boxes = [o for o in bol.objects.objects if o.objectid == 1]
    for o in boxes:
        x, y, z = o.position.x, o.position.y, o.position.z
        g = col.ground(x, y, z, up=400, down=3000)
        if not g: say("box", f"item box ({x:.0f},{y:.0f},{z:.0f}) has no ground below within 3000")
        elif kind(g[1]) == 'dead': say("box", f"item box ({x:.0f},{z:.0f}) over a dead zone")
        elif y - g[0] > 1200: say("box", f"item box ({x:.0f},{z:.0f}) {y - g[0]:.0f} above the ground")
    bp = [(o.position.x, o.position.y, o.position.z) for o in boxes]
    for a, b in itertools.combinations(bp, 2):
        if math.dist(a, b) < 150: say("box", f"item boxes overlap at ({a[0]:.0f},{a[2]:.0f})")
    kf = keyfiles.get(name, {})
    if not kf.get("routes"): say("files", f"no route file for {name}")
    # route
    if kf.get("routes"):
        pts = [tuple(map(float, l.split())) for l in open(kf["routes"]) if l[:1] not in "#>" and l.strip()]
        offg = wall = 0; prev = None; dup = 0
        for i, (x, y, z) in enumerate(pts):
            g = col.ground(x, y, z, up=300, down=600)
            if not g: offg += 1
            elif kind(g[1]) == 'dead': say("route", f"route point {i} ({x:.0f},{z:.0f}) over a dead zone")
            if col.wall_near(x, y, z, 700): wall += 1
            # overhead surface just above the route (holds karts under it)
            if any(y + 150 < gy < y + 900 for gy, typ, ny in col.hits(x, z)): say("route", f"route point {i} ({x:.0f},{z:.0f}) has a surface 150-900 above it")
            if prev and math.hypot(x - prev[0], z - prev[2]) < 100: dup += 1
            prev = (x, y, z)
        if offg: say("route", f"{offg}/{len(pts)} route points with no ground within 300 up / 600 down (jumps are expected)")
        if wall: say("route", f"{wall}/{len(pts)} route points within 700 of a wall")
        if dup: say("route", f"{dup} route points under 100 from the previous one")
    # coins
    if kf.get("coins"):
        cs = [tuple(map(float, l.split()[:3])) for l in open(kf["coins"]) if l[:1] != "#" and l.strip()]
        for x, y, z in cs:
            g = col.ground(x, y, z, up=400, down=8000)
            if not g: say("coin", f"coin ({x:.0f},{y:.0f},{z:.0f}) has no ground below")
            elif kind(g[1]) == 'dead': say("coin", f"coin ({x:.0f},{z:.0f}) over a dead zone")
        for a, b in itertools.combinations(cs, 2):
            if math.dist(a, b) < 60: say("coin", f"coins overlap at ({a[0]:.0f},{a[2]:.0f})")
    # fx
    if kf.get("fx"):
        for l in open(kf["fx"]):
            if l[:1] == "#" or not l.strip(): continue
            f = l.split(); t = f[0]; x, y, z = map(float, f[1:4])
            g = col.ground(x, y, z, up=300, down=600)
            if not g: say("fx", f"{t} ({x:.0f},{y:.0f},{z:.0f}) has no ground at its height")
            elif kind(g[1]) == 'dead': say("fx", f"{t} ({x:.0f},{z:.0f}) over a dead zone")
            if t in ('pad', 'geyser') and col.wall_near(x, y, z, 400): say("fx", f"{t} ({x:.0f},{z:.0f}) within 400 of a wall")

print("\nTOTAL", dict(issues))
# route and coin notes are informational (jumps over gaps, coin arcs over pits); the rest must be fixed before deploying
HARD = {k: v for k, v in issues.items() if k not in ("route", "coin")}
if HARD:
    sys.exit(f"FAIL: {HARD}")
print("OK")
