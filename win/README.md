# MKDD-NX for Windows x64

The same recompiled game, runtime, menu and renderer logic as the Switch build, on Win32 and OpenGL 4.5.

## Layout

Put `MKDD-NX.exe` next to your own GM4E01 disc image (`.iso`, `.gcm`, `.ciso` or `.rvz`). It creates
the same folders the Switch build uses, so a Switch SD card's `switch/MKDD-NX` folder can be copied over as it is:

```
MKDD-NX.exe
Mario Kart Double Dash (USA).iso
logs/     mkdd.log, crash.log, boot.txt (previous run: *.prev.log)
config/   settings.ini, color.ini, shaders.bin, glprog.bin (program cache)
save/     slotA.bin + card_a/*.gci, one folder per mod
mods/     <mod key>/, characters/, karts/, audio/, hd/hd-textures.bin
```

`MKDD-NX.exe <disc image>` plays that image instead of the one next to it.

## Launcher

A settings window opens before the game (turn off "Show this launcher" to skip it; hold Shift while starting,
or run `MKDD-NX.exe --launcher`, to get it back; `--play` skips it):

- **Game**: disc image, widescreen, FPS counter, LAN play and the LAN peer address, log file.
- **Graphics**: window size, full screen, render resolution, texture filtering, field of view.
- **Audio**: output device, volume, latency, MKDD Plus stereo music.
- **Controller**: a live view of the last-used controller (sticks, d-pad, buttons, analog triggers), rumble,
  the driving button map (as the in-game BUTTON MAPPING): click a box and press the button, held buttons light their row.
- **Keyboard**: two keys for every controller button.
- **Mods**: the mod to play, what is installed, installing from an .xdelta / patched image, extra characters and karts, HD textures.

Game settings go to `config/settings.ini` (shared with the Switch build); window, sound and keys to `config/windows.ini`.

## Controls

Controllers go through SDL2: Xbox, PlayStation 4/5, Switch Pro, Joy-Con pairs, PowerA and other Switch
pads, and GameCube controllers on the official adapter (needs the WinUSB driver, e.g. via Zadig). Others work
with SDL's `gamecontrollerdb.txt` in `config/`. Players 1-4 are the controllers in the order they connect.
Buttons go by their label (PlayStation: Cross = A, Circle = B, Square = X, Triangle = Y); LB/RB = L/R,
LT/RT = ZL/ZR, Start = +, Back or Guide = − (the menu). GameCube controllers keep their own layout
(Z + Start opens the menu).

The keyboard is player 1 as well (defaults; change them on the launcher's Keyboard page):

| key | button | key | button |
|---|---|---|---|
| arrows / WASD | control stick | Q / E | L / R |
| X | A | Shift | ZL |
| Z | B | Space | ZR |
| C | X | Enter | + (Start) |
| V | Y | Esc / Tab | − (menu) |
| I J K L | right stick | F / R | stick presses |
| numpad 8 4 2 6 | D-pad | | |

F11 or Alt+Enter toggles borderless full screen.

## Build

```bash
win/build_win.sh /e/MKDD/genM4
```

Needs MSYS2 at `C:\msys64` with the UCRT64 gcc, zstd, SDL2 and xz (`mingw-w64-ucrt-x86_64-gcc`,
`mingw-w64-ucrt-x86_64-zstd`, `mingw-w64-ucrt-x86_64-SDL2`, `mingw-w64-ucrt-x86_64-xz`). Output: `build-win/MKDD-NX.exe`, statically linked (no DLLs).

Release zip: `win/package.sh /e/MKDD/genM4` builds, then writes `dist/MKDD-NX-Windows-<version>.zip` (the exe,
`release/README.txt`, LICENSE, the static libraries' licences from MSYS2, empty `mods/` folders). Only the exe
comes from `build-win/`, so nothing personal (config, saves, logs, mods, disc image) can get in.

The in-game menu is shared with the Switch (`switch/menu.c`): `SW_PC()` text and `item_hidden()` rows hide the
Switch-only items on PC (handheld resolution, controller pairing, Joy-Con sideways, clocks, GPU test, updater)
and add FULL SCREEN; hints show player 1's keys beside the buttons.

## Files

| file | role |
|---|---|
| `main_win.c` | window + GL context, keyboard, waveOut audio, crash.log, folders, the main loop |
| `launcher.c` | the settings window before the game |
| `pad_sdl.c`, `pad.h` | controllers through SDL2 |
| `wincfg.c`, `wincfg.h` | `config/windows.ini` |
| `mkdd.rc`, `mkdd.manifest` | the manifest (visual styles, DPI aware) |
| `render_gl.c` | `render.h` + `gfx.h` on OpenGL 4.5: a pass-for-pass port of `switch/render_dk.c`, same GLSL |
| `gl.h`, `gl_load.c`, `gl_funcs.h`, `gl_names.h` | GL function pointers |
| `nx_compat.h` | the libnx names `menu.c`, `pause_art.c`, `charpack_art.c` use |
| `stubs_win.c` | the updater's stubs (no in-game updater on PC) |
| `data_win.S` | the pause screen art (as `switch/pause_art_data.S`) |

## Not done yet

- The in-game updater: download the new zip and copy `MKDD-NX.exe` over the old one.
