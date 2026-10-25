# MKDD-NX: Mario Kart: Double Dash!! 
Static recompilation of the GameCube game Mario Kart: Double Dash!! for Nintendo Switch (USA, GM4E01). 

**No game code or data is in this repository.** 
**Bring your own legally dumped disc image of Mario Kart: Double Dash!! (USA, GM4E01). We do not condone piracy and will not respond to requests for game image files!**

## Features

**Game**

- Close to 60 fps at stock Switch clocks; the heaviest moments drop a few frames. 
- Plays the USA release (GM4E01) from an `.iso`, `.gcm`, `.ciso` or `.rvz` disc image.
- 4:3 or 16:9. In 16:9 the game camera is genuinely wider rather than stretched.
- Field of view options: Normal, Wide, Wider, Widest, Wideee.
- **Single player battle mode:** Balloon Battle, Shine Thief and Bob-omb Blast against three computer
  karts, on the game's six arenas and all 24 of MKDD Plus's. The CPUs drive routes built into the app,
  chase karts and item boxes, aim their items and run when down to their last balloon. See
  [Battle mode](#battle-mode).
- **Kart FPV and Driver FPV:** first person with the kart's front in view, or from the driver's eyes.
  The eye follows the driver's head, so every kart and character sits right; height, forward and
  tilt are adjustable. Per player, on the right stick click.
- **Character packs:** extra characters from GameBanana and the MKDD wiki on more select screen
  pages. See [Character packs](#character-packs).
- Camera options: rear view on Y, camera height and distance, and a free camera on the right stick
  that turns 360° around your kart.
- Texture filtering: 2x, 4x, 8x or 16x anisotropic.
- Higher  resolution: up to 2x docked (1080p output) and 1.5x handheld, with an Auto mode
  that lowers it only when the frame rate would drop.
- Colour options: Accurate, Vivid, or your own brightness, contrast and saturation.
- Up to 4 players, controller syncing from the game menu, and button remapping.
- Unlock-all option that never touches your save.
- Race HUD options: a MODERN style for 1 to 4 players, MPH or KM/H, and hiding the position, items,
  laps, map, speedometer or race order one by one.
- Jump button, and 1 to 7 laps per Grand Prix or Versus race.
- Cheats: no weight classes, any special item for any character, stronger mini-turbos (fewer
  sweeps, more speed, longer boosts, for CPUs too if you like) and perfect starts.
- In-game updater that installs the latest GitHub release.
- On-screen FPS counter, and debug logging for bug reports (both under Debug).

**Resolution**

- **Handheld:** the 1280×720 screen. By default (Auto) the game renders at 1.5× (960×720) and steps
  down to 1.25× or the original GameCube resolution (about 640×480) when the frame rate would drop.
  4:3 shows a picture with bars at the sides. 16:9 fills the screen with a
  wider camera, not a stretch; the field of view setting picks how wide.
- **Docked:** the output is 1080p. By default (Auto) the game renders at 2× (1280×960) and steps
  down to 1.75×, 1.5× or 1.25× when the frame rate would drop.

**Mods**

- The [MKDD HD texture pack](https://sites.google.com/view/mkddhd/home-page): see [HD textures](#hd-textures).
- The [Mario Kart: Double Dash!! Plus](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus) mod (v3.3 NTSC):
  see [MKDD Plus](#mkdd-plus).
- The [Retro Track Grand Prix 2.1](https://gamebanana.com/mods/594415) mod: see [Retro Track Grand Prix](#retro-track-grand-prix).
- Custom characters from [GameBanana](https://gamebanana.com/games/6555) and the
  [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/): see [Character packs](#character-packs).

## Getting started

**You need:**

- A Switch running custom firmware (Atmosphere) with the homebrew menu.
- Your own dump of **Mario Kart: Double Dash!! (USA)**, game ID **GM4E01**, as `.iso`, `.gcm`,
  `.ciso` or `.rvz` (Dolphin's compressed format, read as-is: no need to convert it, and it takes
  about a third of the space). Other regions (PAL GM4P01, Japan GM4J01) and the bundle/demo discs don't work: the game
  checks the disc's `main.dol` and refuses any other version.
- `MKDD-NX.nro` from the releases section.

**Setup:**

1. On the SD card, create the folder `sdmc:/switch/MKDD-NX/`.
2. Copy `MKDD-NX.nro` into it.
3. Copy your disc image into the same folder. If there is more than one, the game uses an `.iso` /
   `.gcm` first, then `.ciso`, then `.rvz`. Swapping an `.iso` for an `.rvz` (or back) is fine: saves,
   settings and HD textures carry over.
4. Optional: HD textures in `mods/hd/`, the MKDD Plus mod in `mods/plus/`, the Retro Track Grand Prix
   patch in `mods/`, extra characters in `mods/characters/` (see [HD textures](#hd-textures),
   [MKDD Plus](#mkdd-plus), [Retro Track Grand Prix](#retro-track-grand-prix) and
   [Character packs](#character-packs)).
5. Start it from the homebrew menu with **title takeover**: hold **R** while starting any game
   from the HOME menu, then pick MKDD-NX. (The album / applet mode has too little memory.)

```
sdmc:/switch/MKDD-NX/
    MKDD-NX.nro
    Mario Kart - Double Dash!! (USA).iso     (or .rvz)
    mods/hd/hd-textures.bin  (optional)
    mods/plus/files, sys     (optional: MKDD Plus)
    mods/MKDD RTGP 2.1 Update.xdelta   (optional: Retro Track Grand Prix, installed on the next start)
    mods/characters/         (optional: character packs)
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
  ZR = R (drift), R = Z, left stick (or the D-pad) = control stick. ZL = jump (when Game >
  Jump is on), L or Y = rear view in races, right stick click = Kart FPV on and off, right stick
  sideways = free camera. Remappable in the menu (applies to all of these; **Reset** on the Button
  mapping page gives these defaults again). In menus L and R are both Z, as before.
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
| Game | the page below |
| Graphics | the page below |
| Camera | the page below |
| Controls | **Button mapping** (each Switch button's GameCube button or action: Driver FPV, Kart FPV, jump, camera reset; A, B, R, X and Start always keep a button, the rest may have none) and **Sync controllers** (the system screen for pairing, the player order and single Joy-Cons) |
| Mods | **Level pack**: which installed mod to play, PLUS or RTGP, or OFF for the standard game (NONE FOUND when no mod is installed; see [MKDD Plus](#mkdd-plus) and [Retro Track Grand Prix](#retro-track-grand-prix)). **HD textures** (Off (default) / On when `mods/hd/hd-textures.bin` is in the app folder (see [HD textures](#hd-textures)), else NO PACK), **Single player battle mode** (On (default) / Off: computer karts fill battles up to 4 karts; with one player, Battle is on the mode page) and **Restart now**, which starts the game again to apply a Level pack change (unsaved race progress is lost) |
| Cheats | the page below |
| Exit game | back to the homebrew menu. **Update** and **Debug** sit on the same row: press right |
| Update | shows this version and the latest release; **Check for updates** downloads and installs the newest `MKDD-NX.nro`, then **Restart now** starts it (save your progress first). The button reads UPDATE: NEW when there is one |
| Debug | the page below |

**Graphics**

| Option | |
|---|---|
| Widescreen | 16:9 (default): wider game camera, race HUD at the screen edges, menus 4:3 in the middle; or 4:3 |
| 4:3 edge border | Off / On: checkered border art in the side bars during races in 4:3 (menus keep black bars) |
| Docked resolution | on the TV (1080p output): Native (640x480), 1.5X, 2X (1280x960) or Auto (default: 2X, stepping down when the frame rate drops); handheld uses Handheld resolution |
| Handheld resolution | on the Switch screen (1280x720): Native (the game's own 640x480), 1.25X, 1.5X (960x720, as many lines as the screen) or Auto (default: 1.5X, stepping down when the frame rate drops) |
| Texture filtering | Normal / 2x / 4x / 8x / 16x anisotropic filtering: sharper roads and walls at a distance (most visible with HD textures, which have mipmaps) |
| Color | Accurate / Vivid / Custom brightness, contrast, saturation (hold Y on that page to preview); saved to `config/color.ini` |

**Camera**

| Option | |
|---|---|
| Camera view | Normal (the game's chase camera), Driver FPV (first person from the driver's eyes) or Kart FPV (first person with the kart's front in view), in 1 to 4 player races. Both follow the driver's head, so every kart and character gets its own eye. With either chosen, A opens its page: eye height and eye forward (steps of 5, from the head), tilt (degrees down) and defaults, also in settings.ini (`driver_eye_up`, `driver_eye_forward`, `driver_tilt`, `kart_eye_up`, `kart_eye_forward`, `kart_tilt`). Player 1's setting; each player's right stick click switches their own camera between Normal and Kart FPV (Driver FPV can get a button too, in Button mapping). Y looks behind you in any view |
| Camera height | Low / Medium / Regular (default, the game's) / High: the chase camera's height; it never goes below the kart |
| Camera distance | Very close / Close / Regular (default, the game's) / Far / Very far: how far the chase camera sits behind the kart |
| Field of view | Normal / Wide / Wider / Widest / Wideee race camera, kept separately for 4:3 (default Normal) and 16:9 (default Wider); the row changes the current display mode's |
| Free camera | the right stick (a GameCube pad's C-stick) turns the camera around the kart. When you let go: Snap back (default), Ease back (glides back in about a third of a second) or Stay (until **Camera reset**, a button you choose in Button mapping). Off turns it off. Not on a single sideways Joy-Con |

**Game**

| Option | |
|---|---|
| Race HUD | Classic (the game's) or Modern: smaller pieces in the screen corners, 1 to 4 players |
| Speed | the speedometer in MPH or KM/H |
| HUD assets | turn each part of the race HUD on or off (position, items, laps, map, speedometer, race order; left/right turns them all on or off), 1 to 4 players |
| Jump height | Off / Low / Mid (default) / High: a hop on the Jump button (ZL by default) that keeps your speed and follows the slope |
| Rumble strength | Off / Low (default) / High; the game's own Rumble option must be on too |

**Cheats**

| Option | |
|---|---|
| Unlock all | every character, kart, cup and mode while it is on; never written to your save, so your own progress stays as it is |
| No weight classes | any unlocked kart for any pair of drivers, whatever their weight |
| Special items | Normal, Any (any character can get any special item), or Custom (choose each character's) |
| Mini-turbo sweeps | how many steering sweeps charge a mini-turbo: 3 (the game's), 2 or 1 |
| Mini-turbo speed | up to +100% boost speed (the speed cap rises with it while boosting) |
| Mini-turbo length | up to 4x as long a boost |
| Mini-turbo for CPUs | the computer's karts get the three mini-turbo settings above too (off: only the players') |
| Perfect starts | every player gets the start boost at GO, without timing the button (holding it through the countdown no longer burns out) |
| Laps | Normal (the course's) or 1 to 7 laps, for Grand Prix and Versus races from the next race; Time Trial and Battle keep theirs |

**Debug**

| Option | |
|---|---|
| Show FPS | Off / FPS / FPS + times: ms per game frame of the game thread (GAME), the graphics thread (GX) and the render thread (RENDER); 16.7 is the 60 fps budget |
| Logging | debug logs on the SD card, for bug reports |
| Profiler | Off / On: a sampling profile (`logs/mkdd_prof.bin`) for performance reports, from the next start |

Settings are saved to `config/settings.ini`.

## Battle mode

The GameCube game only has battles for 2 to 4 players. MKDD-NX adds **single player battles** against
three computer karts (on by default: **Mods > Single player battle mode**). With one player, the mode
page shows **Battle**; pick Balloon Battle, Shine Thief or Bob-omb Blast and an arena as usual. In 2
and 3 player battles, computer karts fill the empty places up to 4.

- **Arenas:** the game's six and all 24 of MKDD Plus's.
- **The CPUs** drive routes made for each arena (the game has none for battles), built into the
  `.nro`: nothing to install. Each battle they take different routes. They go for item boxes, chase
  and aim at the nearest karts (shells forward or backward, bananas behind them), steal the Shine,
  and run when they're down to their last balloon. Smaller arenas are fiercer, bigger ones calmer.
- **Your own routes:** `tools/route_editor.py` edits them on a PC, and a `routes/<key>.txt` in the
  app folder is used instead of the built-in route for that arena. See
  [docs/BATTLE_ROUTES.md](docs/BATTLE_ROUTES.md).

## Character packs

Extra characters made by the community play in MKDD-NX on more pages of the character select screen.
Each one replaces a character's model, face and name, and plays with that character's weight, stats,
voice and special item. Your disc image and saves are not changed.

**Where to get them:** [Mario Kart: Double Dash!! on GameBanana](https://gamebanana.com/games/6555)
(Skins and Characters) and the MKDD wiki's
[List of Custom Characters](https://mkdd.org/wiki/List_of_Custom_Characters/). Download and unzip
them on a PC. Most come as a folder for the MKDD Patcher, with `modinfo.ini` and a `files` folder.

**The easy way: the MKDD-NX Characters app** (`MKDD-NX-Characters.exe` from the releases, Windows,
nothing to install):

1. Start it. **Add mod or folder of mods…** (or drop folders on the window) adds the characters you
   downloaded: one mod, or a folder holding many. Mods the game can't use are rejected, with the reason.
2. Optional: **Disc image** (your own `.iso`): each model is checked against the character it replaces.
   The disc image is only read.
3. For each character, check or change **Name**, **On** (the character it plays as) and **Cell** (where
   it shows on the select screen); **Author** and **Source link** are kept with it.
4. **Output folder:** `sdmc:/switch/MKDD-NX/mods/characters/` on the SD card (or a folder on the PC to
   copy there), then **Export**. Each character becomes its own folder with a `character.ini`.

**By hand:** copy a pack's `files` folder (the one the MKDD Patcher uses) into
`mods/characters/<pack name>/`. Every driver model in it becomes a character. A `names.txt` next to
`files` names them, one line each, for example `nokonoko=SHY GUY`.

```
sdmc:/switch/MKDD-NX/mods/characters/
    shyguy/            (made by the app)
        character.ini
        driver.bmd
        face.bti, map.bti, name.bti   (optional)
    Some Pack/         (copied by hand)
        files/MRAM.arc/mram/driver/nokonoko/driver.bmd ...
        names.txt      (optional)
```

**In the game:** on the character select screen, **X** and **Y** turn the pages; page 1 is the game's
20. A cell with no character on that page is dimmed. Random picks from every page. Up to 128
characters. A missing map icon or name tag is made from the face and the name. A race never has the
same character twice, so a character and the one it plays as can't race together.

## HD textures

MKDD-NX can use the community **MKDD HD texture pack** made for Dolphin
([mkddhd](https://sites.google.com/view/mkddhd/home-page); use the **English** version, downloaded from the
Mega link on that site). The pack is not included: download it yourself and convert it once on a PC.
The Switch can't use the pack's PNGs directly (about 1 GB, and decoding them would stall the game), so
`tools/hdtex_pack.py` turns them into one file of GPU-ready BC7 textures, at the pack's full
resolution with mipmaps.

### The easy way: MKDD-NX HD Textures app

Download the app for your computer from the [v1.4.1 release](https://github.com/digdat0/mkdd-nx/releases/tag/v1.4.1):
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
6. Copy `hd-textures.bin` to `sdmc:/switch/MKDD-NX/mods/hd/` (create the folders; the name must be
   exactly `hd-textures.bin`).
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

## MKDD Plus

MKDD-NX can also play the **Mario Kart: Double Dash!! Plus** mod (version **3.3, NTSC**), with its
extra cups and courses, from the files the mod comes as. Nothing is patched: your disc image stays
as it is, and the standard game stays playable.

1. Get MKDD Plus 3.3 [NTSC] (see the [MKDD Plus wiki page](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus)) and extract it. You get a folder with `files` and `sys` in it.
2. On the SD card, create `sdmc:/switch/MKDD-NX/mods/plus/` and copy the mod's `files` and `sys`
   folders into it:

   ```
   sdmc:/switch/MKDD-NX/
       MKDD-NX.nro
       Mario Kart - Double Dash!! (USA).iso
       mods/
           hd/
               hd-textures.bin   (optional)
           plus/
               files/
               sys/
   ```

   Copy it as it is: don't delete `Course`, `CourseName`, `Movie` or `StaffGhosts` (the mod's
   folders don't have them; the steps for building a modded ISO on a PC don't apply here).
3. Start the game: with the mod in `mods/plus/`, MKDD-NX plays Plus. To play the standard game
   again, set **Level pack** to OFF on the menu's **Mods** page and choose **Restart now** (or move or
   rename the `plus` folder). If a mod file is missing, the standard game starts
   instead and `logs/mkdd.log` names the file.

Plus keeps its own saves in `save/plus/`, apart from the standard game's. The first time you play
Plus, if you have a standard save, the game offers to start Plus from a copy of it (unlocks and
records; ghosts aren't copied, and your standard save stays as it is). On the select-mode screen
the classes are listed 50cc, 150cc, 200cc, Mirror (the mod itself puts 200cc second). In a cup,
X and Y switch between the mod's course sets. Its intro movie doesn't
play (the mod removes it). HD textures still apply where the mod uses the game's own textures
(characters, karts, menus it didn't change); its new courses show their original textures. Only
version 3.3 NTSC works: the game's code is built for that exact version, and another one is
refused with a message.

## Retro Track Grand Prix

MKDD-NX can also play **Retro Track Grand Prix 2.1** (RTGP), with its retro cups and courses. It comes
as a patch of the game's disc; MKDD-NX installs it on the Switch, so you don't need a PC tool to patch
your disc image, and the standard game stays playable.

1. Download `mario_kart_double_dash_rtgp_2_1.zip` from the mod's [GameBanana page](https://gamebanana.com/mods/594415) and
   unzip it. The mod is too big for GameBanana: the zip only holds `OPEN ME.txt` with the link to the
   patch. Download the patch from that link: `MKDD RTGP 2.1 Update.xdelta` (about 2 GB).
2. On the SD card, create `sdmc:/switch/MKDD-NX/mods/` and copy the `.xdelta` file into it, as it is
   (any name ending in `.xdelta` works):

   ```
   sdmc:/switch/MKDD-NX/
       MKDD-NX.nro
       Mario Kart - Double Dash!! (USA).iso     (or .rvz)
       mods/
           MKDD RTGP 2.1 Update.xdelta
   ```

3. Start the game. It finds the patch and asks **INSTALL RETRO TRACK GRAND PRIX 2.1?**: choose
   **INSTALL NOW**. It takes a few minutes (B cancels) and writes the mod's files to `mods/rtgp/`
   (about 2 GB more on the SD card; the patch is applied to your own disc image, which stays as it is,
   and an `.rvz` works as well as an `.iso`).
4. When it's done, the game offers to delete the `.xdelta` file: it is no longer needed, and deleting it
   frees about 2 GB. Then the game starts in RTGP.

To switch between RTGP, Plus and the standard game, change **Level pack** on the menu's **Mods** page and
choose **Restart now**. RTGP keeps its own saves in `save/rtgp/`, apart from the standard game's and
Plus's (the first time, the game offers to start from a copy of your standard save). In a cup, X and Y
switch between the mod's course sets. Only version 2.1 works; a patch for another version or another
game is reported on screen and left alone.

## Reporting bugs

1. Press **-**, go down to **Exit game**, press right twice for **Debug**, open it and set **Logging** to **GAME** (GAME + AUDIO for sound problems).
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

How the port works, and how it is tested: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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
- **That1loser4**, creator of [Mario Kart: Double Dash!! Plus](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus),
  and everyone who contributed courses to it. The mod is not included here: get it from its own release.
- **Portable Productions**, creators of [Mario Kart: Double Dash!! Retro Track Grand Prix](https://gamebanana.com/mods/594415),
  with additional credits to **ShaneMGD**, **LMFinish**, **AdolescentSeagull**, **Tarsa**, **Yoshi2** and
  **Cristian64**. The mod is not included here: get it from its own page.
- The creators of the custom characters on [GameBanana](https://gamebanana.com/games/6555) and the
  [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/). None are included here: each
  character's author is kept in its `character.ini` (or its pack's `modinfo.ini`).
- **realneonplayz** on [SteamGridDB](https://www.steamgriddb.com/), for the app icon.

The code in this repository is released under the MIT licence (`LICENSE`). Mario Kart:
Double Dash!! and its characters are Nintendo's; this project is not affiliated with Nintendo.
