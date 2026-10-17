# MKDD-NX: Mario Kart: Double Dash!! 
Static recompilation of the GameCube game Mario Kart: Double Dash!! for Nintendo Switch (USA, GM4E01). 

**No game code or data is in this repository.** 
**Bring your own legally dumped disc image of Mario Kart: Double Dash!! (USA, GM4E01). We do not condone piracy and will not respond to requests for game image files!**

## Getting started

**You need:**

- A Switch running custom firmware (Atmosphere) with the homebrew menu.
- Your own dump of **Mario Kart: Double Dash!! (USA)**, game ID **GM4E01**, as `.iso`, `.gcm` or
  `.ciso`. Other regions (PAL GM4P01, Japan GM4J01) and the bundle/demo discs don't work: the game
  checks the disc's `main.dol` and refuses any other version.
- `MKDD-NX.nro` from the releases section.

**Setup:**

1. On the SD card, create the folder `sdmc:/switch/MKDD-NX/`.
2. Copy `MKDD-NX.nro` into it.
3. Copy your disc image into the same folder. Keep only one disc image there: the game uses the
   first `.iso` / `.gcm` / `.ciso` it finds.
4. Optional: add `hd-textures.bin` there too (see [HD textures](#hd-textures)).
5. Start it from the homebrew menu with **title takeover**: hold **R** while starting any game
   from the HOME menu, then pick MKDD-NX. (The album / applet mode has too little memory.)

```
sdmc:/switch/MKDD-NX/
    MKDD-NX.nro
    Mario Kart - Double Dash!! (USA).iso
    hd-textures.bin          (optional)
```

The first start takes longer while shaders are compiled; later starts reuse them. Press **-** at
any time for the options menu.

### App folder

The app creates three folders next to the `.nro` and the disc image:

| Folder | |
|---|---|
| `save/` | standard `.gci` saves in `save/card_a/` (usable in Dolphin's GCI-folder mode and save editors); each file's previous version is kept in `save/backup/` |
| `config/` | `settings.ini`, `color.ini` and `shaders.bin` (compiled shaders: delete it to rebuild) |
| `logs/` | `mkdd.log`, `mkdd_trace.log`, `crash.log`, `stderr.log`, `boot.txt` (the previous run's as `*.prev.log`) |

### Controls

Up to 4 players. Controllers become players in order: the console in handheld mode first,
then wireless controllers as the system numbers them (so handheld + one Pro Controller is a
2-player game). **CONTROLLERS** in the - menu opens the system screen for pairing, the player
order and single Joy-Cons. Connects and disconnects are logged (`pad:` lines in `mkdd.log`).

- **Handheld, Pro Controller, Joy-Con pair:** A/B/X/Y as on the GameCube, + = Start,
  ZR = R (drift), ZL = L, L/R = Z, left stick (or the D-pad) = control stick, right stick =
  C-stick. Remappable in the menu (applies to all of these).
- **Single Joy-Con, held sideways:** the four face buttons sit where a GameCube pad's A (right),
  B (bottom), X (top) and Y (left) are; SL = L, SR = R, the top shoulder or stick click = Z,
  + (right Joy-Con) or ZL / - (left Joy-Con) = Start. Player 1 on a single right Joy-Con opens
  the menu with stick click + +.
- **GameCube controller (official adapter):** its own layout, with analog L/R.

Rumble works on controllers with HD Rumble and on GameCube controllers.

### Options menu (-)

The game pauses while the menu is open (and while the Switch is in the HOME menu or asleep). It
also opens by itself when a controller in use disconnects; reconnect it and close the menu.

| Option | |
|---|---|
| Display | 16:9 (default): wider game camera, race HUD at the screen edges, menus 4:3 in the middle; or 4:3 |
| HD textures | Off (default) / On when `hd-textures.bin` is in the app folder (see [HD textures](#hd-textures)), else NO PACK |
| Controllers | the system controller screen |
| Button mapping | pick a GameCube button, press the Switch button for it |
| Advanced | the page below |
| Debugging | the page below |
| Exit game | |

**Advanced**

| Option | |
|---|---|
| Color | Accurate / Vivid / Custom brightness, contrast, saturation (hold Y on that page to preview); saved to `config/color.ini` |
| Rumble | Off (default) / Low / High |
| Field of view | Normal / Wide / Wider race camera, kept separately for 4:3 (default Normal) and 16:9 (default Wider); the row changes the current display mode's |
| Texture filtering | Normal / 2x / 4x / 8x / 16x anisotropic filtering: sharper roads and walls at a distance (most visible with HD textures, which have mipmaps) |
| Docked resolution | 2X (default) / Native: on the TV the game renders at twice its size (1280x960 instead of 640x480) and outputs 1080p; handheld always renders at the game's own size |
| Border | Off / On: checkered border art in the side bars during races in 4:3 (menus keep black bars) |

**Debugging**

| Option | |
|---|---|
| Show FPS | Off / FPS / FPS + times: ms per game frame of the game thread (GAME), the graphics thread (GX) and the render thread (RENDER); 16.7 is the 60 fps budget |
| Unlock all | every character, kart, cup and mode while it is on; never written to your save, so your own progress stays as it is |
| Logging | debug logs on the SD card, for bug reports |
| Profiler | Off / On: performance samples in `logs/mkdd_prof.bin` for reports (`tools/prof_report.py`); costs a little speed, takes effect at the next start |

Settings are saved to `config/settings.ini`.

## HD textures

MKDD-NX can use the community **MKDD HD texture pack** made for Dolphin
([mkddhd](https://sites.google.com/view/mkddhd/home-page); use the **English** version, downloaded from the
Mega link on that site). The pack is not included: download it yourself and convert it once on a PC.
The Switch can't use the pack's PNGs directly (about 1 GB, and decoding them would stall the game), so
`tools/hdtex_pack.py` turns them into one file of GPU-ready BC7 textures, at the pack's full
resolution with mipmaps.

### The easy way: MKDD-NX HD Textures app

Download the app for your computer from the [latest release](https://github.com/digdat0/mkdd-nx/releases/latest):
`MKDD-NX-HD-Textures.exe` (Windows) or `MKDD-NX-HD-Textures-arm64.dmg` / `-x86_64.dmg` (Mac with Apple
Silicon / Intel). Nothing else needs installing.

1. Download and extract the English pack (link above, also in the app).
2. Open the app. Windows may say "Windows protected your PC": click **More info**, then **Run anyway**.
   On a Mac, open the `.dmg`, drag the app to Applications, then right-click it and choose **Open** the
   first time (it isn't signed by Apple).
3. **Texture pack:** choose the pack's `GM4E01` folder (or the folder you extracted it into: the app finds
   `GM4E01` inside).
4. **Output folder:** choose where to save `hd-textures.bin` (about 800 MB).
5. **Convert.** It takes about 5-15 minutes depending on the computer, and keeps the computer awake, so you
   can walk away. When it's done, the window says so and where the file is.
6. Copy `hd-textures.bin` to `sdmc:/switch/MKDD-NX/`, next to `MKDD-NX.nro` (the name must be exactly
   `hd-textures.bin`).
7. Start the game: **HD TEXTURES** in the - menu now shows ON (switch it off to compare during a race). The
   hint on that row counts the textures currently shown in HD.

### From the command line

**Needs:** Python 3 with Pillow (`py -3 -m pip install pillow`) and a C compiler for the bundled BC7
encoder (`tools/bc7enc.c`, built automatically on first run). On Windows that is MSYS2's UCRT64 gcc
(`pacman -S mingw-w64-ucrt-x86_64-gcc` in an MSYS2 shell, found at `C:\msys64\ucrt64\bin`); elsewhere
any gcc or clang. Set `CC` to use another compiler.

1. Download and extract the English pack. Find its `GM4E01` folder (the one Dolphin would load from
   `Load/Textures/GM4E01`, full of `tex1_..._.png` files).
2. Convert it (about 6 minutes on a 12-thread PC; the result is about 790 MB).

   ```bash
   py -3 tools/hdtex_pack.py "D:/Downloads/MKDD HD/GM4E01" "D:/Downloads/hd-textures.bin"
   ```

3. Copy it to the SD card and turn it on, as in steps 6 and 7 above.

The app is built from `tools/hdtex_app/` (`build_app.py`; GitHub Actions builds the Windows and Mac
versions for each release).

Textures are matched the way Dolphin matches them (format, size and the XXH64 hash of the
original texture), so other packs in Dolphin's `tex1_` format work the same way. HD textures load
in the background from the SD card: a texture shows in its original form for a moment, then
switches to HD. Up to about 80 MB of GPU memory is used at a time; races run at 60 fps as with
the original textures.

## Reporting bugs

1. Press **-**, open **Debugging** and set **Logging** to **GAME** (GAME + AUDIO for sound problems).
2. Play until the problem happens again, then exit the game from the menu (**Exit game**) if you
   can. After a crash or freeze, restart the console first.
3. Put the SD card in a PC and zip (or copy) the whole `sdmc:/switch/MKDD-NX/logs/` folder.
4. Open an issue on [GitHub](https://github.com/digdat0/mkdd-nx/issues): describe what you did and
   what went wrong (the track, mode, settings, docked or handheld), attach the zip, and add a
   screenshot or video if it helps.

Set Logging back to DISABLED afterwards. The logs contain no game data or personal information,
only the port's own messages.

## Build

See [Build Instructions](https://github.com/digdat0/mkdd-nx/wiki/Build-Instructions) on the wiki: one command builds `MKDD-NX.nro` from your disc image.

## Credits and licence

Based on [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp) (Rust/Bevy, PC): its
spreadsheet pipeline, runtime design and renderer were ported to C / GLES here. Function names
come from the [doldecomp/mkdd](https://github.com/doldecomp/mkdd) decompilation (CC0).

- [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp): the spreadsheet pipeline,
  runtime design and renderer this port is based on; the build fetches its sheets and scripts.
- [doldecomp/mkdd](https://github.com/doldecomp/mkdd) (CC0): function names and structures.
- [GameCubeLoader](https://github.com/Cuyler36/Ghidra-GameCube-Loader), Ghidra, devkitPro.
- The [MKDD HD texture pack](https://sites.google.com/view/mkddhd/home-page) team, for the textures
  `tools/hdtex_pack.py` converts (not included here).

The code in this repository is released under the MIT licence (`LICENSE`). Mario Kart:
Double Dash!! and its characters are Nintendo's; this project is not affiliated with Nintendo.
