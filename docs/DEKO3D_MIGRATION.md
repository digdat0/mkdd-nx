# Moving MKDD-NX from OpenGL to deko3d

MKDD-NX is a native Nintendo Switch port of Mario Kart: Double Dash!!, built by statically recompiling the GameCube code. Until October 2026 it drew through OpenGL ES on Mesa's nouveau driver. It now draws only through [deko3d](https://github.com/devkitPro/deko3d), devkitPro's low-level API for the Switch GPU.

This note records what changed, how much it helped, and what we learned, for anyone doing the same on another Switch port. The working plan and the list of switches are in [DEKO3D_PLAN.md](DEKO3D_PLAN.md).

## Results

All figures come from a retail-clocked console: handheld, CPU 1020 MHz, GPU 307 MHz, 1.5x internal resolution, HD textures, 16x anisotropic filtering.

| One player | OpenGL | deko3d, first build | deko3d, after GPU work |
|---|---|---|---|
| Screen fps in a race | about 42 | about 53 | 59.9 |
| GPU timer per frame | about 23 ms | about 18 ms | about 15.3 ms, including idle time |
| CPU spent rendering per frame | 5–9 ms | about 2.5 ms | not re-measured |
| Race stalls over 100 ms | 5, worst 272 ms | 1, 262 ms | not re-measured |

| Build | OpenGL | deko3d |
|---|---|---|
| App size | 55.3 MB | 51.7 MB |
| Mesa linked | yes | no |

Over two later cups on deko3d (8 races, with intros and awards), the game itself produced an average of 59.0 new frames per second, excluding loading screens. The slowest 5% of 5-second windows fell to about 55.

In a 2-player race, deko3d's real GPU work is about 11.3 ms per frame (range 9.3–13.7). Game frames averaged about 55 in top/bottom and 57.5 in side by side. The CPU is the limit there, so more GPU work would not raise those figures.

### How far to trust these numbers

- **OpenGL and first-build deko3d** were compared on the same course at the same settings. The later runs were on other courses.
- **There is no 2-player OpenGL baseline.** We can't state a 2-player gain.
- **The "fps" counter counts screen updates**, which can repeat a game frame. The later game-frame figures use the "game frames rendered" counter, which was not extracted for the OpenGL run.
- **The GPU timer is not a pure measure of work.** See "Measure what you think you're measuring" below.
- **The two APIs were timed differently:** GL through timer queries, deko3d through timestamps. The 21% first-build gap is good evidence but not an exact ratio. The fps gain is the sturdier figure.
- **Boot time was only measured in Ryujinx.** First game frame took 7.7 s instead of 12.1 s, with the shader cache loaded. That shows the direction only.

## Why move at all

Other Switch ports had reported large gains from deko3d, mostly by cutting CPU cost per draw: NFS Most Wanted lost slow Vulkan pipeline creation, while SA2 and Wind Waker HD issue over a thousand draws a frame. MKDD's replay was only 100–400 batches a frame, costing 3–5 ms of CPU, so we expected less. Our forecast was 2–3 ms of CPU and 10–25% of GPU time, with no guarantee: NFS MW's first deko3d build was slower than its old renderer.

So we treated the move as an experiment with an exit. The rule, written before the renderer existed, was at least 10% less GPU time at the same settings with no visual regressions, or GL stays. The first build passed.

## Architecture

The game side did not need to know. `runtime/gx.c` already decoded GameCube graphics commands on its own thread into a neutral `Frame`: batches, TEV state, EFB copies, vertex and index arrays, and transform data. Only the part that replays a `Frame` changed.

```
Game code (core 1) -> GX decoder (core 2) -> Frame -> render.h -> render_dk.c (deko3d)
Menus, font, pause art, character art -> gfx.h (window, textures, 2D overlay) -> render_dk.c
```

Choices that mattered:

- **Two apps during the move, chosen at build time.** A build option selected the backend, and the two apps shared settings and mods. A runtime switch would have linked Mesa's compiler twice over and added risk for no gain. Once deko3d proved itself on hardware, the GL backend was removed.
- **Split first, port second.** All GL calls outside the renderer moved behind `gfx.h` while the GL build still shipped, so deko3d only had to implement one small 2D layer.
- **The GL renderer as the specification.** `render_dk.c` followed the GL renderer's frame replay step by step. That kept the question "what does this GX state mean?" separate from "how does deko3d do it?". About 2,100 lines of C.
- **Same GLSL for both during the move.** One source with `#ifdef DK` for explicit bindings and uniform blocks. Only the deko3d side remains.

