MKDD-NX for Windows (x64)
=========================

Mario Kart: Double Dash!! (GameCube), statically recompiled to run natively on Windows.
This download contains no game data: you need your own copy of the game.

GETTING STARTED
1. Put your own Mario Kart: Double Dash!! (USA, GM4E01) disc image in this folder,
   next to MKDD-NX.exe: .iso, .gcm, .ciso or .rvz.
2. Run MKDD-NX.exe. The launcher opens first: settings, controllers, keys and mods.
   Press PLAY.
3. In the game, press - (Back on Xbox pads, Esc on the keyboard) for the options menu.

The first start takes longer while shaders are prepared. MKDD-NX creates config\,
logs\ and save\ next to itself; keep them when you update. To update: the launcher's
Updates page (one click: it downloads the latest release, replaces MKDD-NX.exe and
starts again; it can also check by itself at start) or the in-game menu's UPDATE.

Windows may warn about an unknown publisher (the exe is not signed): More info > Run anyway.

LAUNCHER
It opens before the game. Turn off "Show this launcher" on its Game page to skip it;
hold Shift while starting MKDD-NX.exe to get it back.

CONTROLLERS
Xbox, PlayStation 4/5, Switch Pro, Joy-Con pairs and GameCube controllers on the official
adapter (WinUSB driver, e.g. installed with Zadig). The launcher's Controller page shows
every button and stick live and maps the driving buttons. Other controllers: put SDL's
gamecontrollerdb.txt in config\.

Keyboard (player 1, change on the launcher's Keyboard page):
  arrows / WASD  steer          X  A (accelerate)    Z  B (brake)
  C  X (item)    V  Y           Q / E  L / R         Shift / Space  ZL / ZR
  Enter  Start   Esc / Tab  menu                     F11 or Alt+Enter  full screen

MODS AND EXTRAS (all optional, from their own authors' releases)
  mods\hd\hd-textures.bin   the community HD texture pack
  mods\characters\          extra characters
  mods\karts\               extra karts
  mods\audio\               MKDD Plus stereo music
  Level packs (MKDD Plus, Retro Track Grand Prix...): launcher > Mods > Install a mod...,
  then pick the mod's .xdelta patch or an already patched disc image. Your own clean disc
  image is still the one you play with.

LAN PLAY
Works between PCs and Switches running the same MKDD-NX version and the same mod:
launcher > Game > LAN play, then the game's LAN mode on the title screen.

PROBLEMS
logs\mkdd.log and logs\crash.log help with bug reports:
https://github.com/digdat0/mkdd-nx/issues

LICENCE
MKDD-NX is released under the MIT licence (LICENSE.txt). It includes third-party
libraries under their own licences (THIRD-PARTY-NOTICES.txt).
Mario Kart: Double Dash!! and its characters are Nintendo's. MKDD-NX is not affiliated
with or endorsed by Nintendo.
