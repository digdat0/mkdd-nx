"""Battle routes for the computer's karts, made from RECORD BATTLE ROUTE's recordings (runtime/route.c).

usage: py -3 tools/battle_routes.py <routes dir> [--key 3a_99e0cbaf] [--seed N] [--open KEYS] [--game IMAGE] [--mod DIR] [--variants N] [--install]

<routes dir> is the repository's routes (built into the .nro) or the SD card's switch/MKDD-NX/routes
(recordings in its rec/). For each arena (key:
course ID _ BOL hash), all its recordings make a map of where karts drove: the floor in cells of
CELL units (in layers, for floors above floors), the moves between cells, the cells at the edge
(next to a wall, a drop or a part never driven). From that, a network of routes round the arena,
kept off the walls: junctions spread over the arena and parts between them until the routes cover
it (make_network); each computer's kart picks its next part at random at each junction, so they
spread out. An arena's file holds --variants different networks (default VARIANTS); the game picks
one at random each battle. Written to
<routes dir>/net/<key>.txt (with --install to <routes dir>/<key>.txt, the file the game reads, any
file there not made by these tools kept as <key>_old.txt) and a picture of the map and routes,
<routes dir>/net/<key>.svg. tools/route_editor.py edits them, and the map: floor added where
nobody drove (by hand or from the game's collision, game_collision) and taken away, in
<routes dir>/map/<key>.txt (read_map), used here too. --game IMAGE makes the map from the game's
collision instead (game_map: the real road and walls, read from your disc image and never kept);
the recordings then only give the pipes' warps. --mod DIR does the same for a mod's arenas (MKDD Plus:
the folder with files and sys in it; its files/Cours*/Mini*.arc), with or without --game.

A recording's lines: "frame x y z fx fy fz", "-" a respawn (older "x y z" loops are read too).
"""
import argparse, glob, heapq, math, os, random, re, sys
from collections import Counter, defaultdict

CELL = 400.0        # floor cell size (x, z)
LAYER = 300.0       # heights further apart than this in a cell: separate floors
RESPAWN_CUT = 120   # frames before a respawn dropped (the fall and Lakitu's carry)
WARP_FRAMES = 30    # a break this soon after the last point: a warp (a pipe), not a respawn
CLIMB = 200.0       # height between two cells a kart drives up: more is a one-way drop
FLAT = 100.0        # height between neighbouring cells joined without a recorded move
FILL_NEAR = 6       # driven neighbours (of 8) that make a gap between passes floor
FLOOR_SHARE = 0.08  # of the hits at a height: a floor of its own in the picture
SPACING = 700.0     # between a route's points
MAX_POINTS = 512    # ROUTE_MAX (runtime/route.h): all of an arena's parts
MAX_TURN = 75.0     # degrees: a part turning more at a point is thrown away
LEAD = 300.0        # a junction's lead in and out: parts meet heading the same way
JUNCTION_GAP = 4500.0  # between the first junctions, spread over the arena (more come where no route reaches)
OUTS = (2, 3)       # parts wanted from a junction at first (the game takes 4 at most; from junction 0 one)
TURN_COST = 1.5     # a 45-degree turn on a path costs as much as this many cells of way
COVER_R = 1000.0    # a cell this near a route (on its floor) is covered
COVER_DONE = 0.97   # of the arena's cells covered: enough
DETOURS = 8         # the cheapest ways through a place no route reaches tried
MAX_JUNCTIONS = 32
FEWER_JUNCTIONS = (20, 14)  # an arena too big for MAX_POINTS with MAX_JUNCTIONS (MKDD Plus's N64 Block Fort): tried with these
NETWORKS = 4        # tries for a network, the best kept (most of the arena covered, then most choices)
VARIANTS = 3        # networks in an arena's file: the game picks one at random each battle
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]  # headings, 45 degrees apart in turn
WANT_WALLS = 900.0  # from the nearest wall (or drop) a route keeps to where it can, walled arenas
MIN_WALLS = 500.0   # a part with a point nearer a wall than this is thrown away
WANT_OPEN = 450.0   # the same in --open arenas: their routes go closer to the edges
MIN_OPEN = 300.0
MIN_LOW = 150.0     # where few parts keep the least, the least it may go to (a lane driven once: its middle)
FINE = 100.0        # the distances to the walls: a square this size
HOLE_MIN = 3        # places with no floor together inside an arena that make a wall (fewer: floor not driven)
DENSE = 200.0       # between a part's points while it is pushed off the walls
PUSH_PASSES = 8
PUSH_STEP = 100.0   # units a point moves a pass, away from a wall it is too near
LAND_RUN = 1500.0   # after a pipe: how far past touching down the kart counts as landed
APPROACH = 1200.0   # before a pipe: where the straight run into it starts
PIPE_IN = 300.0     # the run's last point: this far past the last point recorded before the warp
SAME_NEAR = 500.0   # a part with SAME_SHARE of its points this near another's between the same junctions: the same, thrown away
SAME_SHARE = 0.8
CUT_MAX = 24        # cells: a path coming back to a cell after at most this many turned back (cut out)
RING8 = [(dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1) if dx or dz]
MAP_HEADER = "# MKDD-NX battle map edits (tools/route_editor.py)"
ROAD = {0x01, 0x04, 0x13}   # collision types (a triangle's type's high byte) karts drive on (0x04: ice it seems, MKDD Plus's Sherbet Rink and SNES Battle Course 3; 0x13: Tilt-a-Kart's rim)
STEP_UP = 300.0       # height between neighbouring cells of the game's road counted as one floor (fill)
# the map from the game's collision (--game): its walls are where they are, so routes go nearer them
GAME_WANT, GAME_MIN = 600.0, 300.0
GAME_STEEP = 0.5      # a triangle whose normal's height is less than this (of 1): a wall, whatever its type
GAME_HEAD = 150.0     # a wall reaching this far above the road (and more than 20 above it) stands on it
GAME_SLOPE = 100.0    # height between next FINE squares one surface: more is a step or a drop (an edge)
GAME_CELL_MIN = 6     # of a cell's 16 FINE squares free at a height: a cell of floor there
HEADER = "# MKDD-NX battle routes (tools/battle_routes.py)"


def landing(pts):
    """after a warp (frame, x, y, z points): the index of the first point on the floor again, and of
    the first LAND_RUN past it; None if the kart never came down"""
    down = False
    for k in range(1, len(pts)):
        dy = pts[k][2] - pts[k - 1][2]
        if dy < -30:
            down = True
        elif down:
            run = 0.0
            for j in range(k, len(pts)):
                run += math.dist(pts[j][1:], pts[j - 1][1:])
                if run >= LAND_RUN:
                    return k - 1, j
            return None
    return None


def read_recording(path):
    """segments (lists of x, y, z; a new one at each break) and warps ((entry, the point before it,
    exit, landing): breaks with no fall before; the flight out dropped from the segments)"""
    segs, cur = [], []
    with open(path) as f:
        for i, line in enumerate(f):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line == "-":
                if cur:
                    segs.append((cur, True))
                cur = []
                continue
            v = line.split()
            try:
                if len(v) >= 7:
                    cur.append((int(v[0]), float(v[1]), float(v[2]), float(v[3])))
                elif len(v) == 3:
                    cur.append((i, float(v[0]), float(v[1]), float(v[2])))
            except ValueError:
                pass
    if cur:
        segs.append((cur, False))
    out, warps = [], []
    for i, (pts, cut) in enumerate(segs):
        if cut and pts:
            end = pts[-1][0] - RESPAWN_CUT
            nxt = segs[i + 1][0] if i + 1 < len(segs) else []
            # a warp (a pipe): at once; else a respawn (Lakitu takes seconds): drop the fall before it
            if nxt and nxt[0][0] - pts[-1][0] < WARP_FRAMES:
                land = landing(nxt) if len(pts) > 1 else None
                if land:
                    warps.append((pts[-1][1:], pts[-2][1:], nxt[0][1:], nxt[land[1]][1:]))
                    segs[i + 1] = (nxt[land[0]:], segs[i + 1][1])
            else:
                pts = [p for p in pts if p[0] < end]
        if len(pts) >= 2:
            out.append([p[1:] for p in pts])
    return out, warps


