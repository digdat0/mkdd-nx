# MKDD-NX: Mario Kart: Double Dash!! on Nintendo Switch (static recompilation)

<img width="400" height="400" alt="Double Dash NX" src="Double%20Dash%20NX_%20Logo3.png" />

A native Switch homebrew build of Mario Kart: Double Dash!! (USA, GM4E01). The game's PowerPC
code is translated to C ahead of time and compiled for the Switch's ARM CPU; the GameCube
hardware it talks to (OS threads, GX graphics, DSP audio, DVD, memory card, pads) is
reimplemented in `runtime/`. No emulation of the CPU at run time.

Based on [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp) (Rust/Bevy, PC): its
spreadsheet pipeline, runtime design and renderer were ported to C / GLES here. Function names
come from the [doldecomp/mkdd](https://github.com/doldecomp/mkdd) decompilation (CC0).

**No game code or data is in this repository.** The generated C and the `.nro` built from it
contain code recompiled from your own disc: they are for your personal use only and must not be
shared. Bring your own legally dumped disc image of Mario Kart: Double Dash!! (USA, GM4E01).

## Build

`tools/build_from_iso.py` does the whole build from your disc image in one command. Everything
it makes from the disc goes to a work folder outside the repository (default `../mkdd-nx-work`).

### Requirements (one-time setup)

| What | Notes |
|---|---|
| [devkitPro](https://devkitpro.org/wiki/Getting_Started) | packages `switch-dev switch-sdl2 switch-mesa switch-tools` |
| Python 3.8+ | on Windows run scripts with `py -3` (Git Bash's `python` may be devkitPro's msys2 Python) |
| Git | on Windows, Git for Windows (its `bash` runs the Switch build) |
| Java 21 (JDK) | for Ghidra, e.g. Temurin 21 |
| [Ghidra 12.1.4](https://github.com/NationalSecurityAgency/ghidra/releases) | unzip anywhere |
| [GameCubeLoader](https://github.com/Cuyler36/Ghidra-GameCube-Loader) for Ghidra 12.1 | in Ghidra: File > Install Extensions, or unzip into `<Ghidra>/Ghidra/Extensions` |

The script also fetches [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp) at a pinned
commit: its function list, metadata and Ghidra export scripts.

### One command

```
py -3 tools/build_from_iso.py "D:/Games/Mario Kart Double Dash (USA).iso" --ghidra C:/tools/ghidra_12.1.4_PUBLIC
```

`.iso`, `.gcm` and `.ciso` all work. It:

1. checks the disc is GM4E01 and extracts `main.dol` (its SHA-1 must match the supported build),
2. clones MKDD-Recomp (pinned),
3. runs Ghidra headless on `main.dol` (about 10-20 minutes): import, auto-analysis,
   `CreateGapFunctions.java`, `ExportPcode.java` (expected: 740,738 instructions),
4. assembles the sheets and generates the C with `tools/gen_c.py` (a few minutes),
5. builds the `.nro` with `switch/build.sh` (30-60 minutes the first time).

Output: `build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro`. Re-running skips the steps already
done (the Ghidra export is reused); `--force` redoes everything. Other options: `--work DIR`,
`--recomp DIR` (use an existing MKDD-Recomp checkout), `--bash PATH`, `--no-build` (stop after
generating the C). `GHIDRA_INSTALL_DIR` can replace `--ghidra`.

### By hand

1. Extract `main.dol` from the disc and export it with Ghidra:
   ```
   analyzeHeadless <project dir> MKDD -import main.dol -loader GameCubeLoader        -loader-autoloadMaps false -scriptPath <MKDD-Recomp>/tools        -postScript CreateGapFunctions.java -postScript ExportPcode.java <export dir>
   ```
   Expected: 13,490 functions, 740,738 instructions, 55,822 memory rows.
2. Put MKDD-Recomp's `sheets/functions.csv`, `meta.csv`, `hle.csv` and the exported
   `instructions.csv`, `memory.csv`, `registers.csv` into one folder (outside this repository).
3. `py -3 tools/gen_c.py <sheets folder> <gen folder>`
4. `switch/build.sh <gen folder>` (Git Bash on Windows)

Headless PC test build (MSYS2 UCRT64 gcc): `pc/build_pc.sh <gen folder>`, then
`pc/build/mkdd_pc.exe <disc image> [seconds] [pad script]`.

## Install

Copy `build-switch/sdcard/switch/MKDD-NX/` to `sdmc:/switch/` and put your disc image (`.iso`, `.gcm` or `.ciso`)
in the same folder. Start it with title takeover (hold R while starting a game). Saves are
standard `.gci` files in `save/card_a/` (usable in Dolphin's GCI-folder mode and save editors;
each file's previous version is kept in `save/backup/`), logs go to `mkdd.log` /
`mkdd_trace.log` / `crash.log` in the same folder. An older `save/slotA.bin` is converted on
first start and kept as `slotA.bin.migrated`.

Controls: A/B/X/Y as on the GameCube, + = Start, ZR = R (drift), ZL = L, L/R = Z,
left stick (or the D-pad) = control stick, right stick = C-stick.

Press - for the options menu: display 4:3 or 16:9, FPS counter, button mapping (pick a
GameCube button, press the Switch button for it) and exit. Settings are saved to
`settings.ini` in the app folder. 16:9 widens the game's camera (more of the track at the
sides) and moves the race HUD to the screen edges; menus stay 4:3 in the middle.

## Credits and licence

- [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp): the spreadsheet pipeline,
  runtime design and renderer this port is based on; the build fetches its sheets and scripts.
- [doldecomp/mkdd](https://github.com/doldecomp/mkdd) (CC0): function names and structures.
- [GameCubeLoader](https://github.com/Cuyler36/Ghidra-GameCube-Loader), Ghidra, devkitPro.

The code in this repository is released under the MIT licence (`LICENSE`). Mario Kart:
Double Dash!! and its characters are Nintendo's; this project is not affiliated with Nintendo.
