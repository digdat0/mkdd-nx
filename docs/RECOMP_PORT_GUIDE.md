# Porting a GameCube game to Switch by static recompilation

This guide collects what the MKDD-NX project (Mario Kart: Double Dash!! on Switch) learned, so that the next port of
one of your own discs goes faster and skips the mistakes we already made. It's written for personal ports of
games you own, played on your own console.

The other docs here hold the full detail for MKDD-NX:

- [ARCHITECTURE.md](ARCHITECTURE.md): the code layout
- [DEKO3D_MIGRATION.md](DEKO3D_MIGRATION.md) and [DEKO3D_PLAN.md](DEKO3D_PLAN.md): the renderer
- [OPTIMIZATION.md](OPTIMIZATION.md): the performance work

This guide is the reusable distillation of all of them.

---

## 0. Ground rules (set these on day one)

- **You supply the disc image.** Never put game code, game data, generated C, disc images, keys or dumps containing them in the repository or a release. The generated C counts as game code. Everything is built from the player's own disc on their own PC.
- **Commit only port code and tools.** Add the disc formats, `gen/`, `*.nro`, logs and screenshots to `.gitignore` before the first commit. Tools that embed game assets, such as preview mocks, stay local.
- **Fix the git identity per repository** with local `git config` plus pre-commit and commit-msg hooks that refuse any other identity or trailer. A global identity leaks into the first commit made in a fresh clone, including a wiki clone. GitHub keeps showing a wrong contributor for a while even after the history is rewritten.
- **Keep every tested `.elf`** (`debug/<App>-<tag>.elf`). Crash addresses only mean something against the exact build that crashed.
- **Stamp every build** (date and time in `boot.txt` and in the first log line). Half of all "it still crashes" reports turned out to be an old `.nro` on the SD card.

---

## 1. Choose the approach

| Situation | Approach | Typical time to first boot |
|---|---|---|
| A finished decomp **and** a PC or Android port exist | Adapt the PC port to Switch (SDL2 plus GL, then deko3d). See the `gc-decomp-switch-port` skill. | days |
| No decomp, but a static recompiler project or Ghidra analysis exists | **Static recompilation** (this guide) | 1–3 weeks |
| Neither | Start from Ghidra plus a recompiler, and expect months | months |

Static recompilation turns the game's PowerPC code into C ahead of time. A small runtime stands in for the
GameCube hardware: memory, threads, GX, DSP, disc and memory card. MKDD-NX started from MKDD-Recomp's data sheets
(LeftInWalmart, Rust/Bevy, PC) and rewrote the runtime in C for the Switch.

Why it works well on Switch:

- **Speed.** The generated C runs natively on the A57, with no JIT and no interpreter, and is fast enough at 1 GHz.
- **Hooks.** Every guest function is a C function, so you can replace any of them with a "native" or hook it.
- **Mods.** Mods that patch the main executable, such as MKDD Plus or Retro Track Grand Prix, can be recompiled and merged in.

---

## 2. The pipeline

```
disc image ──► sheets (CSV: p-code per instruction, functions, memory map, HLE list)
                 │        (from a recompiler project, or exported from Ghidra)
                 ▼
          gen_c.py ──► gen/part_NN.c + table.c   (one C function per guest function)
          gen_mod.py      (merges mods: only the functions whose C differs)
                 │
                 ▼
   runtime/ + switch/ (or pc/) ──► App.nro   (or a headless PC exe for testing)
```

Put the whole thing behind one script (`build_from_iso.py`) so that a user goes from disc to `.nro` in one step,
and you can regenerate without remembering flags.

---

## 3. Code generator design (mapping guest functions to C)

### Core model

- **One C function per guest function**, all over one CPU context struct (`Ctx`: GPRs, FPRs including the paired-single halves, CR, XER, LR, CTR, FPSCR).
- **Guest registers live in C locals** for the whole function. They are written back to `Ctx` only where something else can read them: calls, returns and slow memory paths. A per-function dataflow pass tracks which locals may be dirty.
- **ABI liveness** (PowerPC EABI): at calls, skip storing volatile registers the callee never reads. Returns hand back only r3, r4 and f1. This was a large code-size and speed win.
- **FPR locals as C `double`** (ps1 too). Gekko does single-precision maths in double registers, so doubles match the hardware, and the code was smaller and faster (51.1 MB of C fell to 44.8 MB together with the next item).
- **Memory: inline fast path for MEM1** (big-endian loads and stores into one host buffer). Anything else (hardware registers, the gather pipe, locked cache) calls a trampoline.
  - **The MMIO census.** Instrument every access once (`MKDD_MMIO_CENSUS`), play the game, and drop the hardware-address check everywhere except where the census found hardware access (`mmio_keep.txt`). Keep the check for SDK ranges, `lis` of a hardware constant, and mod code you didn't census. Deleting the keep file restores every check, as a safety valve.
