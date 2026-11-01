# Plan: a deko3d renderer for MKDD-NX

The finished story, results and lessons are in [DEKO3D_MIGRATION.md](DEKO3D_MIGRATION.md).

## Status (2026-10-30, end of day)

deko3d is the only renderer: `switch/build.sh <gen>` builds `MKDD-NX.nro` with it (in `build-switch-dk`). The GL backend has been removed, so GL and Mesa references below are history. The last release with it is v1.7.1 in git.

**Measured on hardware** (handheld, SYSTEM clocks, 1.5x, HD textures, 16x AF, same race):

| | GPU per frame | fps |
|---|---|---|
| GL | about 23 ms | about 42 |
| deko3d, first build | about 18 ms | about 53 |
| deko3d, after the GPU work | about 15.3 ms as first measured; a corrected profile showed that figure included idle waiting, and real work in a 2-player race is about 11.3 ms | 59.9 (lowest 58.8) |

The go/no-go gate (at least 10% less GPU time) passed by a wide margin.

**GPU work, by measured effect:**
- **EFB copies** were the big cost: 15 a frame took 9.35 ms of about 17.
  - The copy shader skips filter rows of weight 0, and half-size colour copies use one bilinear read (5.4 ms).
  - The XFB copy (2.65 ms) goes through the 2D engine when it is a plain 1:1 row copy (`xfb_blit`).
  - Copies in a row share one barrier.
- **ZCULL:** a little faster, no artefacts. On by default.
- **No measurable change:** game mipmaps, depth compression, the tiled cache. Mipmaps and depth compression stay on: they look right, and distant textures shimmer less. The tiled cache stays off.
- **Uploads** share one barrier per draw, instead of one per texture.

**Switches** (an empty file in `sdmc:/switch/MKDD-NX/config/`; the first `mkdd.log` line shows the state):
- `dk_nozcull`, `dk_nozcomp`, `dk_nomips`, `dk_noblit`: turn a change off.
- `dk_tiled`: turns the tiled cache on (a test).
- `dk_profile`: one frame in 300 gets per-draw, per-copy and per-clear GPU timestamps (`gpu profile:` lines). The profiled frame drains between items, so its total is a little high.
- `dk_capture`: screenshots to `logs/dk_shot_N.ppm`.

**Still to check on hardware:**
- Docked (1080p swapchain), HOME / sleep, the pause screen, 2-player side by side (seam line), movies.

**Later:**
- Audio through audout instead of SDL, which drops SDL and `egl_stub.c`.
- The remaining half-size copies cost about 1.1 ms each, a lot for their size: worth a specialised copy shader.
- The game's full-screen effect quads cost about 1.2 ms each.

Written 2026-10-30 from three guides:
- StevensND, nfsmw-nx: `HowToDeko3d.md` and `deko3d-devlog.md`.
- ChanseyIsTheBest, sa2-nx: `deko3d-renderer-guide.md`.

It also draws on SwitchWakerHD's shipped deko3d renderer and on MKDD-NX's own code and logs.

## 1. What deko3d would and wouldn't buy us

The other ports saw large gains, but most came from **per-draw CPU cost**:
- NFS MW: the Vulkan driver's pipeline creation, at 49–82 ms per new pipeline.
- SA2: ~1,400 draws per frame at 2.4–3 µs each, plus descriptor updates.
- WWHD: Mesa's GL thread, at 9 µs per draw falling to 4.3 µs.

MKDD-NX's situation is different:

| Measure (handheld, recent logs) | Value |
|---|---|
| Batches per race frame | ~100–400 (4-player and heavy courses higher) |
| Renderer CPU per frame (`timing: render`) | 3–5 ms in races, 0.4 ms in menus |
| Real limit | GPU fill rate: AUTO resolution, not clocks, fixed the frame rate (see clocks memory) |
| Shader start-up | 186 TEV programs, 1.6 s at boot; new ones compile mid-race (Mesa, ~0.3 s each) |

**Expected gains, honestly:**
- **CPU:** about 2–3 ms of the main thread's per-frame cost back (no Mesa state validation, no `glFlush` kicks, no hidden buffer waits). This only shows in fps where the main thread is the limit (4 players, heavy scenes).
- **GPU:** gains come from features Mesa doesn't use or doesn't let us control, not from shader code generation. uam is the same nvc0 backend family as Mesa.
  - Hardware color compression on the EFB and copy targets (memory bandwidth, which is our bottleneck).
  - Explicit ZCULL.
  - No driver-inserted barriers.
  - Copies done as 2D blits where no format conversion is needed.
  - Hazard-only barriers.
  - A reasonable target is **10–25% less GPU time per frame**, which AUTO resolution turns into higher internal resolution or steadier 60 fps. **This is not guaranteed.** NFS MW's first deko3d build was **slower** on the GPU (35–43 ms vs 19–24) until it re-implemented what the driver had been doing for it.
