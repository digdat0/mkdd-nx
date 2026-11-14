# Trick Style track builder

Builds the sixteen Trick Style tracks (four cups, `cups8.txt`), their picker art and the mode page's TRICKS row.
These are developer tools: they are not part of the Tools app. Players get the tracks in `assets/trick.pak`,
which the game unpacks on its first start.

Nothing the builder makes is committed. It works from **your own disc image** (GM4E01). `gamefiles.py` copies the
few game files it builds on into `work/`, which git ignores. The track geometry, textures, collision, course
data, routes, coins and effects are all generated here.

## Requirements

- Python 3.10 with numpy and Pillow (`requirements.txt`)
- SuperBMD.exe (github.com/RenolY2/SuperBMD) for the track models. It is not in the repo: pass `--superbmd <path>` or set
  `SUPERBMD`.

## Build

```
py -3.10 tools/tricktracks/buildall.py <outdir> --disc <GM4E01 image> --superbmd <SuperBMD.exe>
```

This builds every track (`Course/Tnn.arc`, `routes/`, `coins/`, `fx/`), then the art (`Art/`, `CourseName/`). Then
`trackval.py` checks the result. It exits 1 on a hard problem, such as:
- a start or respawn point on a boost or a dead zone;
- dead zones that send karts to a respawn point that doesn't exist;
- item boxes with no ground under them.

Route and coin notes are informational: jumps over gaps, and coin arcs over pits.
`--disc` is needed only once, because the work folder keeps the files. Name tracks to build only those
(`buildall.py out stadium neon`); `--no-art` skips the art.

One track: `py -3.10 tools/tricktracks/build7.py <outdir> <track> <Tnn>` (the stem is required for the cup tracks).
Builds are seeded, so the same layout gives the same bytes.

Install for a test: copy `<outdir>`'s files into the app's `mods/trick/tracks/` (Course, CourseName, Art), `mods/routes/`,
`mods/coins/` and `mods/trick/fx/`. Then rebuild the pack that ships in the app:

```
py -3.10 tools/trickpak.py <GM4E01 image> <app folder> -o assets/trick.pak
```

A route key is a hash of the course data's header, so changing a track's respawn or object counts gives it a new key.
Remove the old key's routes, coins and fx files, and copy the new routes into the repo's `routes/`.

## Files

| file | role |
|---|---|
| `tracks7.py`, `tracks8.py`, `tracks9.py` | the layouts (tracks9 holds the twelve unique tracks; `tracks7.TRACKS` gathers them) |
| `props9.py`, `tex6.py`, `tex7.py` | scenery props; procedural textures |
| `build7.py` | one track: geometry, collision (`bcolib.py`), model (SuperBMD), course data, route, coins, fx, starts, respawns |
| `art8.py`, `render7.py`, `cupicons.py`, `btix.py` | picker pictures, course banners, cup icons; software render of a track's view |
| `trackval.py` | the invariant checks |
| `gamefiles.py` | the game files from your disc → `work/` |
| `menu/` | the mode page's TRICKS row (run from `menu/`: `icons.py ../work/balloon00.bti art`, `cuppict.py ../work/cup_pict_balloon.bti art/cup_pict_trick.bti`, then `menu4.py ../work/menu.arc <out>/SceneData/English/menu.arc`). The TRICK STYLE titles go in blank: the app letters them with the disc's MarioFont when it unpacks the pack (`runtime/trickpak.c`) |
| `lib/` | `libbol`, `rarc`, `yaz0`, `vectors` from Yoshi2's mkdd-track-editor (MIT, `lib/LICENSE`) |