- **Paired singles** compile to inline C for the common forms, with a runtime helper for the rest.
- **Indirect calls** (`bctrl`, vtables) go through a dispatch map from guest address to C function. An unmapped target logs `bad call <addr>` with the LR. "0 unresolved calls" is the first health check on any new code path.

### Natives and hooks: how you change the game

- **Natives:** a list (`NATIVE_LIST` in `native.h`) of guest functions replaced by hand-written C: matrix maths, animation, collision, HUD, and anything you want to change. The generator emits a direct call to `n_<name>` instead of the recompiled body.
- **HLE hooks:** SDK functions (OS, DVD, audio, GX, CARD, ETH) check `core_hooked()` on entry and are routed to `hle.c`. Emulating at the SDK API level is far easier than emulating the hardware.
- **The native verifier:** with `--native-verify` it samples native calls, also runs the recompiled original from the same state, and compares the results and the memory written. Write a native, verify it on the PC harness, and only then trust it.
- **Getting game structure:** the game's managers usually hang off the small-data pointer (`r13 - offset`). Log the fields read-only first (a "state log" native), confirm the meaning from logs, then change behaviour. Decomp headers can mislead on offsets. Trust the bytes (for example, the BOL start count was at header +0x3b, not where the struct said).

### Rules learned the hard way

1. **Native IDs come from list order.** Adding or removing an entry in `NATIVE_LIST` shifts every ID after it. Objects built before the change jump to the wrong native: a black screen, or the wrong constructor called. Make every runtime object depend on `native.h` in CMake, and **regenerate all gen dirs** (base plus every mod) after any change to the native list or the HLE bind table. The generator reads both at generation time.
2. **Verify that the regen actually happened.** Check the timestamps of the `gen/part_*.c` files and grep for the new `n_<name>` calls. A merge step skipped by a mis-copied script line produced a full build with none of the new hooks.
3. **Mods must be generated by the same `gen_c.py`** as the base, then merged. Mods that rewrite immediates at runtime (for example a cup-page switcher patching `li` instructions) need those addresses compiled as dynamic immediates (`--dyn-imm`), or the feature silently does nothing. Mods that break register assumptions need `--keep-regs`.
4. **Write generator toggles as command-line flags, not environment variables.** In Git Bash on Windows, the devkitPro msys2 `python` drops inline environment variables (`VAR=1 python x.py`), and the toggle silently does nothing. That cost an hour of bogus bisecting.

### Generator self-checks (run on the PC harness)

| Flag | What it proves |
|---|---|
| `--abi-check` | Registers whose store was skipped are filled with a poison value, and any read of one is reported |
| `--check-lazy` | Callee-saved registers come back unchanged |
| `--check-mod` | Registers not reloaded after a call are unchanged (for mods) |
| `--native-verify` | Natives match the original code |

---

## 4. Runtime design

| Part | Design that worked |
|---|---|
| Memory | MEM1 (24 MB) and ARAM as host buffers. Big-endian accessors. `IS_RAM(0)` is true, so use an explicit "pointer valid" check for nullable guest pointers. |
| Threads (`sched.c`) | **Each guest thread is a host thread, and only one runs at a time** (a baton), which keeps the guest's cooperative scheduling exact. Interrupts, alarms and time live here. One-shot alarms must clear `alarm->handler` when they fire, because SDK code polls it. |
| GX | A decoder for the command stream (FIFO and display lists) on **its own thread and core**. It outputs a backend-neutral `Frame`: batches, TEV/pipeline state, EFB copies, one vertex array, one index array and XF data. **This seam is the most valuable design decision.** It let us swap OpenGL for deko3d without touching the game side. |
| Vertex work | Cache per-draw setup until the state changes. Cache decoded display lists by hash (hash the bytes only 1 frame in 16). Do the XF transform on the GPU (a vertex shader), with a CPU path kept for verification. |
| Audio | HLE the DSP microcode (JAudio) at the command-list level, not by DSP emulation. |
| Disc | `.iso`/`.gcm`, `.ciso` and `.rvz` readers with a read-ahead thread. Scan the app folder for any of those extensions and check the game ID. |
| Memory card | Emulate at the CARD API level. Saves are standard `.gci` files, importable and exportable with Dolphin's. |
| Movies | A THP decoder in the runtime. |
| Logging | Each distinct line is logged once, with repeats counted. Logs **buffer in memory** and flush at loads, slow frames, exit and crash. Plus a watchdog trace for hangs. |
| Platform layer (`plat.c`) | Threads, locks, time and files for both PC and Switch, so the same runtime builds headless on Windows. |