- **Start-up and hitches:** a DKSH cache loads in milliseconds (WWHD: 7,680 shaders in 0.29 s). Fixed shaders are compiled at build time, and a new TEV variant compiles on a worker thread instead of stalling the frame.
- **Frame pacing:** we own the swapchain (image count, swap interval, when we wait). This is the right tool for the pacing complaints, independent of clocks.
- **NRO size:** about 14 MB smaller (no Mesa, EGL, glapi or drm_nouveau).

**Recommendation:** worth doing, but as a **measured, switchable second backend**, not a rewrite-and-hope. Keep the GL backend shipping until deko3d beats it on the same course at the same settings. Gate the work on a go/no-go measurement after P2.

## 2. Feasibility already checked

- devkitPro has `libdeko3d.a`, `libdeko3dd.a` (validating debug build) and `uam.exe` installed.
- Our current GLSL was run through `uam.exe` offline:
  - PRE_VS and FULL_VS compile as-is.
  - TEV_FS (uber shader) and XF_VS compile once given explicit bindings and with `u_squeeze` / `u_shift` moved into a UBO. They produce 13 KB and 22.5 KB of DKSH.
  - The only errors were deko3d's rules: explicit bindings, no loose uniforms. `usamplerBuffer` + `texelFetch` (XF data), `textureGrad`, `discard` and `gl_FragCoord.z` were all accepted.
- **Our data model is already backend-neutral.** `runtime/gx.c` produces `Frame`, made of `Batch`, `TevState`, `EfbCopy`, one vertex array, one index array and an XF texel array. `render_gl.c` only consumes it. **gx.c does not change.**
- **No GL feature without a direct deko3d equivalent:**

| GL feature | deko3d equivalent |
|---|---|
| R8/RG8 + `GL_TEXTURE_SWIZZLE_*` | `DkImageView` swizzle |
| BC1 (CMPR) and BC7 (HD textures) | native formats |
| D32F depth | `DkImageFormat_Z32_Float` |
| `glBlitFramebuffer` (seam blur) | `dkCmdBufBlitImage` |
| `glTexStorage2D` | image layout + heap |

  We never call `glGenerateMipmap`, so we don't need a mip-chain blitter.
- **Reusable code:** SwitchWakerHD (`E:\SwitchWakerHD\SwitchWakerHD-fork`) ships a deko3d renderer as default.
  - `runtime/third_party/uam` is uam 1.1.0 as a library with the patches that matter: reentrancy, output to memory, no `exit()`, deterministic scheduling, no dual issue (the 4x8 flicker). It is **MIT** and can be vendored as-is.
  - `runtime/src/gfx/deko/*` (C++, MPL-2.0) is the reference for memory, swapchain and mode switching, the tile-size fix and sync. MPL is file-level copyleft: copied files keep their MPL header inside our MIT project. **Rewriting the ideas in our C avoids that and is the plan.**

## 3. Architecture

```
runtime/gx.c ──Frame──▶ render.h (today's render_gl.h API, unchanged)
                          ├── render_gl.c     (current backend, kept)
                          └── render_dk.c     (new)
                        render_common.c  (backend-neutral, moved out of render_gl.c)
                        ui2d.h  ── ui2d_gl.c / ui2d_dk.c  (menus, font, pause art, charpack art)
                        present: SDL+EGL (GL)  |  nwindow + DkSwapchain (deko3d)
```

**Build:** `MKDD_RENDERER=GL|DEKO3D` in `switch/CMakeLists.txt` gives `MKDD-NX.nro` (GL) and `MKDD-NX-dk.nro` side by side in the same folder.
- Settings and mods are shared. Shader caches are separate files (`shaders.bin` for GL, `dksh.bin` for deko3d).
- `MKDD_DEKO3D_DEBUG=ON` links `libdeko3dd` for bring-up.
- No in-game renderer switch: two NROs are simpler and safer.

**render_common.c (moved, not rewritten):**
- `decode_copy`
- the texture-cache bookkeeping (keys, ages, eviction policy)
- the 4:3 HUD / shift / below math
- `tev_bytes`
- the movie-plane logic and hdtex queueing

