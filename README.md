# MKDD-NX: Mario Kart: Double Dash!! 
Static recompilation of the GameCube game Mario Kart: Double Dash!! for Nintendo Switch (USA, GM4E01). 

**No game code or data is in this repository.** 
**Bring your own legally dumped disc image of Mario Kart: Double Dash!! (USA, GM4E01). We do not condone piracy and will not respond to requests for game image files!**


Note: This is an AI assisted project. 

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
- **LAN play over Wi-Fi:** the game's own LAN mode, between Switches on the same Wi-Fi network, with
  no cables or adapters. Standard game only. See [LAN play](#lan-play).
- **Two players side by side:** split screen left and right instead of top and bottom, with a
  HUD laid out for it (Multiplayer > Split screen).
- **Character packs:** extra characters from GameBanana and the MKDD wiki on more select screen
  pages, with a third row (30 characters a page) when there are extra characters. A custom character
  and the one it plays as can now race together. See [Mods](#mods).
- **Mario Kart Wii drivers:** MKDD-NX Tools converts a Mario Kart Wii driver into a character. See
  [Wii Characters](https://github.com/digdat0/mkdd-nx/wiki/Wii-Characters).
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

- The [MKDD HD texture pack](https://sites.google.com/view/mkddhd/home-page): see [HD Textures](https://github.com/digdat0/mkdd-nx/wiki/HD-Textures-App).
- The [Mario Kart: Double Dash!! Plus](https://mkdd.org/wiki/Mario_Kart:_Double_Dash!!_Plus) mod (v3.3 NTSC):
  see [MKDD Plus](https://github.com/digdat0/mkdd-nx/wiki/MKDD-Plus).
- The [Retro Track Grand Prix 2.1](https://gamebanana.com/mods/594415) mod: see [Retro Track Grand Prix](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix).
- Its two alternate versions, [RTGP Baby Mode 1.4](https://gamebanana.com/mods/download/594415) (easier, for newcomers:
  `rtgp_baby_mode_1_4.zip`) and [RTGP Classic 1.4](https://gamebanana.com/mods/download/594415) (with the classic
  soundtrack: `mario_kart_double_dash_rtgp_classic_ost.zip`): see [Retro Track Grand Prix](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix#baby-mode-and-classic).
- Custom characters from [GameBanana](https://gamebanana.com/games/6555) and the
  [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/): see [Characters App](https://github.com/digdat0/mkdd-nx/wiki/Characters-App).
- Mario Kart Wii drivers (`.szs`), converted into characters: see [Wii Characters](https://github.com/digdat0/mkdd-nx/wiki/Wii-Characters).

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

Press **-** at any time for the options menu: **Game**, **Graphics**, **Camera**, **Controls**,
**Multiplayer**, **Mods**, **Cheats**, **Update** and **Debug**. The game pauses while it is open
(except in LAN play, where the game carries on behind it). Every option is
described on the [Options Menu wiki page](https://github.com/digdat0/mkdd-nx/wiki/Options-Menu). Settings are saved to `config/settings.ini`.

## Battle mode

The GameCube game only has battles for 2 to 4 players. MKDD-NX adds **single player battles** against
three computer karts (on by default: **Mods > Single player battle mode**). With one player, the mode
page shows **Battle**; pick Balloon Battle, Shine Thief or Bob-omb Blast and an arena as usual. In 2
and 3 player battles, computer karts fill the empty places up to 4.

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
| [Retro Track Grand Prix](https://gamebanana.com/mods/594415) 2.1 (`mario_kart_double_dash_rtgp_2_1.zip`) | retro cups and courses | `mods/MKDD RTGP 2.1 Update.xdelta`, installed on the Switch at the next start | [wiki: Retro Track Grand Prix](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix) |
| [RTGP Baby Mode](https://gamebanana.com/mods/download/594415) 1.4 (`rtgp_baby_mode_1_4.zip`) | RTGP made easier, for newcomers | `mods/Baby Mode 1_4.xdelta`, installed the same way | [wiki: Baby Mode and Classic](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix#baby-mode-and-classic) |
| [RTGP Classic](https://gamebanana.com/mods/download/594415) 1.4 (`mario_kart_double_dash_rtgp_classic_ost.zip`) | RTGP with the classic soundtrack | `mods/Mario Kart Double Dash RTGP Classic OST 1_4.xdelta`, installed the same way | [wiki: Baby Mode and Classic](https://github.com/digdat0/mkdd-nx/wiki/Retro-Track-Grand-Prix#baby-mode-and-classic) |
| [MKDD HD texture pack](https://sites.google.com/view/mkddhd/home-page) (English) | the game's textures in HD | `mods/hd/hd-textures.bin`, converted once on a computer | [wiki: HD Textures](https://github.com/digdat0/mkdd-nx/wiki/HD-Textures-App) |
| Custom characters from [GameBanana](https://gamebanana.com/games/6555) and the [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/) | up to 256 extra characters on more select screen pages | `mods/characters/` | [wiki: Characters App](https://github.com/digdat0/mkdd-nx/wiki/Characters-App) |
| Mario Kart Wii drivers (`.szs`) | Wii characters, converted on a computer | `mods/characters/` | [wiki: Wii Characters](https://github.com/digdat0/mkdd-nx/wiki/Wii-Characters) |

- **Level packs:** each keeps its own saves (`save/plus/`, `save/rtgp/`, `save/rtgpbaby/`,
  `save/rtgpclassic/`). Pick one with **Level pack** on the − menu's **Mods** page (OFF: the standard
  game), then **Restart now**.
- **MKDD-NX Tools**, one app for Windows and Mac, from the [latest release](https://github.com/digdat0/mkdd-nx/releases/latest)
  (`MKDD-NX-Tools.exe`, or `MKDD-NX-Tools-arm64.dmg` / `-x86_64.dmg`), a tab for each tool:
  **HD Textures** (converts the texture pack), **Export MKDD Characters** (prepares character mods),
  **Convert WiiMK Characters** (beta: Mario Kart Wii drivers; drivers only, no karts, and Mii outfits
  aren't supported) and **Battle Routes** (edits the battle CPUs' routes).

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
- **Portable Productions**, creators of [Mario Kart: Double Dash!! Retro Track Grand Prix](https://gamebanana.com/mods/594415)
  and its Baby Mode and Classic versions,
  with additional credits to **ShaneMGD**, **LMFinish**, **AdolescentSeagull**, **Tarsa**, **Yoshi2** and
  **Cristian64**. The mod is not included here: get it from its own page.
- The creators of the custom characters on [GameBanana](https://gamebanana.com/games/6555) and the
  [MKDD wiki](https://mkdd.org/wiki/List_of_Custom_Characters/). None are included here: each
  character's author is kept in its `character.ini` (or its pack's `modinfo.ini`).
- **realneonplayz** on [SteamGridDB](https://www.steamgriddb.com/), for the app icon.

The code in this repository is released under the MIT licence (`LICENSE`). Mario Kart:
Double Dash!! and its characters are Nintendo's; this project is not affiliated with Nintendo.