### Memory

A few large blocks, sub-allocated by hand:

| Block | Holds | Lifetime |
|---|---|---|
| Data ring, one per frame slot | vertices, indices, transforms, uniforms, texture staging | reused after that slot's fence; grows on demand |
| Command memory, one per frame slot | recorded GPU commands | same |
| Image heap, in 32 MB chunks | textures, render targets | freed images wait 4 frames before reuse |
| Code | compiled shaders | whole session |

Three frame slots, each behind a fence, let the CPU record while the GPU draws. Nothing the GPU might still be reading is reused, because deko3d, unlike a GL driver, won't protect you from that.

### Shaders

The app compiles GLSL on the console with a vendored copy of [uam](https://github.com/devkitPro/uam), carrying SwitchWakerHD's patches (`switch/third_party/uam/PATCHES.md`). Results go to `config/dksh.bin`. A worker thread does the compiling, and a draw uses the general "uber" shader until its specialised version is ready, so a new shader no longer stalls the frame.

The plan was to compile the fixed shaders at build time. Compiling everything through one path with one cache proved simpler, so we did that.

### Keeping SDL for now

SDL remains for audio and input. Its video driver references EGL, and Mesa's EGL would clash with uam's embedded Mesa compiler, so `egl_stub.c` provides stand-ins that nothing calls. Moving to libnx's audio and HID APIs would remove both.

## Method

1. **Baseline.** Log GPU, CPU and fps on GL at fixed settings.
2. **Separate the backend** on the GL build, and check nothing changed on hardware.
3. **Bring up deko3d in Ryujinx.** It's good for "does the picture look right, does it hang". It says nothing about real GPU cost, and it doesn't emulate ZCULL or compression.
4. **Go/no-go on hardware.**
5. **One GPU change at a time, each behind a switch.** An empty file in `config/` (such as `dk_nozcull`) turns a change off without a rebuild, and the first log line records the state. Testers can run comparisons with no tooling.
6. **Add a profiler before guessing further.** `dk_profile` timestamps every draw, copy and clear on one frame in 300, on the real console.

## What actually moved GPU time

Our first round of optimisation targeted what we assumed was the bottleneck, and gained almost nothing:

| Change | GPU per frame | Kept? |
|---|---|---|
| Game mipmaps, trilinear filtering | no measurable change | yes: less distant shimmer |
| Depth compression | no measurable change | yes: harmless |
| ZCULL | about 0.2 ms | yes: no artefacts seen |
| Tiled cache | no measurable change | no: off |

The profiler then showed the real cost: **15 EFB copies per frame took 9.35 ms of about 17.** These are the game's screen copies: 11 tiny depth copies (probably sun-flare checks), 3 half-size scene copies and the final TV image.

| Change | Effect |
|---|---|
| Copy shader skips filter rows of weight 0 (MKDD weights one of three) | together, copies fell from 9.35 ms to about 5.4 ms |
| Half-size colour copies use one bilinear read instead of four | |
| Consecutive copies share one barrier | |
| Final copy through the 2D engine instead of the shader | **no measured gain:** still 2.65 ms |

The unexplained part is that final copy. A full-screen draw of the same size costs about 0.33 ms, while the copy costs 2.65 ms by either route. We don't know why. It may be the cost of reading the compressed EFB, but that's untested, and logs can't confirm races take the blit path at all. With GPU headroom available we stopped there.

## Key learnings

### Set the exit test first

A written go/no-go figure turned "this feels faster" into a decision, and made it safe to start something that might have failed.

### Measure what you think you're measuring

For a while we reported about 15 ms of GPU per frame, and treated a few draws costing 1.5 ms or more as expensive. Both were wrong. The total stayed near 15 ms at 137 draws and at 825, in one player and in two. Of 221 profiled draws over 1.5 ms, 219 sat exactly on a multiple of 128, the point where the renderer submits its next chunk of commands. We were timing the GPU waiting for work.

Sending the profiled frame as one submission gave the real figure of about 11.3 ms, and removed the motivation for the copy and effect work we had planned next. **If a figure doesn't change when the workload does, it isn't measuring the workload.**

### Find the real limit before tuning

Once the GPU had headroom, 2-player dips turned out to be CPU load. The game thread ran at about 99% busy with no function above about 5%, and the GX decoder averaged 67% busy with an equally flat profile. Lower resolution won't help, and neither will more GPU work. Screen fps had hidden this, reading 60 while the game produced 53 new frames a second.

### The driver was doing work you now own

Things GL handled silently and we now do ourselves: barriers between drawing and sampling, deferred freeing, resetting state after clears, invalidating ZCULL. Most bring-up risk is here, and it's also where the speed comes from, because you can share one barrier across a run of copies or uploads.

Known traps, most learned from other ports' guides:

- Keep slack at the end of each command memory piece, because deko3d 0.5.0 can write a few words past it.
- Bind one vertex buffer per call.
- BC textures 24–32 pixels tall need a custom tile size, or mip levels can corrupt the base level.
- Clears respect the scissor, so a state cache must forget the scissor after one.
- Depth textures read as (D, D, D, 1).
- uam's dual-issue scheduling produced flickering blocks on another port, so the vendored copy turns it off.

### Loading loops change when you own the swapchain

Our startup screen flickered 8–10 times after shaders loaded. The loop drew through a helper that presented, then presented again with nothing drawn, so a loading screen and a black frame alternated. That pattern was harmless on GL and visible on deko3d.

### Effects can be wrong without being slow

The 2-player seam blur, carried over from GL, left a pale band beside the centre line because it blended the two players' pictures. A plain 1-pixel line (2 pixels from 2x) replaces it in the deko3d build.

### Keep the fallback

Keeping GL buildable beside deko3d made every comparison possible and kept a way back until hardware runs had covered enough. We removed it after that. v1.7.1 in git is the last release with it.

## 4x docked at stock clocks (v1.8.1)

Measured on the console: docked, 4x (2432x1792), stock clocks, HD textures, 16x anisotropic filtering, Plus. DEBUG > GPU TEST switches one change at a time during a race, so each is compared over the same road.

| GPU test, two races | GPU time per frame |
|---|---|
| Normal | 14.60 ms |
| Final copy back in | 16.56 ms |
| Half-size copies uncapped | 14.33 ms |
| 4-tap output | 14.74 ms |

- **Skipping the final copy is the real gain.** Using the 2D engine for that copy gained nothing (see above), so the copy is gone: when nothing is drawn after it, the picture is shown from the EFB and the next frame's clear waits until then.
- **The 2x cap on half-size copies has shown no measurable gain** in two runs. It stays until it is tested on its own, and may go.
- **Races hold about 60 on screen, but game frames are the honest number:** about 98.5% new frames on most tracks, about 92% (roughly 55 fps) on N64 Toad's Turnpike. GPU time is under budget there, so the CPU is the limit.
- **Multiplayer is CPU limited.** 2-player Turnpike makes about 39-53 game frames a second with 11-12 ms of GPU time.

A helper thread copying kept vertex lists caused a crash we nearly shipped: normal draws grew the frame's index buffer without waiting for it, so on the largest frame so far it could write through a stale pointer. The symptom was a GPU queue error about 11.8 s into 2-player Turnpike, every time. Turning features off one at a time found it in two runs, after reading the renderer's diff had not.

## Still open

- **3 and 4 players:** on most tracks the course or the insides of item boxes and dash panels draw black. v1.8.0 does this too. Cause unknown.
- **Awaiting hardware checks:** HOME and sleep, and movies.
- **Later:** replace SDL audio and input, and remove the 2D copy path if it never shows a gain.

## Credits

- [deko3d](https://github.com/devkitPro/deko3d) by fincs and devkitPro.
- [uam](https://github.com/devkitPro/uam), with patches from SwitchWakerHD.
- Deko3d guides by **StevensND** (nfsmw-nx) and **ChanseyIsTheBest** (sa2-nx). Their pitfall lists saved days.
- SwitchWakerHD's shipped deko3d renderer, used as a reference. Its ideas were rewritten rather than copied.