Both backends call it, so behaviour can't drift.

**ui2d.h:** menu.c, ui_font.c, pause_art.c and charpack_art.c make about 84 direct GL calls. Replace them with a small API:
- textured quad
- solid rect
- scissor
- texture create/update from RGBA8

Port those files to it **on the GL backend first** (P1a). This is the only GL code outside render_gl.c, so afterwards deko3d needs just one new 2D implementation.

### render_dk.c design

**Device and present:**
- Create the device with `DkDeviceFlags_DepthZeroToOne` and drop the `2z - w` remap in the shaders.
- Create the swapchain on `nwindowGetDefault()`: 3 images (2 as an option), swap interval 1, RGBA8.
- Docked/handheld switch: destroy, then `nwindowSetDimensions`, then recreate (SA2: it is refused while buffers are registered). It runs from the existing OperationMode hook.
- SDL keeps audio only. `SDL_PumpEvents` becomes `appletMainLoop()`.

**Memory**, a few large blocks, each sub-allocated:

| Block | Flags | Use |
|---|---|---|
| code | `Code`, CPU uncached | DKSH, `DK_SHADER_CODE_ALIGNMENT`, keep `DK_SHADER_CODE_UNUSABLE_SIZE` tail free |
| images | GPU cached, image | EFB color/depth, copy targets, textures, HD BC7; free-list allocator, frees deferred until the slot's fence |
| ring ×3 slots | CPU uncached, GPU cached | vertices, indices, XF texels, TEV UBOs, staging for texture uploads |
| cmd ×3 slots | CPU uncached, GPU cached | command memory, `dkCmdBufClear` + re-add per slot; keep 256 B slack per piece |

Three slots match today's `NSLOT` fence scheme (`render_update` waits on the slot's fence and copies the frame's arrays in, as now).

**Descriptors:**
- An image descriptor set and a sampler descriptor set.
- Slots 0–15 are reserved for the renderer (EFB, XF buffer, blit); game textures use 16 and up.
- A freed slot is reused only after its fence.
- Samplers come from a small table keyed by wrap/filter/aniso, as `R.samplers` does today.

**Shaders:**
- Fixed shaders are compiled at build time by `uam.exe` and embedded with bin2s: XF_VS, PRE_VS, FULL_VS, the TEV uber FS, COPY_FS, BLIT_FS, the movie passes and the ui2d shaders.
- Specialised TEV FS (tev_gen.c GLSL) go to vendored **uamlib** on one worker thread (8 MB stack, priority 0x3B). The batch draws with the uber shader until its variant is ready. That is today's behaviour, minus the stall.
- `dksh.bin` maps FNV-1a(GLSL) to DKSH. It is appended and `fsync`ed, keyed with the uam patch level, and its entries are compiled on load as today.
- Source changes, shared by both backends:
  - explicit `binding=` on every block and sampler
  - loose uniforms (`u_squeeze`, `u_shift`, copy and blit uniforms) moved into a small UBO updated with `dkCmdBufPushConstants`
  - generated under `#ifdef` only where GL needs it otherwise

**Per batch** (the loop body of `render_update`, kept structurally identical):
- `pipe[0..3]` map to cached `DkRasterizerState`, `DkDepthStencilState`, `DkColorState`, `DkBlendState` and `DkColorWriteState`, emitted only when they change.
- `slot_layout`: one precomputed `DkVtxAttribState`/`DkVtxBufferState` set per layout (0–8 and 15).
- Bind **one vertex buffer per call** (the deko3d 0.5.0 overrun).
- Bind shaders when the program changes. Bind the UBO range when `ub_off` changes.
- Bind the 8 texture handles plus the XF buffer at binding 8, skipping unchanged handles.
- Set the scissor, then `dkCmdBufDrawIndexed(…, first_index, base_vertex)`.
- Submit (`dkQueueSubmitCommands`) every N draws, starting at 128 as a setting. This replaces the `glFlush` every 32.

**EFB copies and clears:**
- **Before a copy:** a `DkBarrier_Fragments` plus texture-cache invalidate, only if the target is sampled after being written since the last barrier. Track this per target with a "written" epoch.
- **The copy itself:** COPY_FS stays the general path (format conversion, Y/Z modes, the vertical filter). A copy with a plain RGBA8 format and no filter takes `dkCmdBufBlitImage`.
- **Clears:** `dkCmdBufClearColor` / `ClearDepthStencil` with the copy's scissor. Then invalidate the state cache's scissor, because clears respect it.