class Map:
    def __init__(self, one_floor=False, open_floor=False, ground=0.0):
        self.open = open_floor  # no holes: all inside the outline is floor (at the lowest floor's height, ground)
        self.ground = ground
        # one floor (a tilting one too): heights never separate cells, drops or neighbours
        self.layer, self.flat, self.climb = (1e9, 1e9, 1e9) if one_floor else (LAYER, FLAT, CLIMB)
        self.layers = defaultdict(list)  # (ix, iz) -> [[sum x, sum y, sum z, hits, passes]]
        self.moves = defaultdict(int)    # (cell, cell) -> times driven
        self.trace = []                  # segments for the picture
        self.warps = []                  # (entry, before it, exit, landing), from read_recording
        self.pipes = []                  # the warps by pipe (build)
        self.added, self.removed = [], []  # the map edits (read_map): (ix, iz, height) each
        self.want, self.min = (WANT_OPEN, MIN_OPEN) if open_floor else (WANT_WALLS, MIN_WALLS)
        self.surface = None  # the game's map (game_surface): {(fx, fz): [heights]} free FINE squares, or None
        self.fine3 = None    # its distances to the walls: {(fx, fz): [(height, units)]}

    def cell(self, x, y, z, add=None):
        k = (math.floor(x / CELL), math.floor(z / CELL))
        ls = self.layers[k]
        for i, l in enumerate(ls):
            if abs(l[1] / l[3] - y) < self.layer:
                break
        else:
            ls.append([0.0, 0.0, 0.0, 0, set()])
            i = len(ls) - 1
        if add is not None:
            l = ls[i]
            l[0] += x
            l[1] += y
            l[2] += z
            l[3] += 1
            l[4].add(add)
        return k + (i,)

    def add_segment(self, pts, pass_id):
        self.trace.append(pts)
        prev = None
        for a, b in zip(pts, pts[1:]):
            n = max(1, int(math.dist(a, b) / (CELL / 2)))
            for s in range(n):
                t = s / n
                p = [a[j] + (b[j] - a[j]) * t for j in range(3)]
                c = self.cell(*p, add=pass_id)
                if prev is not None and c != prev:
                    self.moves[(prev, c)] += 1
                prev = c
        c = self.cell(*pts[-1], add=pass_id)
        if prev is not None and c != prev:
            self.moves[(prev, c)] += 1

    def build(self):
        self.cells = {}
        for k, ls in self.layers.items():
            for i, l in enumerate(ls):
                if l[3]:
                    self.cells[k + (i,)] = (l[0] / l[3], l[1] / l[3], l[2] / l[3], l[3], len(l[4]))
        self.edit()
        grid = {c[:2] for c in self.cells}
        outside = self.outside(grid)
        if self.surface is not None:
            self.join_game()
        else:
            if self.open:
                self.fill_inside(grid, outside)
            self.fill()
        for c in self.painted:  # floor added joins its neighbours on the same floor (no moves recorded there)
            for dx, dz in RING8:
                d = self.near(c, dx, dz, self.climb)
                if d:
                    self.moves[(c, d)] += 1
                    self.moves[(d, c)] += 1
        self.edges = defaultdict(set)
        for (a, b), n in self.moves.items():
            if a not in self.cells or b not in self.cells:
                continue
            self.edges[a].add(b)
            if self.cells[a][1] - self.cells[b][1] < self.climb:  # not a drop: drivable back too
                self.edges[b].add(a)
        # the same floor all round: neighbouring cells join though never driven between
        self.interior = {c for c in self.cells if all(self.near(c, dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1))}
        for c in self.interior:
            for dx, dz in ((1, 0), (0, 1), (1, 1), (1, -1)):
                d = self.near(c, dx, dz)
                if d in self.interior:
                    self.edges[c].add(d), self.edges[d].add(c)
        # how many cells to the nearest edge cell (0 an edge cell)
        self.depth = {c: (1 if c in self.interior else 0) for c in self.cells}
        for _ in range(4):
            nxt = dict(self.depth)
            for c in self.interior:
                ns = [self.near(c, dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1) if dx or dz]
                nxt[c] = 1 + min(self.depth.get(n, 0) for n in ns)
            self.depth = nxt
        # how far each point is from the nearest wall (units, FINE squares): the walls are the places
        # with no floor round the arena and the holes in it of HOLE_MIN places or more (a smaller one is
        # floor between passes); seen from above, so a floor's edge over another floor is no wall
        grid = {c[:2] for c in self.cells}
        x0, x1 = min(k[0] for k in grid), max(k[0] for k in grid)
        z0, z1 = min(k[1] for k in grid), max(k[1] for k in grid)
        walls, seen = set(outside), set(outside) | grid
        for x in range(x0, x1 + 1):
            for z in range(z0, z1 + 1):
                if (x, z) in seen:
                    continue
                part, todo = {(x, z)}, [(x, z)]
                while todo:
                    px, pz = todo.pop()
                    for k in ((px + 1, pz), (px - 1, pz), (px, pz + 1), (px, pz - 1)):
                        if x0 <= k[0] <= x1 and z0 <= k[1] <= z1 and k not in seen and k not in part:
                            part.add(k), todo.append(k)
                seen |= part
                if len(part) >= (1 if self.surface is not None else HOLE_MIN):  # (the game's holes are all real)
                    walls |= part
        self.walls, self.floor = walls, seen - walls
        self.fine, n = {}, round(CELL / FINE)
        if self.surface is not None:
            self.fine3 = self.game_clear()
            self.clear = {c: max(0.0, clear_at(self, v[:3])) for c, v in self.cells.items()}
            self.main = max(self.parts(), key=len, default=set())
            self.find_pipes()
            return
        # units from each FINE square's middle (on the floor: not a wall) to the nearest wall place, at
        # most 5 cells
        near = [(dx, dz) for dx in range(-5, 6) for dz in range(-5, 6)]
        for k in self.floor:
            ws = [(k[0] + dx, k[1] + dz) for dx, dz in near if (k[0] + dx, k[1] + dz) in walls]
            for i in range(n):
                for j in range(n):
                    px, pz = k[0] * CELL + (i + 0.5) * FINE, k[1] * CELL + (j + 0.5) * FINE
                    self.fine[(k[0] * n + i, k[1] * n + j)] = min(
                        (math.hypot(max(w[0] * CELL - px, 0.0, px - (w[0] + 1) * CELL), max(w[1] * CELL - pz, 0.0, pz - (w[1] + 1) * CELL)) for w in ws),
                        default=5 * CELL)
        self.clear = {c: self.fine[(c[0] * n + n // 2, c[1] * n + n // 2)] for c in self.cells}
        # the largest part every cell can reach and come back from
        self.main = max(self.parts(), key=len, default=set())
        self.find_pipes()

    def edit(self):
        """the map edits: floor taken away (and never filled in again), then floor added where there is
        none near its height (hits 1: a junction may go there)"""
        self.painted, self.erased = set(), set()
        on = lambda k, y: [k + (i,) for i in range(len(self.layers.get(k, ()))) if k + (i,) in self.cells and abs(self.cells[k + (i,)][1] - y) < self.layer]
        for x, z, y in self.removed:
            for c in on((x, z), y):
                del self.cells[c]
            self.erased.add((x, z))
        for x, z, y in self.added:
            k = (x, z)
            if on(k, y):
                continue
            self.layers[k].append([0.0, 0.0, 0.0, 0, set()])
            c = k + (len(self.layers[k]) - 1,)
            self.cells[c] = ((x + 0.5) * CELL, y, (z + 0.5) * CELL, 1, 1)
            self.painted.add(c)

    def free_at(self, x, y, z, tol=STEP_UP):
        """the game's map: the height of the free surface at (x, z) nearest y (within tol), or None"""
        hs = self.surface.get((math.floor(x / FINE), math.floor(z / FINE)), ())
        best = min(hs, key=lambda h: abs(h - y), default=None)
        return best if best is not None and abs(best - y) < tol else None

    def join_game(self):
        """the game's map: next cells on one floor joined both ways where the straight way between
        their middles is free surface all along (no wall, step or gap between)"""
        for c, v in self.cells.items():
            for dx, dz in RING8:
                d = self.near(c, dx, dz, self.climb)
                if not d or (c, d) in self.moves:
                    continue
                w = self.cells[d]
                n = max(2, math.ceil(math.dist(v[:3:2], w[:3:2]) / (FINE / 2)))
                y = v[1]
                for s in range(n + 1):
                    t = s / n
                    y = self.free_at(v[0] + (w[0] - v[0]) * t, y, v[2] + (w[2] - v[2]) * t, GAME_SLOPE * 1.5)
                    if y is None:
                        break
                else:
                    self.moves[(c, d)] += 1
                    self.moves[(d, c)] += 1

    def game_clear(self):
        """the game's map: units from each free FINE square (each height) to the nearest edge of the
        surface (a wall, a drop, a step), along the surface (so a ramp is no wall); at most 5 cells"""
        nodes = {(k, i): h for k, hs in self.surface.items() for i, h in enumerate(hs)}
        links = {}
        for (k, i), h in nodes.items():
            out = []
            for dx, dz in RING8:
                hs = self.surface.get((k[0] + dx, k[1] + dz), ())
                j = min(range(len(hs)), key=lambda j: abs(hs[j] - h), default=None)
                if j is not None and abs(hs[j] - h) < GAME_SLOPE * math.hypot(dx, dz):
                    out.append(((k[0] + dx, k[1] + dz), j, FINE * math.hypot(dx, dz)))
            links[(k, i)] = out
        dist = {n: FINE / 2 for n, l in links.items() if len(l) < 8}  # an edge square: half a square from it
        q = [(d, n) for n, d in dist.items()]
        heapq.heapify(q)
        while q:
            d, n = heapq.heappop(q)
            if d > dist[n] or d > 5 * CELL:
                continue
            for k, j, step in links[n]:
                m = (k, j)
                if d + step < dist.get(m, 1e30):
                    dist[m] = d + step
                    heapq.heappush(q, (d + step, m))
        out = defaultdict(list)
        for (k, i), h in nodes.items():
            out[k].append((h, min(dist.get((k, i), 5 * CELL), 5 * CELL)))
        return out

    def find_pipes(self):
        """the warps by pipe (one exit each), those whose run in and landing are in the main part"""
        groups = []
        for w in self.warps:
            for g in groups:
                if math.dist(g[0][2], w[2]) < 600:
                    g.append(w)
                    break
            else:
                groups.append([w])
        for g in groups:
            avg = lambda i: tuple(sum(w[i][j] for w in g) / len(g) for j in range(3))
            entry, land = avg(0), avg(3)
            dx = sum(w[0][0] - w[1][0] for w in g)
            dz = sum(w[0][2] - w[1][2] for w in g)
            d = (math.copysign(1.0, dx), 0.0) if abs(dx) > abs(dz) else (0.0, math.copysign(1.0, dz))  # pipes face along an axis
            start = (entry[0] - d[0] * APPROACH, entry[1], entry[2] - d[1] * APPROACH)
            a, l = self.at(start), self.at(land)
            ok = a in self.main and l in self.main
            print(f"  pipe {entry[0]:.0f} {entry[1]:.0f} {entry[2]:.0f} -> {land[0]:.0f} {land[1]:.0f} {land[2]:.0f}: "
                  f"{len(g)} warps{'' if ok else ', not usable (its run in or landing is off the map)'}")
            if ok:
                self.pipes.append({"start": a, "land": l, "in": (entry[0] + d[0] * PIPE_IN, entry[1], entry[2] + d[1] * PIPE_IN)})

    def at(self, p):
        """the cell under point p (x, y, z), on the floor nearest its height, or None"""
        k = (math.floor(p[0] / CELL), math.floor(p[2] / CELL))
        best = None
        for i in range(len(self.layers.get(k, ()))):
            d = k + (i,)
            if d in self.cells and (best is None or abs(self.cells[d][1] - p[1]) < abs(self.cells[best][1] - p[1])):
                best = d
        return best if best is not None and abs(self.cells[best][1] - p[1]) < self.layer else None

    def outside(self, grid):
        """the cells round the arena: from beyond the cells driven, through cells never driven"""
        x0, x1 = min(k[0] for k in grid) - 1, max(k[0] for k in grid) + 1
        z0, z1 = min(k[1] for k in grid) - 1, max(k[1] for k in grid) + 1
        out, todo = {(x0, z0)}, [(x0, z0)]
        while todo:
            x, z = todo.pop()
            for k in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
                if x0 <= k[0] <= x1 and z0 <= k[1] <= z1 and k not in grid and k not in out:
                    out.add(k), todo.append(k)
        return out

    def fill_inside(self, grid, outside):
        """--open: every cell inside the outline never driven is floor, at the height of the nearest driven"""
        x0, x1 = min(k[0] for k in grid), max(k[0] for k in grid)
        z0, z1 = min(k[1] for k in grid), max(k[1] for k in grid)
        driven = [v for v in self.cells.values() if abs(v[1] - self.ground) < LAYER] or list(self.cells.values())
        for x in range(x0, x1 + 1):
            for z in range(z0, z1 + 1):
                k = (x, z)
                if k in grid or k in outside or k in self.erased:
                    continue
                cx, cz = (x + 0.5) * CELL, (z + 0.5) * CELL
                y = min(driven, key=lambda v: (v[0] - cx) ** 2 + (v[2] - cz) ** 2)[1]
                self.layers[k].append([0.0, 0.0, 0.0, 0, set()])
                self.cells[k + (len(self.layers[k]) - 1,)] = (cx, y, cz, 0, 0)

    def fill(self):
        """a cell never driven with FILL_NEAR of its 8 neighbours driven on one floor: the gap between
        two passes, counted as floor (no hits, so never a junction)"""
        ring = [(dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1) if dx or dz]
        cand = defaultdict(list)
        for c, v in self.cells.items():
            for dx, dz in ring:
                cand[(c[0] + dx, c[1] + dz)].append(v)
        for k, vs in cand.items():
            if k in self.erased or any(k + (i,) in self.cells for i in range(len(self.layers.get(k, ())))):
                continue
            vs.sort(key=lambda v: v[1])
            for i in range(len(vs)):  # the neighbours on one floor: within FLAT of the lowest
                group = [v for v in vs[i:] if v[1] - vs[i][1] < self.flat]
                if len(group) >= FILL_NEAR:
                    y = sum(v[1] for v in group) / len(group)
                    self.layers[k].append([0.0, 0.0, 0.0, 0, set()])
                    self.cells[k + (len(self.layers[k]) - 1,)] = ((k[0] + 0.5) * CELL, y, (k[1] + 0.5) * CELL, 0, 0)
                    break

    def near(self, c, dx, dz, tol=None):
        """the cell next to c (dx, dz) on the same floor (within tol, default FLAT, of its height), or None"""
        k = (c[0] + dx, c[1] + dz)
        y = self.cells[c][1]
        tol = self.flat if tol is None else tol
        best = None
        for i, l in enumerate(self.layers.get(k, ())):
            d = k + (i,)
            if d in self.cells and abs(self.cells[d][1] - y) < tol and (best is None or abs(self.cells[d][1] - y) < abs(self.cells[best][1] - y)):
                best = d
        return best

    def parts(self):
        """strongly connected parts (Tarjan, iterative)"""
        index, low, on, stack, out, n = {}, {}, set(), [], [], [0]
        for root in self.cells:
            if root in index:
                continue
            work = [(root, iter(self.edges[root]))]
            index[root] = low[root] = n[0]; n[0] += 1
            stack.append(root); on.add(root)
            while work:
                v, it = work[-1]
                for w in it:
                    if w not in index:
                        index[w] = low[w] = n[0]; n[0] += 1
                        stack.append(w); on.add(w)
                        work.append((w, iter(self.edges[w])))
                        break
                    if w in on:
                        low[v] = min(low[v], index[w])
                else:
                    work.pop()
                    if work:
                        low[work[-1][0]] = min(low[work[-1][0]], low[v])
                    if low[v] == index[v]:
                        part = set()
                        while True:
                            w = stack.pop(); on.discard(w); part.add(w)
                            if w == v:
                                break
                        out.append(part)
        return out

    def cost(self, c):
        short = max(0.0, self.want - self.clear[c]) / CELL  # nearer a wall than wanted
        return 1.0 + 3.0 * short * short + (1.0 if self.cells[c][4] < 2 else 0.0)

    def path(self, a, b, rng, noise):
        dist, back, q = {a: 0.0}, {}, [(0.0, a)]
        while q:
            d, c = heapq.heappop(q)
            if c == b:
                break
            if d > dist[c]:
                continue
            for n in self.edges[c]:
                if n not in self.main:
                    continue
                nd = d + self.cost(n) * math.dist(self.cells[c][:3:2], self.cells[n][:3:2]) / CELL * (1 + noise[n])
                if nd < dist.get(n, 1e30):
                    dist[n], back[n] = nd, c
                    heapq.heappush(q, (nd, n))
        if b not in dist:
            return None
        out = [b]
        while out[-1] != a:
            out.append(back[out[-1]])
        return out[::-1]


def smooth(p, closed, fixed=frozenset()):
    """one pass: each point to a quarter of each neighbour's way (an open path's ends and fixed stay)"""
    n = len(p)
    return [p[i] if i in fixed or (not closed and i in (0, n - 1)) else
            tuple((p[i - 1][j] + 2 * p[i][j] + p[(i + 1) % n][j]) / 4 for j in range(3)) for i in range(n)]


def resample(p, step, closed, most=None):
    """a point each step along p (an open path keeps both ends)"""
    n = len(p)
    seg = n if closed else n - 1
    at = [0.0]  # distance along to each point (a closed one's last back to the first)
    for i in range(seg):
        at.append(at[-1] + math.dist(p[i], p[(i + 1) % n]))
    k = max(8 if closed else 2, round(at[-1] / step))
    if most:
        k = min(k, most if closed else most - 1)
    out, i = [], 0
    for s in range(k if closed else k + 1):
        t = at[-1] * s / k
        while i < seg - 1 and at[i + 1] < t:
            i += 1
        a, b = p[i], p[(i + 1) % n]
        u = (t - at[i]) / (at[i + 1] - at[i]) if at[i + 1] > at[i] else 0.0
        out.append(tuple(a[j] + (b[j] - a[j]) * min(1.0, u) for j in range(3)))
    return out


def clear_at(m, p):
    """how far p is from the nearest wall, -1 off the map (the game's map: on the surface nearest p's
    height, or the highest when it is None)"""
    if m.fine3 is not None:
        hs = m.fine3.get((math.floor(p[0] / FINE), math.floor(p[2] / FINE)))
        if not hs:
            return -1.0
        if p[1] is None:
            return max(hs)[1]
        h, d = min(hs, key=lambda q: abs(q[0] - p[1]))
        return d if abs(h - p[1]) < LAYER else -1.0
    return m.fine.get((math.floor(p[0] / FINE), math.floor(p[2] / FINE)), -1.0)  # (only floor has one)


def push(m, p, fixed):
    """each point nearer a wall than wanted moves PUSH_STEP towards the cells round it further away"""
    out = list(p)
    for i, q in enumerate(p):
        here = clear_at(m, q)
        if i in fixed or here >= m.want:
            continue
        vx = vz = 0.0
        for dx, dz in RING8:
            gain = clear_at(m, (q[0] + dx * CELL / 2, q[1], q[2] + dz * CELL / 2)) - here
            if gain > 0:
                l = math.hypot(dx, dz)
                vx, vz = vx + gain * dx / l, vz + gain * dz / l
        l = math.hypot(vx, vz)
        if l:
            out[i] = (q[0] + vx / l * PUSH_STEP, q[1], q[2] + vz / l * PUSH_STEP)
    return out


def shape(m, chunks):
    """a part's points from chunks: ("cells", cells) driven, smoothed and pushed off the walls;
    ("pin", points) kept as they are (a junction and its lead); ("fixed", points) the same, and not
    held to the walls rule (a pipe's run in, into the wall); ("jump",) a pipe's
    warp, nothing joined across it. A point each SPACING (the ends and the points either side of a
    jump kept), with the indices just after a jump; None if a driven point stays nearer a wall than
    m.min"""
    p, fixed, free, cuts, loose = [], set(), set(), [], []
    for ch in chunks:
        if ch[0] == "jump":
            cuts.append(len(p))
            continue
        q = list(ch[1]) if ch[0] != "cells" else [m.cells[c][:3] for c in ch[1]]
        if ch[0] == "cells":
            for _ in range(2):
                q = smooth(q, False)
            if len(q) > 1:
                q = resample(q, DENSE, False)
        if ch[0] != "cells":
            fixed.update(range(len(p), len(p) + len(q)))
        if ch[0] == "fixed":
            loose += q
        else:
            free.update(range(len(p), len(p) + len(q)))
        p += q
    for k in cuts:  # (smoothing would join the two sides)
        fixed |= {k - 1, k}
    for _ in range(PUSH_PASSES):
        p = smooth(push(m, p, fixed), False, fixed)
    if any(clear_at(m, p[i]) < m.min for i in free):
        return None
    out, jumps = [], []
    for a, b in zip([0] + cuts, cuts + [len(p)]):
        if jumps or out:
            jumps.append(len(out))
        out += resample(p[a:b], SPACING, False) if b - a > 1 else p[a:b]
    if any(clear_at(m, q) < m.min for q in out if not any(math.dist(q, x) < DENSE for x in loose)):
        return None  # (between the points checked)
    return out, jumps


def turns(pts, jumps=()):
    """the sharpest turn (degrees) along an open path, none counted across a jump"""
    worst = 0.0
    for i in range(1, len(pts) - 1):
        if i in jumps or i + 1 in jumps:
            continue
        a, b, c = pts[i - 1], pts[i], pts[i + 1]
        u, v = (b[0] - a[0], b[2] - a[2]), (c[0] - b[0], c[2] - b[2])
        lu, lv = math.hypot(*u), math.hypot(*v)
        if lu and lv:
            worst = max(worst, math.degrees(math.acos(max(-1.0, min(1.0, (u[0] * v[0] + u[1] * v[1]) / (lu * lv))))))
    return worst


def uncross(route):
    """a cell twice not far apart: the part between is a there-and-back, cut out (further: the path
    crosses itself, kept)"""
    seen, out = {}, []
    for c in route:
        if c in seen and len(out) - seen[c] <= CUT_MAX:
            del out[seen[c] + 1:]
            seen = {d: i for i, d in enumerate(out)}
        else:
            seen[c] = len(out)
            out.append(c)
    return out


def turn(a, b):
    """45-degree steps between headings a and b (DIRS indices)"""
    return min((a - b) % 8, (b - a) % 8)


def unit(d):
    """heading d (DIRS) as a unit vector (x, z)"""
    x, z = DIRS[d]
    l = math.hypot(x, z)
    return x / l, z / l


def cover_cells(m, pts):
    """the cells of m.main within COVER_R of the line through pts, on its floor (not across a jump: a
    pipe's warp)"""
    out, r = set(), math.ceil(COVER_R / CELL)
    samples = []
    for a, b in zip(pts, pts[1:]):
        d = math.dist(a, b)
        n = 1 if d > 2 * SPACING else max(1, math.ceil(d / DENSE))
        samples += [tuple(a[j] + (b[j] - a[j]) * s / n for j in range(3)) for s in range(n)]
    samples += pts[-1:]
    for p in samples:
        kx, kz = math.floor(p[0] / CELL), math.floor(p[2] / CELL)
        for ix in range(kx - r, kx + r + 1):
            for iz in range(kz - r, kz + r + 1):
                for i in range(len(m.layers.get((ix, iz), ()))):
                    c = (ix, iz, i)
                    if c in out or c not in m.main:
                        continue
                    v = m.cells[c]
                    if abs(v[1] - p[1]) < m.layer and math.hypot(v[0] - p[0], v[2] - p[2]) <= COVER_R:
                        out.add(c)
    return out


def coverage(m, nets):
    """the share of the arena's cells (m.main) within COVER_R of a route: nets, lists of points"""
    got = set()
    for pts in nets:
        got |= cover_cells(m, pts)
    return len(got) / max(1, len(m.main))


def sccs(nodes, links):
    """the strongly connected parts of a small graph: nodes, links (a, b)"""
    out = {n: set() for n in nodes}
    for a, b in links:
        out[a].add(b)
    reach = {}
    for n in nodes:
        seen, todo = {n}, [n]
        while todo:
            for b in out[todo.pop()]:
                if b not in seen:
                    seen.add(b), todo.append(b)
        reach[n] = seen
    comps, done = [], set()
    for n in nodes:
        if n not in done:
            comp = {k for k in reach[n] if n in reach[k]}
            done |= comp
            comps.append(comp)
    return comps


class Planner:
    """one network on map m: junctions spread over the arena, each heading one way along the room it
    has (parts leave it ahead and reach it from behind, so a kart drives on through); parts from each
    to the nearest ahead, then more through whatever no part comes near, until COVER_DONE of the arena
    is covered or the game's room (points, parts at a junction) runs out. Paths turn at most 45
    degrees a cell (a search over cells and headings), so the karts can follow them"""

    def __init__(self, m, rng):
        self.m, self.rng = m, rng
        noise = {c: rng.random() * 0.6 for c in m.main}  # (each network its own: different ways)
        self.succ, self.pred = {}, defaultdict(list)
        for c in m.main:
            self.succ[c] = []
            for n in m.edges[c]:
                k = (n[0] - c[0], n[1] - c[1])
                if n in m.main and k in DIRS:
                    w = m.cost(n) * math.dist(m.cells[c][:3:2], m.cells[n][:3:2]) / CELL * (1 + noise[n])
                    self.succ[c].append((n, DIRS.index(k), w))
                    self.pred[(n, DIRS.index(k))].append((c, w))
        self.js, self.dead, self.parts = [], set(), []
        self.fwd, self.bwd = {}, {}
        self.count = Counter()  # cell -> parts covering it

    # --- junctions ---
    def junction(self, c):
        """a junction at cell c, heading along the way through it with the most room (either way), or None"""
        m, p = self.m, self.m.cells[c][:3]
        runs = []
        for d in range(4):
            ux, uz = unit(d)
            run = []
            for sign in (1, -1):
                k = 0
                while k < 5:
                    q = m.at((p[0] + sign * ux * (k + 1) * CELL, p[1], p[2] + sign * uz * (k + 1) * CELL))
                    if q not in m.main or m.clear[q] < m.min:
                        break
                    k += 1
                run.append(k)
            if min(run) >= 1:
                runs.append((d, min(run)))
        if not runs:
            return None
        d = self.rng.choices([r[0] for r in runs], [r[1] ** 2 for r in runs])[0] + self.rng.choice((0, 4))
        ux, uz = unit(d)
        lo, li = (p[0] + ux * LEAD, p[1], p[2] + uz * LEAD), (p[0] - ux * LEAD, p[1], p[2] - uz * LEAD)
        oc, ic = m.at(lo), m.at(li)
        if oc not in m.main or ic not in m.main:
            return None
        return {"cell": c, "d": d, "pos": p, "lead_out": lo, "lead_in": li, "start": (oc, d), "end": ic}

    def live(self):
        return [i for i in range(len(self.js)) if i not in self.dead]

    def outs(self, i):
        return sum(1 for q in self.parts if q[0] == i)

    def ins(self, j):
        return sum(1 for q in self.parts if q[1] == j)

    def free_out(self, i):
        return i not in self.dead and self.outs(i) < (1 if i == 0 else 4)

    def free_in(self, j):
        return j not in self.dead and self.ins(j) < 4

    # --- paths: over (cell, heading), a turn of 45 degrees at most a cell ---
    def forward(self, i):
        """from junction i's lead out: ({(cell, heading): cost}, the way back)"""
        if i not in self.fwd:
            s = self.js[i]["start"]
            dist, back, q = {s: 0.0}, {}, [(0.0, s)]
            while q:
                d0, st = heapq.heappop(q)
                if d0 > dist[st]:
                    continue
                for n, e, w in self.succ[st[0]]:
                    t = turn(st[1], e)
                    if t > 1:
                        continue
                    nd = d0 + w + TURN_COST * t
                    if nd < dist.get((n, e), 1e30):
                        dist[(n, e)], back[(n, e)] = nd, st
                        heapq.heappush(q, (nd, (n, e)))
            self.fwd[i] = (dist, back)
        return self.fwd[i]

    def backward(self, j):
        """to junction j's lead in, heading its way (within 45 degrees): ({(cell, heading): cost}, the
        way on)"""
        if j not in self.bwd:
            c, h = self.js[j]["end"], self.js[j]["d"]
            dist, nxt, q = {}, {}, []
            for e in ((h - 1) % 8, h, (h + 1) % 8):
                dist[(c, e)] = 0.0
                q.append((0.0, (c, e)))
            heapq.heapify(q)
            while q:
                d0, st = heapq.heappop(q)
                if d0 > dist[st]:
                    continue
                e = st[1]
                for c2, w in self.pred[st]:
                    for h2 in ((e - 1) % 8, e, (e + 1) % 8):
                        nd = d0 + w + TURN_COST * turn(h2, e)
                        if nd < dist.get((c2, h2), 1e30):
                            dist[(c2, h2)], nxt[(c2, h2)] = nd, st
                            heapq.heappush(q, (nd, (c2, h2)))
            self.bwd[j] = (dist, nxt)
        return self.bwd[j]

    def to(self, i, st):
        """the cells from junction i's lead out to state st"""
        back, out = self.forward(i)[1], [st]
        while out[-1] in back:
            out.append(back[out[-1]])
        return [s[0] for s in reversed(out)]

    def on(self, st, j):
        """the cells from state st to junction j's lead in"""
        nxt, out = self.backward(j)[1], [st]
        while out[-1] in nxt:
            out.append(nxt[out[-1]])
        return [s[0] for s in out]

    def near_other(self, cells, i, j):
        """a way passing another junction (it would go through it)"""
        for k in self.live():
            if k in (i, j):
                continue
            p = self.js[k]["pos"]
            for c in cells:
                v = self.m.cells[c]
                if abs(v[1] - p[1]) < LAYER and math.hypot(v[0] - p[0], v[2] - p[2]) < 1.5 * CELL:
                    return True
        return False

    # --- parts ---
    def part(self, i, j, cells, run=None):
        """the part from junction i to j along cells ((before, after) a pipe's run: run), shaped, or
        None (near a wall, turning too sharply, or the same as one there is)"""
        J, K = self.js[i], self.js[j]
        middle = ([("cells", uncross(cells[0])), ("fixed", run), ("jump",), ("cells", uncross(cells[1]))] if run
                  else [("cells", uncross(cells))])
        sh = shape(self.m, [("pin", [J["pos"], J["lead_out"]])] + middle + [("pin", [K["lead_in"]])])
        if not sh or turns(*sh) > MAX_TURN:
            return None
        if any(q[0] == i and q[1] == j and same(sh[0], q[2]) for q in self.parts):
            return None
        return (i, j) + sh

    def add(self, q):
        self.parts.append(q)
        self.count.update(cover_cells(self.m, q[2]))

    def new_cover(self, q):
        return len(cover_cells(self.m, q[2]) - set(self.count))

    def first_part(self, opts, make, need=0):
        """the first part made from the cheapest opts that covers need new cells: added, True"""
        for o in sorted(opts)[:DETOURS]:
            q = make(o)
            if q and (not need or self.new_cover(q) >= need):
                self.add(q)
                return True
        return False

    def direct(self, i, j):
        """the way from junction i straight to j: (cost, cells), or None"""
        s = self.js[i]["start"]
        cost = self.backward(j)[0].get(s)
        return None if cost is None else (cost, self.on(s, j))

    # --- the network ---
    def place(self):
        """junctions spread over the arena: each next among the furthest from those there are"""
        m, rng = self.m, self.rng
        cand = [c for c in m.main if m.clear[c] >= m.want] or [c for c in m.main if m.clear[c] >= m.min]
        if len(cand) < 4:
            return
        far = dict.fromkeys(cand, 1e30)
        pick = rng.choices(cand, [m.clear[c] for c in cand])[0]
        while len(self.js) < MAX_JUNCTIONS:
            J = self.junction(pick)
            if J:
                self.js.append(J)
            pv = m.cells[pick]
            for c in cand:
                v = m.cells[c]
                far[c] = min(far[c], math.hypot(v[0] - pv[0], v[2] - pv[2]) + 3 * abs(v[1] - pv[1]))
            top = max(far.values())
            if top < JUNCTION_GAP:
                break
            pool = [c for c in cand if far[c] >= 0.8 * top]
            pick = rng.choices(pool, [m.clear[c] for c in pool])[0]

    def link(self):
        """from each junction, parts to the nearest ahead (no other junction on the way)"""
        k = len(self.js)
        for i in self.rng.sample(range(k), k):
            want = 1 if i == 0 else self.rng.randint(*OUTS)
            opts = []
            for j in range(k):
                d = self.direct(i, j) if j != i else None
                if d and not self.near_other(d[1], i, j):
                    opts.append((d[0], j, d[1]))
            for _, j, cells in sorted(opts, key=lambda o: o[0]):
                if self.outs(i) >= want:
                    break
                q = self.part(i, j, cells) if self.free_in(j) else None
                if q:
                    self.add(q)

    def connect(self):
        """every junction reached from every other: parts between the strongly connected parts of the
        graph, the cheapest first; those still apart dropped (False: junction 0 with them)"""
        for _ in range(3 * MAX_JUNCTIONS):
            live = self.live()
            comps = sccs(live, [q[:2] for q in self.parts])
            if len(comps) == 1:
                return True
            grew = False
            for S in comps:
                rest = [j for j in live if j not in S]
                for leave in (True, False):
                    if any((q[0] in S) != (q[1] in S) and (q[0] in S) == leave for q in self.parts):
                        continue
                    opts = []
                    for a in S:
                        for b in rest:
                            i, j = (a, b) if leave else (b, a)
                            if self.free_out(i) and self.free_in(j):
                                d = self.direct(i, j)
                                if d:
                                    opts.append((d[0], i, j, d[1]))
                    grew |= self.first_part(opts, lambda o: self.part(o[1], o[2], o[3]))
            if not grew:
                break
        comps = sccs(self.live(), [q[:2] for q in self.parts])
        keep = max(comps, key=len)
        if 0 not in keep or len(keep) < 2:
            return False
        self.dead |= set(self.live()) - keep
        self.parts = [q for q in self.parts if q[0] in keep and q[1] in keep]
        self.count = Counter()
        for q in self.parts:
            self.count.update(cover_cells(self.m, q[2]))
        return True

    def pipes(self):
        """a part through each pipe, between the junctions it adds the least way between"""
        m = self.m
        for pipe in m.pipes:
            s = m.cells[pipe["start"]][:3]
            hd = round(math.atan2(pipe["in"][2] - s[2], pipe["in"][0] - s[0]) / (math.pi / 4)) % 8
            opts = []
            for i in self.live():
                if not self.free_out(i):
                    continue
                fd = self.forward(i)[0]
                a = min(((fd[(pipe["start"], h)], h) for h in range(8) if turn(h, hd) <= 1 and (pipe["start"], h) in fd), default=None)
                if not a:
                    continue
                for j in self.live():
                    bd = self.backward(j)[0]
                    b = min(((bd[(pipe["land"], h)], h) for h in range(8) if (pipe["land"], h) in bd), default=None)
                    if b and self.free_in(j):
                        opts.append((a[0] + b[0], i, j, a[1], b[1]))
            n = max(1, round(math.dist(s, pipe["in"]) / DENSE))
            run = [tuple(s[q] + (pipe["in"][q] - s[q]) * t / n for q in range(3)) for t in range(1, n + 1)]
            self.first_part(opts, lambda o: self.part(o[1], o[2], (self.to(o[1], (pipe["start"], o[3])),
                                                                   self.on((pipe["land"], o[4]), o[2])), run))

    def cover(self):
        """more parts through what no part comes near: through the place that covers most of what is
        left, between the junctions it adds the least way between (else a new junction there)"""
        m, rng = self.m, self.rng
        given_up = set()
        r = math.ceil(COVER_R / CELL)
        for _ in range(4 * MAX_JUNCTIONS):
            left = [c for c in m.main if c not in self.count]
            if len(left) <= (1 - COVER_DONE) * len(m.main) or sum(len(q[2]) for q in self.parts) >= MAX_POINTS:
                return
            lefts = set(left)
            targets = [c for c in left if m.clear[c] >= m.min and c not in given_up]
            if not targets:
                return

            def gain(c):
                v, n = m.cells[c], 0
                for ix in range(c[0] - r, c[0] + r + 1):
                    for iz in range(c[1] - r, c[1] + r + 1):
                        for i in range(len(m.layers.get((ix, iz), ()))):
                            d = (ix, iz, i)
                            if d in lefts:
                                w = m.cells[d]
                                n += abs(w[1] - v[1]) < m.layer and math.hypot(w[0] - v[0], w[2] - v[2]) <= COVER_R
                return n
            t = max(rng.sample(targets, min(60, len(targets))), key=lambda c: gain(c) * (0.8 + 0.4 * rng.random()))
            if not (self.detour(t) or self.new_junction(t)):
                v = m.cells[t]
                given_up |= {c for c in targets if math.hypot(m.cells[c][0] - v[0], m.cells[c][2] - v[2]) < 2 * CELL}

    def detour(self, t):
        opts = []
        for i in self.live():
            if not self.free_out(i):
                continue
            fd = self.forward(i)[0]
            for h in range(8):
                a = fd.get((t, h))
                if a is None:
                    continue
                for j in self.live():
                    b = self.backward(j)[0].get((t, h)) if self.free_in(j) else None
                    if b is not None:
                        opts.append((a + b, i, j, h))
        return self.first_part(opts, lambda o: self.part(o[1], o[2], self.to(o[1], (t, o[3]))[:-1] + self.on((t, o[3]), o[2])), 3)

    def new_junction(self, t):
        if len(self.js) >= MAX_JUNCTIONS:
            return False
        J = self.junction(t)
        if not J:
            return False
        n = len(self.js)
        self.js.append(J)
        made = []
        for into in (True, False):
            opts = []
            for k in self.live():
                if k == n or not (self.free_out(k) if into else self.free_in(k)):
                    continue
                d = self.direct(k, n) if into else self.direct(n, k)
                if d:
                    opts.append((d[0], k, d[1]))
            for o in sorted(opts, key=lambda o: o[0])[:DETOURS]:
                q = self.part(o[1], n, o[2]) if into else self.part(n, o[1], o[2])
                if q:
                    made.append(q)
                    break
        if len(made) == 2:
            for q in made:
                self.add(q)
            return True
        self.js.pop()
        self.fwd.pop(n, None), self.bwd.pop(n, None)
        return False

    def trim(self):
        """the game's room: the parts covering least on their own go, of those the network keeps its
        rules without (every junction reached from every other)"""
        while sum(len(q[2]) for q in self.parts) > MAX_POINTS:
            best = None
            for q in self.parts:
                if self.outs(q[0]) < 2 or self.ins(q[1]) < 2:
                    continue
                rest = [p for p in self.parts if p is not q]
                if len(sccs(self.live(), [p[:2] for p in rest])) > 1:
                    continue
                loss = sum(1 for c in cover_cells(self.m, q[2]) if self.count[c] == 1)
                if best is None or loss < best[0]:
                    best = (loss, q)
            if not best:
                return False
            self.parts.remove(best[1])
            self.count.subtract(cover_cells(self.m, best[1][2]))
            self.count = +self.count
        return True

    def build(self):
        self.place()
        if len(self.js) < 2:
            return None
        self.link()
        if not self.connect():
            return None
        self.pipes()
        self.cover()
        if not self.trim():
            return None
        used = sorted({q[0] for q in self.parts} | {q[1] for q in self.parts})
        num = {0: 0}
        for j in used:
            num.setdefault(j, len(num))
        net = sorted([(num[i], num[j], pts, jumps) for i, j, pts, jumps in self.parts], key=lambda q: q[0] != 0)
        return net if 0 in used and not check_routes([q[:3] for q in net]) else None


def make_network(m, rng):
    """a network on map m (Planner): [(from, to, points, jumps)], junction 0's part first; None if
    none was made"""
    return Planner(m, rng).build()


def same(p, q):
    """path p mostly on path q: SAME_SHARE of its points within SAME_NEAR of q's"""
    near = sum(1 for a in p if any(math.dist(a, b) < SAME_NEAR for b in q))
    return near >= SAME_SHARE * len(p)


def bands_floors(bands):
    """floor heights: 100-unit bands holding FLOOR_SHARE of the weight (next bands merged)"""
    total = sum(bands.values()) or 1
    out = []
    for b in sorted(bands):
        if bands[b] >= total * FLOOR_SHARE:
            if out and b - out[-1][-1] <= 2:
                out[-1].append(b)
            else:
                out.append([b])
    return [100 * sum(bs) / len(bs) for bs in out] or [0.0]


def point_floors(segs):
    bands = defaultdict(int)
    for seg in segs:
        for p in seg:
            bands[round(p[1] / 100)] += 1
    return bands_floors(bands)


def floors(m):
    """the arena's floor heights: 100-unit bands holding FLOOR_SHARE of the hits (next bands merged)"""
    bands = defaultdict(int)
    for v in m.cells.values():
        bands[round(v[1] / 100)] += v[3]
    return bands_floors(bands)


def svg(m, parts, path, key):
    """the map seen from above, a panel per floor (each cell, line on the floor nearest its height);
    parts: (points, jumps) each"""
    fl = floors(m)
    on = lambda y: min(range(len(fl)), key=lambda i: abs(fl[i] - y))
    xs = [v[0] for v in m.cells.values()]
    zs = [v[2] for v in m.cells.values()]
    x0, x1, z0, z1 = min(xs) - CELL, max(xs) + CELL, min(zs) - CELL, max(zs) + CELL
    w = 1000.0
    s = w / (x1 - x0)
    h = (z1 - z0) * s
    top = lambda f: (len(fl) - 1 - f) * (h + 30)  # the highest floor first
    X = lambda x: (x - x0) * s
    Z = lambda z, f: (z - z0) * s + top(f) + 26
    maxhit = max(v[3] for v in m.cells.values())
    H = len(fl) * (h + 30) + 60
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {H:.0f}" style="background:#111">',
         f'<text x="8" y="{H - 34:.0f}" fill="#ddd" font-family="sans-serif" font-size="16">{key}: {len(m.cells)} cells driven, '
         f'{len(m.cells) - len(m.main)} cut off from the rest; edge cells red; {len(parts)} route parts; {len(m.warps)} warps (dashed, dot the exit)</text>',
         f'<text x="8" y="{H - 12:.0f}" fill="#999" font-family="sans-serif" font-size="13">brighter: driven more. Grey lines: your driving. '
         f'Gaps inside the arena: drive there.</text>']
    for f, y in enumerate(fl):
        o.append(f'<text x="8" y="{top(f) + 18:.0f}" fill="#ddd" font-family="sans-serif" font-size="15">'
                 f'{"floor %d of %d, " % (len(fl) - f, len(fl)) if len(fl) > 1 else ""}height {y:.0f}</text>')
    for c, v in sorted(m.cells.items(), key=lambda kv: kv[1][1]):
        g = int(60 + 160 * math.log1p(v[3]) / math.log1p(maxhit))
        col = f"rgb({g},{g // 3},{g // 3})" if m.depth[c] == 0 else f"rgb({g // 3},{g},{g // 2})"
        if not v[3]:
            col = "rgb(40,90,60)" if m.depth[c] else "rgb(90,40,40)"
        if c not in m.main:
            col = f"rgb({g // 2},{g // 2},{g})"
        o.append(f'<rect x="{X(c[0] * CELL):.1f}" y="{Z(c[1] * CELL, on(v[1])):.1f}" width="{CELL * s:.1f}" height="{CELL * s:.1f}" fill="{col}"/>')

    def lines(pts, style, closed=False):
        """pts as polylines, cut where they change floor"""
        pts = list(pts) + (list(pts[:1]) if closed else [])
        run, f = [], None
        for p in pts + [None]:
            g = on(p[1]) if p else None
            if g != f and len(run) > 1:
                o.append(f'<polyline fill="none" {style} points="' + " ".join(f"{X(q[0]):.1f},{Z(q[2], f):.1f}" for q in run) + '"/>')
            if g != f:
                run = []
            run.append(p)
            f = g

    for seg in m.trace:
        lines(seg, 'stroke="#888" stroke-opacity="0.35" stroke-width="1"')
    for a, _, b, _ in m.warps:  # on the floor they start from
        f = on(a[1])
        o.append(f'<line x1="{X(a[0]):.1f}" y1="{Z(a[2], f):.1f}" x2="{X(b[0]):.1f}" y2="{Z(b[2], f):.1f}" stroke="#fff" '
                 f'stroke-width="2" stroke-dasharray="6 4"/><circle cx="{X(b[0]):.1f}" cy="{Z(b[2], f):.1f}" r="5" fill="#fff"/>')
    for i, (pts, jumps) in enumerate(parts):
        style = f'stroke="hsl({360 * i / max(1, len(parts)):.0f},90%,60%)" stroke-width="2" stroke-opacity="0.8"'
        for a, b in zip([0] + list(jumps), list(jumps) + [len(pts)]):  # (not across a pipe's warp)
            lines(pts[a:b], style)
        o.append(f'<circle cx="{X(pts[0][0]):.1f}" cy="{Z(pts[0][2], on(pts[0][1])):.1f}" r="6" fill="#fff"/>')  # its junction
    o.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(o))


def recordings(routes, key=None):
    """{key: its recording files} in <routes>/rec"""
    rec = defaultdict(list)
    for f in glob.glob(os.path.join(routes, "rec", "*.txt")):
        mt = re.match(r"([0-9a-f]{2}_[0-9a-f]{8})_\d+\.txt$", os.path.basename(f))
        if mt and (not key or mt.group(1) == key):
            rec[mt.group(1)].append(f)
    return rec


def arena_map(files, open_floor, edits=((), ()), tris=None):
    """the arena's Map from its recordings and map edits (read_map; built), and its number of passes;
    None if nothing usable. tris (game_collision's): the map from the game's collision instead, the
    recordings giving only the pipes' warps (and the map edits not used: the game's map is the source)"""
    segs, warps = [], []
    for f in sorted(files):
        sg, wp = read_recording(f)
        segs += sg
        warps += wp
    if tris:
        return game_map(tris, warps, open_floor), len(segs)
    if not segs and not edits[0]:
        return None, 0
    fl = point_floors(segs + [[(0.0, y, 0.0) for _, _, y in edits[0]]])
    m = Map(one_floor=len(fl) == 1, open_floor=open_floor, ground=fl[0])
    for n, seg in enumerate(segs):
        m.add_segment(seg, n)
    m.warps = warps
    m.added, m.removed = list(edits[0]), list(edits[1])
    m.build()
    return m, len(segs)


def read_map(routes, key):
    """the arena's map edits, <routes>/map/<key>.txt: (floor added, floor taken away), (ix, iz,
    height) each, cells of CELL"""
    added, removed = [], []
    path = os.path.join(routes, "map", key + ".txt")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                w = line.split()
                if len(w) == 4 and w[0] in "+-":
                    try:
                        c = (int(w[1]), int(w[2]), float(w[3]))
                    except ValueError:
                        continue
                    (added if w[0] == "+" else removed).append(c)
    return added, removed


def write_map(routes, key, added, removed):
    path = os.path.join(routes, "map", key + ".txt")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(MAP_HEADER + f": cells of {CELL:.0f}; \"+ ix iz y\": floor added (at height y), \"- ix iz y\": taken away\n")
        for sign, cs in (("+", added), ("-", removed)):
            for x, z, y in sorted(cs):
                f.write(f"{sign} {x} {z} {y:.0f}\n")
    return path


def game_collision(disc_path=None, mod_dir=None):
    """{key: [(3 vertices (x, y, z), type)]}: the game's battle arenas' collision (Course/Mini*.arc's
    _course.bco), read from the user's own disc image (never kept). The type's high byte: 0x01 road,
    0x02 wall, 0x0a and 0x12 out of bounds, 0x0d a pipe, 0x04 ice and 0x13 Tilt-a-Kart's rim (ROAD)"""
    import struct
    import build_from_iso, charpack
    arcs = []  # (name, the file's bytes, read when needed)
    if disc_path:
        raw = build_from_iso.Disc(disc_path)

        class Disc(charpack.Disc):
            def _read(self, off, n):
                return raw.read(off, n)

        d = Disc(disc_path)
        arcs += [(name, lambda name=name: d.file(name)) for name in sorted(d.files)
                 if re.fullmatch(r"course/mini\d+\.arc", name)]  # (mini1l.arc...: the same arenas)
    if mod_dir:  # a mod's course sets (MKDD Plus: files/Cours0..4/Mini*.arc)
        for path in sorted(glob.glob(os.path.join(glob.escape(mod_dir), "files", "*", "*.arc"))):
            if re.fullmatch(r"mini\d+\.arc", os.path.basename(path).lower()):
                arcs.append((os.path.relpath(path, mod_dir), lambda path=path: open(path, "rb").read()))
    out = {}
    for name, data in arcs:
        raw_arc = data()
        arc = charpack.rarc(charpack.yaz0(raw_arc) if raw_arc[:4] == b"Yaz0" else raw_arc)
        bco = next((v for k, v in arc.items() if k[1].endswith("_course.bco")), None)
        bol = next((v for k, v in arc.items() if k[1].endswith("_course.bol")), None)
        if not bco or not bol or bco[:4] != b"0003" or len(bol) < 0x7c:
            continue
        h = 2166136261  # the key, as runtime/native.c makes it: FNV-1a of the BOL's 0x7c byte header
        for x in bol[:0x7c]:
            h = ((h ^ x) * 16777619) & 0xFFFFFFFF
        tri, vtx, end = struct.unpack_from(">III", bco, 0x20)
        verts = [struct.unpack_from(">fff", bco, o) for o in range(vtx, end - 11, 12)]
        tris = []
        for o in range(tri, vtx - 0x23, 0x24):
            i = struct.unpack_from(">iii", bco, o)
            if all(0 <= q < len(verts) for q in i):
                tris.append(([verts[q] for q in i], struct.unpack_from(">H", bco, o + 0x16)[0]))
        key = f"{bol[0x19]:02x}_{h:08x}"
        if key not in out and mod_dir:
            print(f"{key}: {name}")
        out.setdefault(key, tris)
    return out


def road_cells(tris):
    """{(ix, iz): [heights]}: the cells whose middle is on the game's road (ROAD), the road's height
    there, each floor"""
    out = defaultdict(list)
    for vs, t in tris:
        if t >> 8 not in ROAD:
            continue
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = vs
        det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(det) < 1e-6:
            continue  # a wall seen from above
        for ix in range(math.floor(min(ax, bx, cx) / CELL), math.floor(max(ax, bx, cx) / CELL) + 1):
            for iz in range(math.floor(min(az, bz, cz) / CELL), math.floor(max(az, bz, cz) / CELL) + 1):
                px, pz = (ix + 0.5) * CELL, (iz + 0.5) * CELL
                u = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / det
                v = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / det
                if u >= 0 and v >= 0 and u + v <= 1:
                    y = u * ay + v * by + (1 - u - v) * cy
                    ys = out[(ix, iz)]
                    if all(abs(y - q) >= LAYER for q in ys):
                        ys.append(y)
    return out


def game_surface(tris):
    """{(fx, fz): [heights]}: where a kart can be on the game's map, FINE squares: the road under each
    square's middle, but not where a wall (any steep triangle) stands on it there"""
    road, walls = defaultdict(list), defaultdict(list)

    def under(vs):
        """(square, height) for each FINE square's middle under triangle vs, seen from above"""
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = vs
        det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(det) < 1e-6:
            return
        for fx in range(math.floor(min(ax, bx, cx) / FINE), math.floor(max(ax, bx, cx) / FINE) + 1):
            for fz in range(math.floor(min(az, bz, cz) / FINE), math.floor(max(az, bz, cz) / FINE) + 1):
                px, pz = (fx + 0.5) * FINE, (fz + 0.5) * FINE
                u = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / det
                v = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / det
                if u >= 0 and v >= 0 and u + v <= 1:
                    yield (fx, fz), u * ay + v * by + (1 - u - v) * cy

    for vs, t in tris:
        a, b, c = vs
        n = [(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
             (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
             (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])]
        l = math.hypot(*n)
        if not l:
            continue
        if abs(n[1]) >= GAME_STEEP * l:
            if t >> 8 in ROAD:
                for k, y in under(vs):
                    if all(abs(y - q) >= FINE / 2 for q in road[k]):
                        road[k].append(y)
            continue
        # a wall: the squares its sides pass over (seen from above it is thin), with its heights
        span = (min(a[1], b[1], c[1]), max(a[1], b[1], c[1]))
        ks = {k for k, _ in under(vs)}
        for p, q in ((a, b), (b, c), (c, a)):
            s = max(1, math.ceil(math.dist(p[::2], q[::2]) / (FINE / 2)))
            for i in range(s + 1):
                ks.add((math.floor((p[0] + (q[0] - p[0]) * i / s) / FINE), math.floor((p[2] + (q[2] - p[2]) * i / s) / FINE)))
        for k in ks:
            walls[k].append(span)
    out = {}
    for k, hs in road.items():
        free = [y for y in hs if not any(lo < y + GAME_HEAD and hi > y + 20 for lo, hi in walls.get(k, ()))]
        if free:
            out[k] = sorted(free)
    return out


def game_map(tris, warps, open_floor):
    """the arena's Map from the game's collision (game_collision's triangles; built): a cell of floor
    where GAME_CELL_MIN of its FINE squares are free surface at a height, walls where the game has
    them or no road; warps (read_recording's) for the pipes; None if no road"""
    surf = game_surface(tris)
    if not surf:
        return None
    fl = bands_floors(Counter(round(h / 100) for hs in surf.values() for h in hs))
    m = Map(one_floor=len(fl) == 1, open_floor=open_floor, ground=fl[0])
    if len(fl) > 1:
        m.flat = m.climb = STEP_UP
    m.want, m.min = GAME_WANT, GAME_MIN
    m.surface = surf
    for k, hs in surf.items():
        for h in hs:
            m.cell((k[0] + 0.5) * FINE, h, (k[1] + 0.5) * FINE, add=0)
    for ls in m.layers.values():
        for l in ls:
            if l[3] < GAME_CELL_MIN:
                l[3] = 0
            l[4] = {0, 1}  # (as driven twice: no cost for few passes)
    m.warps = list(warps)
    m.build()
    return m


def network_score(m, net):
    """(the share of the arena covered, junctions with a choice, fewest points): higher is better"""
    choice = sum(1 for j in {q[0] for q in net} if sum(1 for q in net if q[0] == j) > 1)
    return round(coverage(m, [q[2] for q in net]), 2), choice, -sum(len(q[2]) for q in net)


def best_network(m, rng):
    """the best of NETWORKS networks (network_score); little covered: again nearer the walls (narrow
    lanes), down to MIN_LOW. (network, tries); m.used_min: what the best kept to (m.min as it was)"""
    best, score, tries, start = None, None, 0, m.min
    m.used_min = m.min
    while True:
        for _ in range(NETWORKS):
            tries += 1
            net = make_network(m, rng)
            s = network_score(m, net) if net else None
            if net and (best is None or s > score):
                best, score, m.used_min = net, s, m.min
        if (best and score[0] >= COVER_DONE - 0.07) or m.min <= MIN_LOW:
            break
        m.min -= 100.0
    m.min = start
    return best, tries


def read_networks(path):
    """a route file's networks: [[(from, to, points)]] ("=" lines between them); a file of loops: its
    first (junction 0 back to 0)"""
    nets, cur = [[]], None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line == "=":
                nets.append([])
                cur = None
            elif line.startswith(">"):
                a, b = map(int, line[1:].split()[:2])
                cur = (a, b, [])
                nets[-1].append(cur)
            elif line == "-":
                break  # (loops only)
            elif line and not line.startswith("#"):
                try:
                    p = tuple(map(float, line.split()[:3]))
                except ValueError:
                    continue
                if cur is None:
                    cur = (0, 0, [])
                    nets[-1].append(cur)
                cur[2].append(p)
    return [n for n in nets if n] or [[]]


def check_routes(parts):
    """what the game would refuse in a network (runtime/route.c net_load), or None"""
    out, into = defaultdict(int), defaultdict(int)
    for a, b, pts in parts:
        if len(pts) < 2:
            return f"the part {a} to {b} has fewer than 2 points"
        out[a] += 1
        into[b] += 1
    if out[0] != 1:
        return "junction 0 needs exactly one part leaving it"
    for j in set(out) | set(into):
        if out[j] > 4 or into[j] > 4:
            return f"more than 4 parts leave or reach junction {j}"
        if not out[j] or not into[j]:
            return f"parts reach junction {j} but none leave (or the other way)"
    if sum(len(p) for _, _, p in parts) > MAX_POINTS:
        return f"more than {MAX_POINTS} points"
    return None


def write_routes(routes, key, nets, install, note=""):
    """the networks (lists of parts) to <routes>/net/<key>.txt, or (install) <routes>/<key>.txt, the
    file the game reads (one there not made by these tools kept as <key>_old.txt; one of theirs as
    <key>.bak); "=" lines between networks"""
    out = os.path.join(routes, key + ".txt") if install else os.path.join(routes, "net", key + ".txt")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if install and os.path.exists(out):
        with open(out) as f:
            ours = f.readline().startswith(HEADER)
        old = os.path.join(routes, key + (".bak" if ours else "_old.txt"))
        os.replace(out, old)
        if not ours:
            print(f"{key}: kept the old route as {old}")
    many = f"{len(nets)} networks (the game picks one each battle; \"=\" between them)" if len(nets) > 1 else "a network"
    with open(out, "w") as f:
        f.write(HEADER + f": {many}{note}. \"> a b\": a part from junction a to b, then its points (x y z)\n")
        for n, parts in enumerate(nets):
            if n:
                f.write("=\n")
            for a, b, pts in sorted(parts, key=lambda q: q[0] != 0):  # junction 0's part first
                f.write(f"> {a} {b}\n")
                for p in pts:
                    f.write(f"{p[0]:.1f} {p[1]:.1f} {p[2]:.1f}\n")
    return out


def main():
    global MAX_JUNCTIONS  # (FEWER_JUNCTIONS: for one arena)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("routes")
    ap.add_argument("--key", help="one arena (course ID _ BOL hash); default every arena recorded")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--open", default="", type=lambda v: set(v.split(",")) - {""},
                    help="arenas (keys, commas between) with no holes: all inside the outline is floor")
    ap.add_argument("--install", action="store_true", help="write <routes>/<key>.txt, the file the game reads")
    ap.add_argument("--variants", type=int, default=VARIANTS, help=f"networks in each arena's file, the game picking one each battle (default {VARIANTS})")
    ap.add_argument("--game", metavar="IMAGE", help="your disc image (.iso/.gcm/.ciso/.rvz): the map from the game's collision, not the recordings")
    ap.add_argument("--mod", metavar="DIR", help="a mod's folder (MKDD Plus: the one with files and sys): its arenas' maps from their collision too")
    a = ap.parse_args()
    rec = recordings(a.routes, a.key)
    a.game = a.game or None
    game = {k: v for k, v in game_collision(a.game, a.mod).items() if not a.key or k == a.key} if a.game or a.mod else {}
    a.game = a.game or a.mod
    if not rec and not game:
        sys.exit(f"no recordings in {os.path.join(a.routes, 'rec')}" + (f" for {a.key}" if a.key else ""))
    os.makedirs(os.path.join(a.routes, "net"), exist_ok=True)
    for key in sorted(set(rec) | set(game)):
        files = rec.get(key, [])
        rng = random.Random(f"{a.seed}-{key}" if a.seed is not None else None)  # (an arena's the same alone or with the rest)
        edits = read_map(a.routes, key)
        m, n = arena_map(files, key in a.open, edits, game.get(key) if a.game else None)
        if a.game and key not in game:
            print(f"{key}: not one of the game's arenas: its map from the recordings")
        if not m:
            print(f"{key}: nothing usable in {len(files)} recordings")
            continue
        if (edits[0] or edits[1]) and m.surface is None:
            print(f"{key}: map edits: {len(edits[0])} cells of floor added, {len(edits[1])} taken away")
        nets, most = [], MAX_JUNCTIONS
        for v in range(max(1, a.variants)):
            best, tries = best_network(m, rng)
            for fewer in FEWER_JUNCTIONS if not best and not nets else ():
                MAX_JUNCTIONS = fewer
                print(f"{key}: no network fits {MAX_POINTS} points: again with at most {fewer} junctions")
                best, tries = best_network(m, rng)
                if best:
                    break
            if not best:
                break
            nets.append(best)
            pic = os.path.join(a.routes, "net", f"{key}_{v + 1}.svg")
            svg(m, [(q[2], q[3]) for q in best], pic, key)
            print(f"{key}: network {v + 1}: {len({q[0] for q in best})} junctions, {len(best)} parts "
                  f"({sum(1 for q in best if q[3])} through pipes, {sum(len(q[2]) for q in best)} points), "
                  f"covers {100 * coverage(m, [q[2] for q in best]):.0f}% of the arena; {tries} tried; {m.used_min:.0f}+ from the walls; {pic}")
        MAX_JUNCTIONS = most
        print(f"{key}: {len(files)} recordings, {n} passes, {len(m.warps)} warps, {len(m.cells)} cells ({len(m.cells) - len(m.main)} cut off)"
              + ("; the map from the game's" if m.surface is not None else ""))
        if not nets:
            print(f"{key}: no network: " + ("too little floor away from the walls" if m.surface is not None else "drive more of the arena"))
            continue
        out = write_routes(a.routes, key, [[q[:3] for q in net] for net in nets], a.install,
                           " made from the game's map" if m.surface is not None else f" made from {len(files)} recordings")
        print(f"{key}: wrote {out}")


if __name__ == "__main__":
    main()