---

## 5. Test on PC first: the headless harness

Build the same runtime and generated C as a headless Windows exe (MSYS2 UCRT64 gcc): no window, no audio, and
a **scripted controller**: `app_pc <disc> [seconds] [pad script]`. This was the single biggest time-saver.

- **Pad scripts:** per-frame button and stick bitmasks, enough to boot, navigate menus and finish a race unattended.
- **Every new feature runs here first.** The normal bar is reaching a race with 0 unresolved calls and no verifier mismatch.
- **Dumps:**
  - `DUMP_FRAME=<s>`: one frame's batches, pipeline state, textures and a few transformed vertices, as text.
  - `DUMP_TEV=<dir>`: every generated shader, for offline compiles.
  - `PROF=<s>`: a sampling profile.
- **Switches:** `TIMESCALE=n` runs guest time fast for benchmarks. `SLOW_NS` spins per matrix multiply, to load the game thread like the Switch CPU does. Stress injectors such as a lost scene load prove the watchdog works.
- **The emulator (Ryujinx) and the PC harness prove correctness, never speed.** The emulator also doesn't emulate ZCULL, compression or real GPU cost, and runs multiplayer far slower than hardware.
- **Screenshot harness:** for visual tuning, such as a first-person camera per kart, a script that boots a portable emulator, poses a scene and saves screenshots beats manual testing. Keep it on a portable copy, not a system install.

---

## 6. Switch front end

### Layout and threads

- **Threads and cores:**
  - Core 0: the renderer.
  - Core 1: the game (all guest threads, one at a time).
  - Core 2: the GX decoder, the timer and the profiler.
  - Audio runs above the renderer's priority.
  - Helper threads sleep on a condition variable. A spinning helper starved audio in another port.
- **SD layout:** `sdmc:/switch/<App>/<App>.nro` with the disc image next to it, plus `config/`, `saves/`, `logs/`, `mods/` and a shader cache. The build only replaces the `.nro`.
- **Launch with title takeover** (hold R on a game) for full memory. Check the applet type at boot and show a message if the app isn't running in title takeover.

### Logs players can send you

- `boot.txt`: proves `main()` ran, with the build stamp.
- `logs/<app>.log`: buffered, with the state of every feature switch on the first line.
- `crash.log` from your own exception handler: registers and backtrace as image offsets. Get the load address with `svcQueryMemory` on one of your own functions.
- **Hitch lines:** every dropped frame logs where the time went (texture uploads, shader compiles, game time, decoder time, disc reads, GPU waits). Bug reports become actionable without a profiler session.
- **Gate verbose game logs** behind a menu option (DEBUG > LOGGING). `printf` on Switch goes nowhere unless you route it to the log.
- Ask players to put each log set in a dated folder. Read the newest first, and check its build stamp.

### Settings, menus and the updater

- **`settings.ini`** as key=value with `atoi`. When an option's meaning changes, **rename the key**, so old values are ignored instead of misread. Keep old values readable where they map cleanly (for example `>0` means on).
- **An overlay menu** (the − button) drawn with the game's own font from the disc fits the game's look. Keep option pages data-driven (page enum, rows, parent, title, labels, hints), so a new page is a few table entries.
- **An in-game updater** from GitHub releases, which checks size, SHA-256 and the NRO header before replacing anything.
- **Clocks are opt-in.** Default to the system's clocks with no apm profile and no CPU boost. The port's clock calls fought Horizon-OC: uneven pacing, and a crash when overclocking was toggled during boot. Make dynamic (AUTO) resolution the performance answer.