**Textures:**
- Decode exactly as today: `gx_texture_decode`, compact I/IA to R8/RG8 with view swizzles, CMPR to BC1.
- Write the result into the ring's staging area, then `dkCmdBufCopyBufferToImage`.
- Apply the **tile-size fix** to BC images 24–32 px tall: CMPR is common, and mip 1 would corrupt level 0.
- HD BC7 uploads use the same path.

**Present and overlay:**
- BLIT_FS draws the XFB into the acquired swapchain image (letterbox, wide, adjustments).
- ui2d draws the menus on top, then `dkQueuePresentImage`.
- GPU timer: `dkCmdBufReportCounter(Timestamp)` at the same points as today's queries, ×1.6276.

**Orientation:** deko3d clip space has y up and no negative viewports. Keep our convention (texel row 0 = top) by keeping the VS y negation and checking it with a quadrant test pattern in P1, as NFS MW did.

### Pitfalls to design in from day one (from the guides)

1. Bring-up uses `libdeko3dd`. Check `dkQueueIsInErrorState` before acquire, submit and present, and log it.
2. One vertex buffer per bind call. Keep 256 B of slack at the end of each command piece.
3. Apply the BC tile-height fix (`DkImageFlags_CustomTileSize`) for 24–32 px tall BC images.
4. Depth textures read (D, D, D, 1). COPY_FS's Z-copy path must take `.r`.
5. Clears respect the scissor: reset it, then make the state cache forget.
6. Invalidate ZCULL after any copy or blit into a depth image.
7. In uam, no dynamic component index into UBO vectors and no non-constant integer `/` or `%`. Audit tev_gen.c's output (its divisors are constant today). Treat every uam warning as an error.
8. GPU timestamps ×1.6276.
9. No busy helper threads (WWHD's spinning thread starved audio). The uam worker sleeps on a condvar.
10. No clock calls (clocks memory: SYSTEM stays default).

## 4. Phases, each with an exit test on hardware

Baseline before starting: the same course and settings (clocks SYSTEM, AUTO, 1 and 4 players) on the GL build. Log `render` ms, `gpu` ms, fps, and hitch counts over 3 laps.

| Phase | Work | Exit |
|---|---|---|
| **P0** shaders | Bindings and UBO changes in the GLSL (GL keeps working with them); vendor uamlib; CMake step for build-time DKSH; host check that compiles every shader in `shaders.bin` with uam, 0 warnings | GL build unchanged on hardware; every cached TEV variant compiles with uam |
| **P1a** split | `render_common.c` extracted; `ui2d` API with menus, font, pause and charpack art ported onto it on GL | GL build identical on hardware (menus, pause, charpack, title backdrop) |
| **P1b** device | `MKDD-NX-dk.nro`: device, swapchain, memory, a test pattern (quadrants and text), ui2d on deko3d, dock switching, HOME | menus usable on a test background; 10 min with no crash; dock/undock works |
| **P2** frames | `render_dk.c` full loop: batches, copies, clears, textures, XF, movies, present, GPU timer | title, menus, movie and a full race correct against GL screenshots |
| **Go/no-go** | Same-course comparison against the baseline | GPU ms per frame **≥10% lower** at the same resolution, with no visual regression. Otherwise stop: GL stays, the uamlib cache idea can still be weighed |
| **P3** parity | split screen (`vseam` blur), HD textures, aniso, 16:9 HUD paths, charpack art, all screen effects | 30 min of mixed play with no crash; 1–4 players |
| **P4** perf | color compression, ZCULL, hazard-only barriers, blit fast path, submit size, state skipping, 2 vs 3 swapchain images; each one behind a debug option | each change measured on its own |
| **P5** default | deko3d becomes `MKDD-NX.nro`; GL ships as `MKDD-NX-gl.nro` for one or two releases; README, wiki, third-party notices (deko3d zlib, uam MIT/zlib) | user sign-off |

**Size estimate:**
- render_dk.c plus ui2d_dk.c: about 2,500–3,500 lines of C. SwitchWakerHD's is 10k lines of C++, but it emulates the far more complex GX2.
- P0 and P1a are low risk and also improve the GL build's structure.

## 5. Open questions for the user

- **Two NROs during the transition:** is that acceptable, or should it be a hidden setting in one NRO? Two NROs is recommended.
- **Ryujinx:** SA2 used it for picture and hang checks of its deko3d path. Do we want that, or hardware only?
