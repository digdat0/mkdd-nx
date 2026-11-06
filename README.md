# MKDD-NX: Mario Kart: Double Dash!! 
Static recompilation of the GameCube game Mario Kart: Double Dash!! for Nintendo Switch and Windows (USA, GM4E01). 

**No game code or data is in this repository.** 
**Bring your own legally dumped disc image of Mario Kart: Double Dash!! (USA, GM4E01). We do not condone piracy and will not respond to requests for game image files!**


Note: This is an AI assisted project. 

## Features

**Platforms**

- Nintendo Switch (`.nro`) and Windows x64 (`.exe` with a launcher: see [Windows](https://github.com/digdat0/mkdd-nx/wiki/Windows)).
- Plays the USA release (GM4E01) from `.iso`, `.gcm`, `.ciso` or `.rvz`.
- In-game updater on Switch.

**Gameplay**

- **Tricks:** flips, spins, doubles, combos and rider tricks off ramps, with a landing boost. See [Tricks](https://github.com/digdat0/mkdd-nx/wiki/Tricks).
- **Drafting:** a slipstream boost behind other karts.
- **Single player battles** against three CPUs on the game's 6 arenas and MKDD Plus's 24. See [Battle mode](#battle-mode).
- **LAN play over Wi-Fi** between Switches and PCs, no adapters. See [LAN play](#lan-play).
- Jump button, 1 to 7 laps per race.
- Cheats: no weight classes, any special item, stronger mini-turbos, perfect starts, unlock all (your save is never touched).

**Players and controls**

- Up to 4 players, side-by-side split screen for two.
- Each player gets their own camera and button mapping.
- Button remapping, single sideways Joy-Cons, GameCube controllers, rumble.

**Camera**

- **Driver FPV and Kart FPV** first person, tuned per kart.
- Rear view, camera height and distance, and a 360° free camera.
- Field of view from Normal to Wideee.

**Graphics**

- Close to 60 fps at stock Switch clocks.
- True 16:9: a wider camera, not a stretch.
- Up to 4x resolution docked and 3x handheld, with an Auto mode that keeps the frame rate.
- Anisotropic filtering and colour options.
- Race HUD styles: Classic, Modern and Dashboard (speed dial and rear-view mirror), MPH or KMH, every piece can be hidden.

**Mods** (see [Mods](#mods))

- [MKDD Plus](https://github.com/digdat0/mkdd-nx/wiki/MKDD-Plus) 3.3, with its stereo music.
- [Retro Track Grand Prix](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix) 2.1, Baby Mode and Classic.
- [HD textures](https://github.com/digdat0/mkdd-nx/wiki/HD-Textures-App).
- Up to 256 [custom characters](https://github.com/digdat0/mkdd-nx/wiki/Characters-App) and [extra karts](https://github.com/digdat0/mkdd-nx/wiki/Extra-Karts).
- Mario Kart Wii [drivers](https://github.com/digdat0/mkdd-nx/wiki/Wii-Characters) and [karts](https://github.com/digdat0/mkdd-nx/wiki/Wii-Karts), converted with MKDD-NX Tools; characters can also be exported for a real GameCube.

**Resolution details**

- **Handheld (1280×720):** Auto renders at 1.5× (960×720) and steps down when needed; 2× and 3× are fixed (3× needs an overclock for 60 fps).
- **Docked (1080p):** Auto renders at 2× (1280×960) and steps down when needed; 3× and 4× are fixed (4× holds about 60 fps with one player).

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
   patches (2.1, Baby Mode, Classic) in `mods/`, extra characters in `mods/characters/` (see [Mods](#mods)).
5. Start it from the homebrew menu with **title takeover**: hold **R** while starting any game
   from the HOME menu, then pick MKDD-NX. (The album / applet mode has too little memory.)

```
sdmc:/switch/MKDD-NX/
    MKDD-NX.nro
    Mario Kart - Double Dash!! (USA).iso     (or .rvz)
    mods/hd/hd-textures.bin  (optional)
    mods/plus/files, sys     (optional: MKDD Plus)
    mods/MKDD RTGP 2.1 Update.xdelta   (optional: Retro Track Grand Prix, installed on the next start)
    mods/Baby Mode 1_4.xdelta          (optional: RTGP Baby Mode, the same way)
    mods/Mario Kart Double Dash RTGP Classic OST 1_4.xdelta   (optional: RTGP Classic, the same way)
    mods/characters/         (optional: character packs)
    mods/audio/              (optional: MKDD Plus Expansion Pack, stereo music)
```

The first start takes longer while shaders are compiled; later starts reuse them. Press **-** at
any time for the options menu.

### On Windows

1. Unzip `MKDD-NX-Windows-<version>.zip` from the releases (Windows 10 or 11, x64, a GPU with OpenGL 4.5).
2. Put your disc image next to `MKDD-NX.exe` and run it. The launcher opens first (disc, graphics,
   audio, controllers, keyboard, mods); press **PLAY**.

The folders are the same as on the Switch, so the Switch's `switch/MKDD-NX` folder (saves, settings,
mods) can be copied over as it is. Launcher, keyboard keys and controllers:
[Windows](https://github.com/digdat0/mkdd-nx/wiki/Windows).

### App folder

The app creates three folders next to the `.nro` and the disc image:

| Folder | |
|---|---|
| `save/` | standard `.gci` saves in `save/card_a/` (usable in Dolphin's GCI-folder mode and save editors); each file's previous version is kept in `save/backup/` |
| `config/` | `settings.ini`, `color.ini`, `shaders.bin` (the shader states seen) and `dksh.bin` (compiled shaders: delete it to rebuild) |
| `logs/` | `mkdd.log`, `mkdd_trace.log`, `crash.log`, `stderr.log`, `boot.txt` (the previous run's as `*.prev.log`) |

### Controls

Up to 4 players. Controllers become players in order: wireless controllers as the system numbers
them, then the console in handheld mode (so a Pro Controller is player 1 as soon as it wakes, and
the attached Joy-Cons are player 2). **Controls > Sync controllers** in the - menu opens the system screen for pairing,
the player order and single Joy-Cons (L + R pairs two Joy-Cons; one Joy-Con alone: hold it sideways
and press SL + SR). A Joy-Con left on its own for 2 seconds becomes a single sideways controller by
itself. Connects and disconnects are logged (`pad:` lines in `mkdd.log`). Full details:
[Controls](https://github.com/digdat0/mkdd-nx/wiki/Controls).

- **Handheld, Pro Controller, Joy-Con pair:** A/B/X/Y as on the GameCube, + = Start,
  ZR = R (drift), R = Z, left stick (or the D-pad) = control stick. ZL = L (drift / co-op rider slide
  left), JUMP has no button until you give it one in Button mapping, L or Y = rear view in races, right stick click = Kart FPV on and off, right stick
  sideways = free camera. Remappable in the menu (applies to all of these; **Reset** on the Button
  mapping page gives these defaults again). In menus L and R are both Z, as before.
- **Single Joy-Con, held sideways (rail on top):** the stick steers as held, the four face buttons
  sit where a GameCube pad's A (right), B (bottom), X (top) and Y (left) are, in races and menus;
  SL and SR = drift, the shoulder or stick click = Z, + or ZL / ZR = Start (- too for players 2-4).
  Remappable for driving on its own page (**Controls > Joy-Con sideways**). Player 1 on a single
  right Joy-Con opens the menu with stick click + +. HOME and Capture belong to the system and
  can't be remapped.
- **GameCube controller (official adapter):** its own layout, with analog L/R. Player 1 opens the menu by
  holding Z and pressing Start.

Rumble works on controllers with HD Rumble and on GameCube controllers.

### Options menu (-)

Press **-** at any time for the options menu: **Game**, **Graphics**, **Camera**, **Controls**,
**Multiplayer**, **Mods**, **Cheats**, **Update** and **Debug**. The game pauses while it is open
(except in LAN play, where the game carries on behind it). To keep cheats away from children,
**Controls > Open Menu** can require holding **-** for 3 or 5 seconds (off by default). Every option is
described on the [Options Menu wiki page](https://github.com/digdat0/mkdd-nx/wiki/Options-Menu). Settings are saved to `config/settings.ini`.

## Battle mode

The GameCube game only has battles for 2 to 4 players. MKDD-NX adds **single player battles** against
three computer karts (on by default: **Game > Single player battle mode**). With one player, the mode
page shows **Battle**; pick Balloon Battle, Shine Thief or Bob-omb Blast and an arena as usual. In 2
and 3 player battles, computer karts fill the empty places up to 4.

**Game > Single player battle mode > Battle karts** sets how many karts a single player battle has, 4 to 8 (you and up to seven
computer karts). The computer karts can be any character, unlocked or not. **Bob-omb hits to
win** makes a single player Bob-omb Blast last longer, 4 (the game's own) to 20 hits: the score still shows 4 stars,
each lighting after its share of the hits, and being hit takes one hit off.

- **Arenas:** the game's six and all 24 of MKDD Plus's.
- **The CPUs** drive routes made for each arena (the game has none for battles), built into the
  `.nro`: nothing to install. Each battle they take different routes. They go for item boxes, chase
  and aim at the nearest karts (shells forward or backward, bananas behind them), steal the Shine,
  and run when they're down to their last balloon. One stuck on a wall or an object backs out and
  rejoins its route. Smaller arenas are fiercer, bigger ones calmer.
- **Your own routes:** MKDD-NX Tools' Battle Routes (Windows and Mac, from the releases) edits them; a
  `routes/<key>.txt` in the app folder is used instead of the built-in route for that arena. See the
  [Route Editor wiki page](https://github.com/digdat0/mkdd-nx/wiki/Route-Editor), and [docs/BATTLE_ROUTES.md](docs/BATTLE_ROUTES.md) for the
  automatic route maker and the file format. The built-in routes are in the [routes](routes) folder.

## LAN play

The GameCube game has a LAN mode for consoles linked by Broadband Adapters and cables. MKDD-NX plays
it over Wi-Fi instead: turn on **Multiplayer > LAN play** in the − menu on each Switch, choose
**Restart now**, then pick **Wi-Fi mode** on the title screen. Every console needs this version of
MKDD-NX, and all of them must be on the same Wi-Fi network.

- **Standard game only:** level pack OFF. Retro Track Grand Prix and its Baby Mode / Classic can't
  (their LAN mode runs code the app doesn't have), and MKDD Plus hasn't been tested.
- From the first Wi-Fi mode screen until the app restarts, the port's gameplay changes (cheats,
  laps, any kart, single player battle CPUs) are off on every console, so all of them play the
  same race.
- In a race, + opens a pause screen with **Continue** and **Quit**, but the race carries on behind
  it. **Quit** leaves the session and goes back to the title screen.
- Networks that block broadcasts (some guest or office Wi-Fi): set **LAN peer** to the other
  console's address, which is shown on the same page.

Setup and troubleshooting: [LAN Play wiki page](https://github.com/digdat0/mkdd-nx/wiki/LAN-Play).

## Mods

None of these are included: get each one from its own page. They live in `mods/` next to the `.nro`,
and your disc image is never changed. Step-by-step instructions are on the wiki.

| Mod | What you get | On the SD card | Instructions |
|---|---|---|---|
| [MKDD Plus](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus) 3.3 NTSC | extra cups, courses and 24 battle arenas | `mods/plus/files`, `sys` | [wiki: MKDD Plus](https://github.com/digdat0/mkdd-nx/wiki/MKDD-Plus) |
| [MKDD Plus Expansion Pack](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus) | Plus's track music in stereo (Plus only, about 720 MB; **Mods > Stereo Music**, on by default) | the download's `files` folder in `mods/audio/` | [wiki: Stereo Music](https://github.com/digdat0/mkdd-nx/wiki/Stereo-Music) |
| [Retro Track Grand Prix](https://gamebanana.com/mods/594415) 2.1 (`mario_kart_double_dash_rtgp_2_1.zip`) | retro cups and courses | `mods/MKDD RTGP 2.1 Update.xdelta`, installed on the Switch at the next start | [wiki: Retro Track Grand Prix](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix) |
| [RTGP Baby Mode](https://gamebanana.com/mods/download/594415) 1.4 (`rtgp_baby_mode_1_4.zip`) | RTGP made easier, for newcomers | `mods/Baby Mode 1_4.xdelta`, installed the same way | [wiki: Baby Mode and Classic](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix#baby-mode-and-classic) |
| [RTGP Classic](https://gamebanana.com/mods/download/594415) 1.4 (`mario_kart_double_dash_rtgp_classic_ost.zip`) | RTGP with the classic soundtrack | `mods/Mario Kart Double Dash RTGP Classic OST 1_4.xdelta`, installed the same way | [wiki: Baby Mode and Classic](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix#baby-mode-and-classic) |
| [MKDD HD texture pack](https://sites.google.com/view/mkddhd/home-page) (English) | the game's textures in HD | `mods/hd/hd-textures.bin`, converted once on a computer | [wiki: HD Textures](https://github.com/digdat0/mkdd-nx/wiki/HD-Textures-App) |
| Custom characters from [GameBanana](https://gamebanana.com/games/6555) and the [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/) | up to 256 extra characters on more select screen pages | `mods/characters/` | [wiki: Characters App](https://github.com/digdat0/mkdd-nx/wiki/Characters-App) |
| Mario Kart Wii drivers (`.szs`) | Wii characters, converted on a computer | `mods/characters/` | [wiki: Wii Characters](https://github.com/digdat0/mkdd-nx/wiki/Wii-Characters) |
| Kart mods (MKDD Patcher layout) from [GameBanana](https://gamebanana.com/games/6555) and the [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/Vehicles#Vehicles) | Extra karts next to the game's, driving like the kart they are added to (**Mods > Extra Karts**); no conversion needed | `mods/karts/` | [wiki: Extra Karts](https://github.com/digdat0/mkdd-nx/wiki/Extra-Karts) |
| Mario Kart Wii karts (`.szs`, not bikes) | Extra karts, converted on a computer | `mods/karts/` | [wiki: Wii Karts](https://github.com/digdat0/mkdd-nx/wiki/Wii-Karts) |

- **Level packs:** each keeps its own saves (`save/plus/`, `save/rtgp/`, `save/rtgpbaby/`,
  `save/rtgpclassic/`). Pick one with **Level pack** on the − menu's **Mods** page (OFF: the standard
  game), then **Restart now**.
- **MKDD-NX Tools**, one app for Windows and Mac, from the [latest release](https://github.com/digdat0/mkdd-nx/releases/latest)
  (`MKDD-NX-Tools.exe`, or `MKDD-NX-Tools-arm64.dmg` / `-x86_64.dmg`), a tab for each tool:
  **HD Textures** (converts the texture pack), **Export MKDD Characters** (prepares character mods),
  **Convert WiiMK Characters** (beta: Mario Kart Wii drivers; drivers only, no karts, and Mii outfits
  aren't supported), **Convert WiiMK Karts** (beta: Mario Kart Wii karts, no bikes) and **Battle Routes**
  (edits the battle CPUs' routes).

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
Lessons from building it, as a guide for porting another game you own the same way:
[docs/RECOMP_PORT_GUIDE.md](docs/RECOMP_PORT_GUIDE.md).

## Credits and licence

Based on [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp) (Rust/Bevy, PC): its
spreadsheet pipeline, runtime design and renderer were ported to C / deko3d here. Function names
come from the [doldecomp/mkdd](https://github.com/doldecomp/mkdd) decompilation (CC0).

- [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp): the spreadsheet pipeline,
  runtime design and renderer this port is based on; the build fetches its sheets and scripts.
- [doldecomp/mkdd](https://github.com/doldecomp/mkdd) (CC0): function names and structures.
- [GameCubeLoader](https://github.com/Cuyler36/Ghidra-GameCube-Loader), Ghidra, devkitPro.
- The [MKDD HD texture pack](https://sites.google.com/view/mkddhd/home-page) team, for the textures
  `tools/hdtex_pack.py` converts (not included here).
- **That1loser4**, creator of [Mario Kart: Double Dash!! Plus](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus),
  and everyone who contributed courses to it. The mod is not included here: get it from its own release.
- **Portable Productions**, creators of [Mario Kart: Double Dash!! Retro Track Grand Prix](https://gamebanana.com/mods/594415)
  and its Baby Mode and Classic versions,
  with additional credits to **ShaneMGD**, **LMFinish**, **AdolescentSeagull**, **Tarsa**, **Yoshi2** and
  **Cristian64**. The mod is not included here: get it from its own page.
- The creators of the custom characters on [GameBanana](https://gamebanana.com/games/6555) and the
  [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/). None are included here: each
  character's author is kept in its `character.ini` (or its pack's `modinfo.ini`).
- **realneonplayz** on [SteamGridDB](https://www.steamgriddb.com/), for the app icon.
- [deko3d](https://github.com/devkitPro/deko3d) (zlib) draws the game, and
  [uam](https://github.com/devkitPro/uam) (zlib; its Mesa parts MIT) compiles its shaders on the
  console, as patched in SwitchWakerHD (`switch/third_party/uam/PATCHES.md`). The deko3d design
  follows guides by **StevensND** (nfsmw-nx) and **ChanseyIsTheBest** (sa2-nx).

The code in this repository is released under the MIT licence (`LICENSE`). Mario Kart:
Double Dash!! and its characters are Nintendo's; this project is not affiliated with Nintendo.