---

## 7. Renderer: deko3d

MKDD-NX shipped on OpenGL ES (Mesa nouveau) first, then moved to **deko3d** (devkitPro's low-level API for the
Switch GPU):

| | OpenGL | deko3d |
|---|---|---|
| Handheld fps at 1.5x | about 42 | 59.9 |
| CPU spent rendering per frame | 5–9 ms | about 2.5 ms |
| App size | 55 MB | 45 MB (Mesa gone) |

**For a new project, start on deko3d directly** if you already have a `Frame`-style seam. Use GL only if you're
adapting an existing GL PC port, and then plan the move.

### How to structure the move (or the start)

1. **The neutral `Frame`** from the decoder, so that only the replay code is backend-specific.
2. **Split before porting.** Put every graphics call outside the renderer (menus, font, overlays, pause art) behind a small 2D API (`gfx.h`: textured quad, solid rect, scissor, texture create and update) while the old backend still ships. The new backend then implements only that API plus the frame replay.
3. **Treat the old renderer as the specification.** Follow its replay step by step, which keeps "what does this GX state mean?" separate from "how does deko3d do it?". MKDD's is about 2,100 lines of C.
4. **Two builds selected at build time during the move**, not a runtime switch. Remove the old backend only after hardware runs prove the new one. Tag the last release that has it.
5. **Write the go/no-go figure before writing the code.** Ours was "at least 10% less GPU time at the same settings, no visual regressions, or GL stays". Another port's first deko3d build was slower than its old renderer.
6. **Bring-up in an emulator** for "does the picture look right, does it hang". Real speed only on hardware.

### Memory model

| Block | Holds | Lifetime |
|---|---|---|
| Data ring, one per frame slot (3 slots) | vertices, indices, transforms, uniforms, texture staging | reused after that slot's fence; grows on demand |
| Command memory, one per slot | recorded commands | same |
| Image heap, in 32 MB chunks | textures, render targets | freed images wait 4 frames before reuse |
| Code | shaders | whole session |

**Nothing the GPU might still read is ever reused before its fence.** deko3d won't protect you.

### Shaders

