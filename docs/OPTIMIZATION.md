# How MKDD-NX reached 60 fps at 4x

MKDD-NX went from about 42 fps at 1.5x in handheld to about 60 fps at 4x docked, both on stock clocks with one player. This note records the design changes behind that, what each one gained, and what turned out not to matter. [DEKO3D_MIGRATION.md](DEKO3D_MIGRATION.md) has the full renderer story.

## The result

| Stage | Settings | Frame rate | GPU per frame |
|---|---|---|---|
| OpenGL (last in v1.7.1) | Handheld, 1.5x | about 42 | about 23 ms |
| deko3d, first build | Handheld, 1.5x | about 53 | about 18 ms |
| deko3d, after GPU work (v1.8.0) | Handheld, 1.5x | **59.9** | about 15.3 ms, including idle time |
| v1.8.0 at 4x | Docked, 4x | mostly 44–52, lows about 41 | about 21 ms |
| v1.8.1 | Docked, 4x | **60.0 in 33 of 36 readings**, low 58.8 | about 15 ms |

All runs used stock clocks, HD textures and 16x anisotropic filtering. The first three rows ran at CPU 1020 MHz and GPU 307 MHz. The docked runs had the GPU at 768 MHz.

4x draws about 7 times the pixels of 1.5x, on a GPU clocked 2.5 times higher, for about the same GPU time per frame.

Other gains, measured on OpenGL against the first deko3d build:

| | OpenGL | deko3d |
|---|---|---|
| CPU spent rendering per frame | 5–9 ms | about 2.5 ms |
| Race stalls over 100 ms | 5, worst 272 ms | 1, 262 ms |
| App size | 55.3 MB | 51.7 MB (45.4 MB in v1.8.2) |

### How far to trust these numbers

