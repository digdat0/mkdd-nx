# Battle routes: the route editor

The battle arenas have no paths for computer karts, so **SINGLE PLAYER BATTLE MODE** (the − menu, MODS page,
on by default) needs a route file for each arena. These files are the port's own data and contain no game data.

**Built in:** the `.nro` carries a route for every arena, the game's six and MKDD Plus's 24, made from
the repository's `routes/<key>.txt` when it is built (`switch/CMakeLists.txt`; `-DROUTES_DIR=` for another
folder). Nothing is needed on the SD card. **Your own:** a `switch/MKDD-NX/routes/<key>.txt` on the SD card
wins over the built-in one for that arena; delete it to go back. The tools below work on any `routes`
folder: the repository's (then rebuild) or the SD card's.

There are two tools:

| Tool | What it does |
|---|---|
| `tools/battle_routes.py` | Makes routes automatically from your recorded laps |
| `tools/route_editor.py` | A window for editing those routes by hand |

Both need Python 3 (Windows: `py -3`). The editor uses tkinter, which comes with Python.

## How CPUs follow a route

An arena's route is a **network**: numbered **junctions** placed round the arena, joined by
**parts** (paths of points from one junction to another).

When a CPU reaches a junction, it picks one of the parts leaving that junction at random. It picks
again at the next junction. So the more junctions that offer a choice, the more the CPUs spread
out instead of driving nose to tail. This uses the game's own branching, the same as CPUs choosing
paths in races.

An arena's route file can hold **several networks**. At the start of each battle the game deals
them out at random: **each CPU gets its own network**, and no two CPUs get the same one. CPU 1
gets one, CPU 2 one of the rest, CPU 3 one of what's left. The networks never join, so each CPU
stays on its own, after a respawn too. A file needs at least as many networks as there are CPUs
(3). With fewer, CPUs share. The tools make 3 by default; more (up to 32) make each battle
different.

## 1. Record the arena

Recordings give the tools the arena's map: where the floor is, where the walls are, and the pipes.
If you use the game's own map instead (see [Map from](#map-from-recordings-or-the-game)), the
recordings only matter on Pipe Plaza, for where each pipe comes out.

The recordings in `routes/rec/` were made with an earlier test build (RECORD BATTLE ROUTE, no
longer in the menu). The tools still read them: keep that folder. With the game's map, every arena
but Pipe Plaza needs no recordings at all.

## 2. Make routes automatically

The route maker spreads routes over the whole arena:

1. **Junctions:** it places junctions spread out across the arena. Each faces along the lane it
   sits in, so CPUs drive straight through it.
2. **First parts:** it joins each junction to its nearest neighbours ahead. Paths turn at most 45
   degrees per 400 units, so CPUs can follow them.
3. **Fill the gaps:** it keeps adding parts through whatever no route comes within 1000 units of,
   or adds a junction there. It stops at 97% of the arena or when the game's limits are reached.
4. **Pick the best:** it makes several networks and keeps the one covering the most, then the one
   giving CPUs the most choices.

Each network is made from a random start, so every run gives different ones.


```bash
py -3 tools/battle_routes.py "<SD>/switch/MKDD-NX/routes" --seed 1 --open 35_3439d285,38_db1d03f9,3b_2ad1d46b --install
```

- `--open` lists the arenas with no holes or inner walls (Nintendo GameCube, Tilt-a-Kart, Pipe
  Plaza). There, everything inside the outline counts as floor, so routes can go close to the edges.
- `--install` writes the files the game reads. Without it, the routes go to `routes/net/` to look
  at first, with a picture (`.svg`) of each.
- `--key <key>` does only one arena. `--seed N` makes the same routes each time; a different number
  gives different routes. Without it, every run is different.
- `--variants N` puts N networks in each arena's file (default 3). Each battle, every CPU gets a
  different one.