- Generate GLSL. TEV-specialised fragment shaders come from a generator (`tev_gen.c`). The XF stage is a vertex shader.
- **Compile on the console with a vendored uam** (devkitPro's shader compiler as a library). SwitchWakerHD's patches add reentrancy, output to memory, no `exit()`, deterministic scheduling and **no dual issue**. Dual issue caused flickering blocks on another port.
- **Compile on a worker thread.** Draw with an "uber" shader until the specialised one is ready, and cache the compiled DKSH to `config/dksh.bin`, keyed by a hash of the GLSL plus the uam patch level. New shaders never stall a frame, and boot loads the cache in milliseconds.
- **uam rules:**
  - Explicit `binding=` everywhere.
  - No loose uniforms (use a small UBO or push constants).
  - No dynamic component index into UBO vectors.
  - No non-constant integer `/` or `%`.
  - Treat every warning as an error.
- **Validate offline** with `uam.exe -s vert|frag` on dumped shaders.

### deko3d pitfalls (most from the NFS MW and SA2 Switch ports' guides; credit them)

1. Bring-up with `libdeko3dd` (the validating build). **Check `dkQueueIsInErrorState`** before acquire, submit and present, and log it. Never assume a dead queue can be recovered, because fences from it can hang.
2. Leave **256 B of slack** at the end of each command memory piece. deko3d 0.5.0 can write past it.
3. **Bind one vertex buffer per call.**
4. **BC textures 24–32 px tall need `DkImageFlags_CustomTileSize`**, or mip 1 corrupts level 0.
5. **Clears respect the scissor.** The state cache must forget the scissor after a clear.
6. **Depth textures read as (D, D, D, 1).** Z-copy paths take `.r`.
7. **Invalidate ZCULL** after any copy or blit into a depth image.
8. **GPU timestamps run 1/1.6276 slow.** Multiply by 1.6276.
9. **You own the barriers.** Put one barrier before a copy target is sampled, only when it was written since the last barrier. Consecutive copies and uploads share one barrier, which is where much of the speed comes from.
10. **Buffers can grow during a frame.** A helper thread writing into a frame's index buffer while the main path grows (reallocates) it wrote through a stale pointer. The result was a GPU queue error 11.8 s into one specific 2-player race, every time. **Join helper jobs before growing a shared buffer.**
11. **Docked/handheld change:** destroy the swapchain, call `nwindowSetDimensions`, then recreate it (it's refused while buffers are registered).
12. **Loading loops:** a helper that presented and then presented again with nothing drawn made the loading screen flicker against black. Harmless on GL, visible once you own the swapchain.
13. **Exit must free everything:** every GPU memory block, the queue and the device. A leaked heap stops the loader reclaiming memory and shows "unexpected error" with no crash report on exit.
14. **Keep SDL for audio and input at first,** with an EGL stub (`egl_stub.c`), because SDL's video driver references EGL and Mesa's EGL clashes with uam's embedded Mesa compiler. Moving to libnx `audout` and HID later removes both.
15. **Orientation:** deko3d clip space has y up and no negative viewports. Prove your convention with a quadrant test pattern on day one.
16. Use `DkDeviceFlags_DepthZeroToOne` and drop the `2z - w` remap.

### GameCube-specific rendering notes

- **EFB copies** (`GXCopyTex`, screen copies used for effects, shadows and the final TV image) are usually the biggest GPU cost. Render the EFB as a GPU image. A copy is a shader pass or a blit into a texture kept per destination address. Never read pixels back to the CPU.
- **Upload CMPR as BC1** (no CPU decode), and compact I and IA formats to R8 and RG8 with view swizzles.
- **Per-vertex lighting** (GX lights per vertex) is faithful and cheaper than per-pixel.
- **Widescreen:** widen the projection (`aspect_scale`) instead of stretching. The decoder flags 2D draws, and the vertex shader squeezes them back to 4:3, with HUD elements pushed to the screen edges. Full-screen draws and fades stay full width. Per-player viewports in split screen need the same squeeze.
- **HD texture packs:** look up replacements by a hash of the original texture, converted offline to BC7.

---

## 8. Performance: method first

What took MKDD-NX from about 42 fps (1.5x handheld) to about 60 fps (4x docked), on stock clocks:

1. **Profile on the real console before optimising.** Our first round targeted the assumed bottleneck and gained nothing. A per-draw GPU profiler (timestamps on one frame in 300, behind `config/dk_profile`) then showed **15 EFB copies taking 9.35 ms of about 17 ms**.
2. **Remove the work before speeding it up.** Presenting the picture straight from the EFB (deferring the next frame's clear), instead of doing the game's final full-screen copy, was the largest single gain at 4x: about 2–4.6 ms.
3. **Cheapen what remains:**
   - Skip filter rows of weight 0 in the copy shader.
   - Use one bilinear read for half-size copies.
   - Share barriers between copies.
4. **"If a figure doesn't change when the workload does, it isn't measuring the workload."** The GPU time read about 15 ms at 137 draws and at 825, because the timer included the GPU waiting for submissions (spikes sat exactly on the 128-draw submit boundary). Profiling the frame as one submission gave the real figure, 11.3 ms.
5. **Find the real limit.** Once the GPU had headroom, 2-player dips were the CPU: the game thread about 99% busy with a flat profile. A lower resolution can't fix that. **Count game frames, not screen frames.** The screen read 60 while the game made 53 new frames a second.
6. **Make every change switchable without a rebuild.** An empty file in `config/` (`dk_nozcull`, `novjobs`, ...) turns a change off. A DEBUG > GPU TEST option alternates one change each second during a race and logs the averages, so each change is compared over the same road. **That's how the crash in item 10 above was found in two runs**, after reading the diff had failed.
7. **Measure one change at a time** and keep only what pays.

### CPU-side wins

- The decoder on its own core, setup caching, the display-list vertex cache, and the XF transform on the GPU.
- Generator work: register locals, ABI liveness, unchecked MEM1, double FPRs.
- **List jobs:** a core 0 helper copies cached vertex lists off the decoder's core.
- **Natives** for the hottest engine functions (matrix maths, animation, collision).
- **Stream all per-frame data into rings.** No per-draw allocations.
- Buffer logs. **SD card writes can stall for about 260 ms.** Keep periodic prints on one thread: stdio across threads spliced garbage into logs.

### What didn't help (save yourself the time)

| Tried | Result |
|---|---|
| Mipmaps and trilinear filtering, depth compression | No measurable GPU change. Kept for image quality. |
| ZCULL | About 0.2 ms. |
| Tiled cache | No change. Off. |
| Lower anisotropic filtering (8x/4x/2x), a 24-bit depth buffer | Within 0.3 ms at 4x, so 16x stays. |
| Final copy through the 2D engine | Never engaged: the game's pixel format was refused by the blit path. Check logs prove a fast path actually runs before reporting its result. |
| Offloading vertex conversion | Only about 5k vertices a frame took that path. Count before offloading. |

### Tunables to expose

- **AUTO resolution** (step down when the GPU is the limit), plus fixed 1x–4x.
- Anisotropic filtering, HD textures on/off, 4:3 or 16:9.
- Clocks: SYSTEM (the default) or GAME.

---

## 9. Horizon (system) pitfalls

| Symptom | Cause and fix |
|---|---|
| Exit to hbmenu fails with 2001-0132 (LimitReached) or "closed because an error occurred" | **Paused threads still count against the thread limit.** Make waiting threads exit themselves at shutdown (condvar waits in 100 ms slices that check an exit flag). Log "exit: N threads ended". |
| Same message on exit, no crash report | GPU memory, queue or device not freed (see deko3d item 13). |
| Exiting straight to HOME with `svcExitProcess` | **Dead end** under hbl title takeover: it errors every time. Don't retry it. |
| Black screen and 2345-0018 on the next launch after a crash | The crash took down `vi`. **Reboot after every crash** before testing again. |
| HOME during a loading screen reboots the console | Loading loops must pump applet events (`appletMainLoop` / `SDL_PumpEvents`). |
| A crash report that "doesn't match" | Compare its Module ID with `readelf -n` of the saved elfs. It may be an old report. |
| Regular hitches with nothing new happening | SD writes from logging. Buffer them. |
| The restart path crashes | Tear down sockets and network (`eth_exit`, `plat_net_exit`) and every thread before restarting. |

Crash triage: symbolize with the elf whose build ID matches (`aarch64-none-elf-addr2line -f -C -i -p`). A data
abort at a small address is a null pointer. No `boot.txt` means the app died before `main()`.

---

## 10. Windows toolchain pitfalls

- devkitPro needs **its own msys2 cmake**. Run it through `env.exe` with `PATH`, `TMPDIR`, `TMP` and `TEMP` set to Windows paths, or gcc tries to write temp files to `C:\Windows`. Wrap this in a `build.sh` so nobody types it again.
- Link thousands of objects with **response files** (`CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS ON`).
- **Header dependencies weren't tracked** by the generated Makefiles. Editing only a header (shaders, settings struct, native list) left objects stale, and the hardware showed the old shapes. After editing a header, touch every `.c` that includes it and confirm the `.o` timestamp, or add explicit `OBJECT_DEPENDS`.
- **Git Bash heredocs mangle backslashes**, even quoted ones (`"\n"` becomes a real newline). Write edit scripts to a file and run them with `py -3 script.py`.
- `python` in Git Bash may be devkitPro's msys2 Python. Use `py -3` for tools, and argv flags, not environment variables.
- Compile the hot runtime (decoder, renderer) at `-O3`, and the generated code at `-O2 -fwrapv -fno-strict-aliasing`. Frame pointers in generated code are opt-in, for profiling builds only.
- **The PC harness:** MSYS2 UCRT64 gcc, the same runtime sources, plus a `pc/main_pc.c` front end and a Windows sampling profiler.

---

## 11. Gameplay features on top of a recompiled game

Patterns that worked for MKDD-NX features (CPU battle opponents, drafting, extra characters and karts, LAN):

- **Read first, write later.** Start every feature with a read-only state-log native. Confirm the meaning of fields from real logs, then change behaviour.
- **Hook the decision point, not the symptom.** Battle CPUs, for example, override the game's own branch-choice function (`findNextPointNo` → `FUN_80226f1c`) and item control (`doItemCtrl`), instead of steering karts directly.
- **Build tools for data the game needs.** The route networks, for example, came from the game's own collision files on the disc (triangle types: road, wall, out of bounds, pipe), through an offline tool with a tkinter editor, and were stored as the port's own data files.
- **Custom content converters** (Wii characters and karts): match the target's rig, texture-slot order and material names, because animations bind by name and index. Bake texture transforms the target can't express into the UVs. Refuse what can't work (bikes) with a clear message.
- **Lockstep LAN:** the game's LAN exchanges pad input in lockstep, so **every gameplay-affecting option must match on both consoles**. Force options to stock during LAN play, or the race desyncs about 100 s in.
  - HLE the Ethernet driver at the SDK level and tunnel frames over UDP.
  - Append the 4-byte FCS the stack expects on received frames.
  - Learn peers, de-duplicate by sequence number, size the receive queue for emulator stalls, and add a fingerprint handshake: build version, disc hash and mod set.
- **Regions:** the address maps differ per region. A name-matched symbol map from the decomp (USA → EU) moves the roughly 300 hard-coded runtime addresses into a per-region table. Plan for this early by keeping guest addresses in one table, not scattered literals.
- **Options UI:** each feature gets an ON/OFF row. A opens a subpage of tunables, with named levels (CLOSE / REGULAR / FAR) instead of raw numbers, and the default is the tuned value. Disable gameplay changes in LAN play.

---

## 12. Bring-up checklist for a new game

Each step has an exit test. Don't start the next until it passes.

1. **Inventory:** disc ID, SDK version, existing decomp, recompiler sheets or Ghidra project, and existing PC ports. Check licences before reusing code: MPL-2.0 is file-level copyleft, so rewrite ideas rather than copying files into an MIT project.
2. **Sheets and generator:** produce the C for the base executable. *Exit:* it compiles and links on the PC harness.
3. **Runtime boot on PC:** memory, scheduler, HLE OS and DVD, disc reader. *Exit:* the game reaches its first GX frame headless, with 0 bad calls.
4. **The GX decoder into `Frame`,** plus a debug frame dump. *Exit:* a dumped title-screen frame has sane batches, textures and transformed vertices.
5. **Input via pad scripts, memory card, audio HLE.** *Exit:* a scripted run from boot to gameplay and back, saving and reloading.
6. **The Switch shell:** SD layout, logs, crash handler, disc lookup, applet checks, and a deko3d device with a test pattern. *Exit:* the test pattern in correct orientation, dock and undock, clean exit to hbmenu.
7. **The deko3d frame replay:** batches, TEV shaders (uber shader plus async compile), textures, EFB copies, present. *Exit:* title, menus and gameplay match emulator or PC screenshots.
8. **Hardware pass:** 30 minutes of mixed play, HOME and sleep, dock changes, exit and restart, movies, multiplayer. Read the logs every time.
9. **Profile on hardware, then optimise** (section 8), one switchable change at a time.
10. **Enhancements:** widescreen, resolution scaling, HD textures, mods, the menu, the updater.
11. **Docs and release:** README, wiki pages, third-party notices (deko3d is zlib; uam is MIT and zlib), and a release script that never packs game data.

---

## 13. Working habits that paid off

- **One question at a time on hardware.** Give each test build a name (`debug/App-<feature>N.elf`) and say exactly what to test and which log to send back.
- **A "known issues, not regressions" list.** Before blaming new work, test the last release. A 3–4-player black-course bug, for example, predated the performance work that seemed to cause it.
- **Write the measurement caveats down** next to the numbers: different courses, different timers, screen fps against game fps.
- **Defensive code:** check every guest pointer, clamp every table index from settings or game state (`x < 3 ? x : default`), and log once instead of crashing.
- **Ask before committing or pushing;** keep features uncommitted until they're polished and tested on hardware.
- **Give each feature a memory or notes file** (addresses, offsets, what's confirmed on hardware and what isn't). Months later, the offsets are the expensive part to rediscover.

---

## Credits and references

- **MKDD-Recomp** (LeftInWalmart): the recompilation sheets and runtime design MKDD-NX ported.
- **deko3d** and **uam** (fincs, devkitPro). SwitchWakerHD's uam patches and its deko3d renderer were used as a reference.
- **deko3d guides** by StevensND (nfsmw-nx) and ChanseyIsTheBest (sa2-nx). Their pitfall lists saved days.
- **Decomp and port base for the PC-port route:** ACreTeam/ac-decomp, flyngmt/ACGC-PC-Port, zwaetschge/ACGC-Android-Port (see the `gc-decomp-switch-port` skill).