- **The stages aren't one continuous ratio.** The settings change between the handheld and docked rows, and only OpenGL and the first deko3d build ran on the same course. The two 4x rows also used different courses.
- **OpenGL and deko3d were timed differently,** through timer queries and timestamps. The fps gain is the sturdier figure.
- **The fps counter counts screen updates,** which can repeat a game frame. See [Where it stands](#where-it-stands) for game frames.
- **The GPU timer includes idle time.** Real 2-player GPU work was about 11.3 ms when the timer read about 15.

## The changes

### 1. Foundations (v1.0–v1.3)

These came before any comparable measurements, so they have no gain figures.

- **Threads:** the game runs on core 1, the GX decoder on core 2 and the renderer on core 0.
- **Decoder:** per-draw setup is cached until the state changes, and a display-list vertex cache avoids decoding the same lists again.
- **Vertices and shaders:** vertex transform runs on the GPU, and data streams into per-frame rings. Specialised TEV shaders compile a few at a time, with the uber shader drawing meanwhile.
- **Textures:** CMPR textures upload as BC1, with no CPU decode to RGBA.
- **Logs:** they buffer in memory, because SD card writes can stall for a quarter of a second.
- **AUTO resolution:** it steps down when the GPU is the limit, and it's still the default. It also lets the port leave the console's clocks alone (DEBUG > CLOCKS: SYSTEM), which avoids conflicts with overclocking tools.

### 2. OpenGL to deko3d (v1.8.0)

The game already decoded GX commands into a neutral `Frame`, so only the code that replays a `Frame` had to change.

```
Game code (core 1) -> GX decoder (core 2) -> Frame -> render.h -> render_dk.c (deko3d)
```

- **Explicit memory:** a data ring and command memory for each of three frame slots, each behind a fence, let the CPU record while the GPU draws.
- **Background shader compiles:** a worker thread compiles, results go to `config/dksh.bin`, and a new shader no longer stalls the frame.
- **Mesa removed:** the driver's per-draw work went with it, which is where the CPU saving comes from.

The move was an experiment with a written exit: at least 10% less GPU time at the same settings with no visual regressions, or OpenGL stayed. The first build passed, at about 23 ms down to about 18 ms.

### 3. Screen copies (v1.8.0)

Our first guesses gained almost nothing. A per-draw profiler (`dk_profile`) then showed that **15 EFB copies took 9.35 ms of about 17 ms per frame.**

| Change | Effect |
|---|---|
| Copy shader skips filter rows of weight 0 | together, copies fell from 9.35 ms to about 5.4 ms |
| Half-size colour copies use one bilinear read instead of four | |
| Consecutive copies share one barrier | |

This took handheld 1.5x from about 53 fps to 59.9.

### 4. Showing the picture directly (v1.8.1)

This is the largest single gain at 4x. The game ends each frame by copying the whole picture to its TV buffer, the largest single read and write of the frame at high resolution.

When nothing is drawn after that copy, the port now shows the picture from the EFB itself and defers the next frame's clear until then.

| GPU TEST during a race | GPU per frame |
|---|---|
| Direct | 14.60 ms |
| Final copy back in | 16.56 ms (+1.96) |

An earlier run measured 15.17 ms against 19.77 ms (+4.6). DEBUG > GPU TEST switches one change at a time during a race, so each is compared over the same road.

### 5. CPU work (v1.8.1)

The generated code and the decoder both got cheaper. None of these has an isolated fps figure on hardware.

- **Unchecked memory access:** `gen_c.py` drops the hardware-address check except where a census found it necessary (`tools/mmio_keep.txt`).
- **Double-precision locals:** FPR locals are now C doubles. With the memory change, this shrank generated code from 51.1 MB to 44.8 MB.
- **List jobs:** a core 0 helper copies kept vertex lists, taking that work off the decoder. List bytes are hashed 1 frame in 16.

## What didn't help

| Tried | Result |
|---|---|
| Game mipmaps, trilinear filtering | No measurable GPU change. Kept for less distant shimmer. |
| Depth compression | No measurable change. Kept, as it's harmless. |
| ZCULL | About 0.2 ms. Kept. |
| Tiled cache | No measurable change. Off. |
| Lower anisotropic filtering (8x, 4x, 2x), Z24 | Within 0.3 ms at 4x, so 16x stays. |
| 2x cap on half-size copies | No measurable gain: 14.60 ms against 14.33 ms uncapped. May be removed. |
| 2-tap output when shrinking | 14.60 ms against 14.74 ms for 4-tap, too close to call. |
| Offloading draw-level vertex conversion | Only about 5k vertices a frame convert in small draws. Most are vertex-cache copies. |
| Final copy through the 2D engine | Never really tested. The game's copy uses a pixel format that `xfb_blit` refuses, so the shader kept running. Showing the picture directly made this moot. |

## What we learned

- **Profile before optimising.** The first round of changes targeted the assumed bottleneck. A single profile then found more than half the frame in copies.
- **If a figure doesn't change when the workload does, it isn't measuring the workload.** GPU time stayed near 15 ms at 137 draws and at 825, because we were timing the GPU waiting for work. Real work was about 11.3 ms.
- **Remove the work before speeding it up.** Skipping the final copy gained more at 4x than any change to how copies run.
- **Find the real limit first.** Once the GPU had headroom, 2-player dips turned out to be CPU load, which a lower resolution can't fix. Screen fps hid this, reading 60 while the game produced 53 new frames a second.
- **Make every change switchable.** An empty file in `config/` or the GPU TEST setting turns a change off without a rebuild. That's how we found the crash below: a GPU queue error about 11.8 s into 2-player Turnpike, every time. Reading the diff hadn't found it, but turning features off found it in two runs. The cause was normal draws growing the frame's index buffer while a list job could still write through the old pointer.

## Where it stands

Measured at stock clocks:

| Scenario | Result | Limited by |
|---|---|---|
| 1 player, 4x docked | Holds about 60 on screen, with about 98.5% new frames on most tracks and about 92% (roughly 55 fps) on N64 Toad's Turnpike | CPU on Turnpike |
| 2 players | About 55–57 game fps at 1.5x; 39–53 at 4x on Turnpike, at 11–12 ms of GPU | CPU |
| 4 players, Plus | 30 fps or less on AUTO, 50+ overclocked | Probably CPU |

- **4x is slower around the award scene:** 50–57 fps in the minute before it and 41–48 in the scene itself.
- **4 players:** one earlier run used about 7 ms of GPU with the decoder 94% busy and 5–32 game fps. That run predates the v1.8.2 black course fix, so it needs measuring again.
- **Next CPU candidate:** per-vertex texture matrix draws (4–18k vertices a frame) aren't vertex-cached and still run on the decoder. Adding them to the vertex cache could help.