- `--game "E:/path/to/game.iso"` makes the map from the game's collision instead of your recordings
  (see [Map from](#map-from-recordings-or-the-game)). It works on every arena, recorded or not.

## 3. Edit routes

**The app:** `MKDD-NX-Route-Editor.exe` (Windows) or `MKDD-NX-Route-Editor-arm64.dmg` / `-x86_64.dmg`
(Mac) from the releases, nothing to install; step by step on the
[Route Editor wiki page](https://github.com/digdat0/mkdd-nx/wiki/Route-Editor). Started without
arguments it asks for the routes folder, your disc image and MKDD Plus's folder (Cancel skips either), and
remembers them for next time (`%APPDATA%/MKDD-NX/route_editor.json`; on a Mac
`~/Library/Application Support/MKDD-NX/`). It carries the built-in routes:
an arena with no file in the folder opens with its built-in route, and Save writes it to the folder.
It takes the same arguments as the script. `py -3 tools/build_route_editor_app.py` builds it.

**The script:**

```bash
py -3 tools/route_editor.py routes --disc "E:/path/to/game.iso" --mod "E:/path/to/MKDD Plus"
```

That edits the built-in routes (rebuild after saving). To try a route without rebuilding, point it at
`switch/MKDD-NX/routes` on the SD card in a card reader instead (copy the arena's `<key>.txt` there first).

The editor needs Pillow (`py -3 -m pip install pillow`). To draw the game's own map, it reads your
disc image. By default it uses the `.iso`, `.gcm`, `.ciso` or `.rvz` in the app folder (the folder
above `routes`). Use `--disc "E:/path/to/game.iso"` for one somewhere else. A mod's arenas (MKDD
Plus) are not on the disc: add `--mod "E:/path/to/MKDD Plus"`, the mod's folder that holds `files/`.
It only reads the image and the mod, and saves nothing from them.

### The window

- **Arena list** (top left): pick the arena. The map is drawn from its recordings: the floor is
  shaded, and the outline marks the walls.
- **Map from** (top): **Recordings** or **Game**. See [below](#map-from-recordings-or-the-game).
- **Floor** (top): on arenas with more than one level (Luigi's Mansion, Pipe Plaza), pick a level
  to see only that level: its floor, game map and route points. Points on other levels are hidden
  and can't be picked. New and moved points take that level's height. **all** shows every level
  stacked, and a moved point keeps its height.
- **Game map** (top): draws the arena's real layout under your map, from the game's collision
  data. Green is road, red is wall, purple is out of bounds and teal is a pipe. Where green shows
  through black, the game has road that nobody has driven yet. The status bar names what the game
  has under the mouse, with its height.
- **Network** (left, above the parts): which of the file's networks you see and edit. **Add
  network** makes another one from the map. **Delete network** removes the one shown.
- **Covers N%** (left): how much of the arena the network shown comes within 1000 units of.
- **Coverage** (top): shades red the floor no route of the network shown comes near.
- **Parts list** (left): every part, written `from > to`. Click one to select it.
- **Status bar** (bottom): the position under the mouse, how far it is from a wall, and how many
  points the selected part has.

On the map, the selected part is yellow and junctions are numbered circles. Dashed lines are pipes.

Point colours show how close each point is to a wall:

| Colour | Meaning |
|---|---|
| Normal | Clear of the walls |
| Orange | Closer than the tools like |
| Red | Very close: a CPU there may hit the wall or get stuck |

### Mouse and keys

| Action | Does |
|---|---|
| Drag a point | Moves it |
| Drag a junction (numbered circle) | Moves it, and every part leaving it with it |
| Click the selected part's line | Adds a point there |
| Click another part's line | Selects that part |
| Shift-click | Adds a point at the end of the selected part |
| Right-click a point (Mac: also Control-click) | Deletes it (a part keeps at least 2) |
| Middle- or right-drag | Pans the map |
| Mouse wheel | Zooms |
| Ctrl+Z | Undo (50 steps) |

### Buttons

- **New part** asks for the junction it starts from and the junction it goes to, then draws a
  straight line between them. Drag its points into shape.
- **Copy part** puts a duplicate on top of the selected part. This is the quickest way to add
  another choice between the same two junctions: copy, then drag the copy's middle points to a
  different line. Its ends already sit at the right junctions.
- **Delete part** removes the selected part.
- **Reload** discards your changes and reads the file again.
- **Make routes** throws away the network shown and makes a new one from the map as it is now. It's
  the same as `battle_routes.py`, for this network only, and takes a few seconds. Each press gives a
  different network. Ctrl+Z brings the old parts back.
- **Add network** / **Delete network**: see the Network list above. Ctrl+Z undoes both.
- **Save** writes the route file the game reads (every network), and your map edits if there are
  any. The previous route file is kept as `<key>.bak`. You don't need to restart the game; the next battle in that
  arena uses it. The editor warns you before closing or loading another arena with unsaved changes.

### Map from: recordings or the game

The map the routes are made on, and the wall checks (the point colours, Save's warning), come from
one of two places:

| Map from | The floor and walls | Edit map |
|---|---|---|
| **Recordings** | Where you drove, with your map edits | Yes |
| **Game** | The game's own collision: every road, wall and edge exactly where the game has them | No: the game's map is the source |

With **Game**, your floor and outline are hidden and only the game's map is drawn. Then:

- **Floor:** a cell of floor wherever the game has road with no wall on it.
- **Walls:** the walls are the game's walls, the edges of drops and the steps between levels.
  Distances are measured along the road, so a ramp between levels is not a wall.
- **Closer to walls:** routes may go nearer the walls than with recordings (600 units wanted,
  300 at least, against 900 and 500), because the walls are known exactly.
- **Pipes:** the game's collision says where a pipe is, not where it comes out. On Pipe Plaza, the
  pipe parts still come from your recordings of driving through them.
- **Map edits:** these are kept in their file but not used while Map from is Game.

Start the editor with `--game-map` to open in this mode. `battle_routes.py --game IMAGE` does the
same from the command line.

### Editing the map

The tools only know the floor you drove. Anywhere you never drove counts as a wall, so routes keep
away from it and parts of the arena get no routes at all. You can fix the map instead of recording
more.

Tick **Edit map**. The routes are hidden and the mouse paints:

| Action | Does |
|---|---|
| Drag | Paints floor (blue cells) |
| Right-drag | Takes floor away, including floor you drove. The tools will never fill it in again |
| Brush | The brush size, 1 to 5 cells across |
| Fill from game | Adds floor wherever the game has road joined to the floor you have |
| Middle-drag, wheel | Pan and zoom |
| Ctrl+Z | Undoes a stroke or a fill |

- **Fill from game** first: it does most of the work and asks before adding anything. On
  arenas with levels, pick a level first and it fills only that level.
- **Painting on levels:** on arenas with more than one, pick the level before painting. Painted
  floor takes the height of the game's road under it on that level, or the level's height where
  the game has none.
- **After editing:** Save, then **Make routes** for routes that use the new floor, or run
  `battle_routes.py`, which reads your edits too.
- **The risk is yours:** painted floor is trusted as driveable. If you paint over a wall or a drop,
  routes will lead CPUs into it.

### Adding choices for the CPUs

To give CPUs another way between two junctions, select an existing part between them, **Copy
part**, and drag the copy somewhere else. Where 3 parts leave a junction, a 4th makes CPUs pick
from 4.

Keep the new path on the floor and away from walls (no red points), and avoid sharp turns. CPUs
follow points loosely, so a path drawn tight round a corner can clip the wall.

## Rules

These apply to each network. Save refuses a route that breaks one of them, and tells you which
network and which rule. If a network in the file breaks a rule anyway, the game deals out the
others instead. If none can be used, the game uses no route for that arena: it still runs,
but the CPUs stand still.

| Rule | Why |
|---|---|
| Exactly **one** part leaves junction 0 | The game's own rule (parts may still *reach* 0) |
| At most **4** parts leave a junction, and at most 4 reach it | Each route point links to at most 4 others |
| Every junction that parts reach has a part leaving it, and the other way round | Otherwise CPUs reach a dead end |
| At least 2 points per part | |
| At most **512** points in the whole arena | The space set aside for routes |
| Junctions numbered 0 to 63 | |

## Files

In a `routes` folder (the repository's, or `switch/MKDD-NX/routes/` on the SD card):

| File | What it is |
|---|---|
| `<key>.txt` | The route the game reads: one or more networks |
| `<key>.bak` | The previous route, kept by Save or `--install` |
| `<key>_old.txt` | A route not made by these tools, kept when they replaced it |
| `rec/<key>_<n>.txt` | Recordings |
| `map/<key>.txt` | Your map edits: `+ x z height` adds a cell of floor, `- x z height` takes one away (cells of 400 units) |
| `net/` | Routes made without `--install`, and a picture of each network (`<key>_<n>.svg`) |

`<key>` is the arena's course ID and a hash of its course file, for example `38_db1d03f9` for
Tilt-a-Kart. The hash keeps a mod's arena apart from the original with the same ID.

| Key | Arena |
|---|---|
| `34_bfb261d8` | Luigi's Mansion |
| `35_3439d285` | Nintendo GameCube |
| `36_9c9b9679` | Block City |
| `38_db1d03f9` | Tilt-a-Kart |
| `3a_99e0cbaf` | Cookie Land |
| `3b_2ad1d46b` | Pipe Plaza |

A route file is plain text. Each part is a line `> a b` (from junction `a` to junction `b`)
followed by its points, one `x y z` per line. A line `=` separates networks. Lines starting with
`#` are comments. A part's last
point sits just before junction `b`; the game joins it to every part leaving `b`.

## Checking in game

The game's log shows one of these when a battle loads:

| Log line | Meaning |
|---|---|
| `route: network, N parts, M points` | The route is in use |
| `route: routes/<key>.txt, network A, B, C of N` | Which of the file's networks this battle uses, in order (the SD card's file) |
| `route: built-in <key>, network A, B, C of N` | The same, from the route built into the `.nro` |
| `battle cpus: kart K drives network I of the M loaded` | Which of those each CPU drives |
| `battle cpus: kart K onto network I at point P` | A CPU was put back on its own network (start or respawn) |
| `battle ai: network I: N parts` | The network's junction map was built, so CPUs on it choose their way |
| `battle ai:   kart K chases T (x y z), focus F` | Every 2 s: a CPU's goal (T: the kart it is after, -1 an item box or the Shine) and how often it takes the best way (0.6–0.9) |
| `battle ai: frame F, kart K at (…): W ways, game G, took P to (…)` | A CPU at a junction: the game's random pick G and the one it took |
| `battle ai: frame F, kart K red forward at kart T, D away (held H)` | A CPU used an item on purpose: forward, back, catch or escape |
| `route: network not used: <reason>` | A network broke a rule (see above); the next one is tried |
