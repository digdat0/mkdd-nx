# Optimisation pass, 2026-10-12 (night)

## What your Switch logs showed (first build)
- Menus ran at 20-36 fps. Races ran at 5-6 fps on screen and about 9 game frames per second.
- The graphics decoder thread shared CPU core 1 with the game.
- The renderer drew everything with one big "uber" shader and re-created its buffers every
  frame.

## Measured on PC (race, i7-3930K): graphics decoder
Before: one full core, at about 1,450 CPU cycles per vertex.

The cause: MKDD sends about 14,000 tiny draws per frame, roughly 5 vertices each, and the
decoder redid its full setup for every one of them.

After: about 3.3× less decoder work per frame. The changes:
- Per-draw setup is cached and only rebuilt when the graphics state changes.
- The lighting and vertex loops are faster.
- Frame buffers are reused instead of being reallocated (an 8 MB allocation per frame).
- The decoder thread is only woken when it's actually asleep.

## Graphics decoder placement (Switch)
The decoder now runs on its own core (core 2). Core 0 renders, and core 1 runs the game.

## Code generator (tools/gen_c.py): the game's own code
- **Registers in locals.** Guest registers live in C local variables for the whole function and
  are only written back at calls and returns. This uses a dataflow analysis of which registers
  may be dirty. The hottest animation and matrix functions now take about half the time on PC.
- **Paired singles inline.** The GameCube's vector math instructions compile directly to C
  instead of going through a runtime decoder.
- **FPU status flags.** The exception flags (FPSCR) that every float instruction updates are now
  function-local scratch values, so the compiler drops them. Only 4 instructions in the game
  ever read them, and the functions containing those keep the exact behaviour.
- **Testing.** Verified on PC: boot → menus → a race, with 0 unresolved calls and a save
  written.

## Renderer (Switch only; needs your hardware to measure)
- **Specialised shaders.** Each material gets its own TEV shader instead of the uber shader. At
  most 2 are compiled per frame; the uber shader draws in the meantime. All 102 shaders seen
  in a race were checked offline with Mesa's GLSL compiler (uam).
- **Streamed buffers.** Vertex, index and uniform data stream into triple-buffered,
  fence-guarded buffers instead of calling glBufferData every frame.
- **Smaller vertices.** Colours are now RGBA8, which shrinks each vertex from 112 to 88 bytes.
- **Fewer state changes.** Texture, sampler, program and blend/depth state changes are skipped
  when nothing changed.
- **GPU clock.** Handheld now asks for the retail 460 MHz GPU profile (it was 307 MHz).
- **New log lines** in mkdd.log, every 300 frames:
  - `timing: render X ms/loop, swap wait Y ms/loop; N TEV programs (Z ms compiling)`
  - `apm: handheld profile raised`
  - In mkdd_trace.log: `gx decoder: N% busy, D draws/5s`

## What to send back after testing
mkdd.log, mkdd_trace.log, stderr.log and any crash.log. Say which screens you tested (menu,
race) and roughly how it felt. The new shaders can stutter briefly the first time each
material appears. That's expected.

## Next ideas, roughly by expected gain
1. Save the shader keys to SD and precompile them at boot, so the first lap doesn't stutter.
2. More generator work: cheaper compare and condition-register code, an inline fast path for
   GX FIFO writes, and lighter function-entry bookkeeping.
3. Transform and light vertices on the GPU (vertex shader) instead of on CPU core 2.

# Round 2, 2026-10-13 (overnight)

## Your second test (first optimised build)
- Screen refresh: a steady 60 fps. The renderer uses only 2-3 ms per frame, and audio had
  0 underruns.
- Race: 22-25 game frames per second. The limit is game logic on core 1, plus the graphics
  decoder, which the game waits for every frame.

## New PC benchmark
`pc/bench.sh` runs a race with guest time 4× faster (`MKDD_TIMESCALE`), so the game runs as fast
as the CPU allows. Results go to `pc/run/results.txt`.

| build                                    | race game fps (PC) |
|------------------------------------------|--------------------|
| baseline (your second test's build)      | 51.4               |
| + cheaper function entry, stack fast path| 63.2               |
| + GXDrawDone doesn't wait for the decoder| 63.5               |
| + cheaper vertex colour packing (decoder −8% cycles) | 63.2   |

The game now also waits for the 240 Hz test VI, so the last two steps barely move the fps
number. The decoder's CPU time did drop.

## Changes
- **Generator.**
  - Ordinary functions skip the HLE check and bypass bookkeeping. Only hooked functions take
    the out-of-line path.
  - Loads and stores through r1, r2 or r13 (stack and small-data areas) skip the MMIO check.
    MEM1 is now a 32 MiB allocation, and these accesses are masked to 25 bits.
- **Runtime.**
  - Guest memory helpers are inlined (the matrix-library HLE calls them per float).
  - New `GXDrawDone` handler: it enqueues the draw-done marker and returns without waiting for
    the decoder, so the decoder can trail behind like the real GPU. If you see graphics
    glitches, this is the first suspect.
- **Shader cache.** Specialised shaders are recorded in `sdmc:/switch/MKDD-NX/shaders.bin`. On
  later boots they compile up front, shown as "PREPARING SHADERS x / y" (about 20 ms each), so
  races don't stutter on new materials. Keep `shaders.bin` when you update the .nro.

## Profile facts for the next steps
- The game thread waits in OSReceiveMessage (VI retrace) and in thread switches
  (OSWakeupThread, OSRestoreInterrupts): other guest threads, mainly audio, take turns.
- Decoder time is spread over per-vertex maths: transform, lighting, texture-coordinate
  generation. The next big step there is doing transform and lighting on the GPU.
- On Switch, the CPU overclock (sys-clk 1785 MHz) is now the cheapest large win.

# Round 3, 2026-10-13 (day)

## Your third test
- Stock clock: race at about 27 game fps. CPU 1785 MHz: about 40 game fps. The game core's CPU
  time is the limit (the decoder is only 40-60% busy).

## Where the game thread's time went (PC, attributed to PowerPC instructions via line info)
- Function entry and exit: 28% (register loads and stores around every call).
- Loads: 12% (`lwz`). Paired-single loads and stores: 12%.
- Runtime: matrix HLE 4%, FIFO writes 2-5%.

## Changes
- **Scheduler.** Each guest thread waits on its own condition variable. A handoff wakes only the
  thread that gets the token, not every guest thread on the Switch's single game core.
- **Native-endian register file.** Register loads and stores at calls no longer byte-swap.
- **Callee-saved registers stay in locals across calls** (r14-r31, f14-f31, cr2-cr4). Calls
  into CodeWarrior's `_savegpr` / `_restgpr` / `_savefpr` / `_restfpr` routines and the OS
  context functions still store and reload everything. `gen_c.py --check-lazy` verifies that
  callees preserve them. On a full race only those routines and boot code were flagged.
- **Scratch registers.** Ghidra's `tea` and `r2Save` are function-local.
- **Cheaper RAM test** on loads and stores: one bit test.
- **Paired singles.** Float-pair `psq_l` / `psq_st` do one 8-byte access.
- **GX FIFO.** Bursts are batched 16 KiB at a time instead of locking the decoder queue every
  32 bytes. They are flushed at every HLE call and every hardware register access.
- **Texture reuse.** Textures that pile up (the intro movie) are evicted after 2 s instead of
  10 s and reused for same-size textures, instead of being deleted.

Result on PC: about 20% less game-thread CPU per frame (29.8 → 23.5 M cycles). PC fps doesn't
change because the PC run is now mostly waiting for the VI.

# Round 4, 2026-10-13 (afternoon): vertex work on the GPU

## Your logs (build 10:55)
- Stock clock: races ran at 31-40 game fps. Both the game thread and the GX decoder were over the
  60 fps budget; the decoder reached 90-110% busy in heavy scenes.
- 1785 MHz: races ran at 52-60 game fps.
- The render thread (Mesa) took about 4 ms per frame. It was not the limit, so switching to
  deko3d would not have helped yet.
- Quit crash: closing from HOME while the game was mid-frame. Fixed in the 11:58 build, and you
  confirmed it on hardware.

## GPU vertex transform
- The decoder no longer transforms, lights or texgens vertices on the CPU. Each vertex now
  carries its raw attributes plus two ids:
  - an XF record: projection, viewport, colour channel controls and texgens;
  - a matrix slot.
- Records, light blocks and matrices go into a per-frame texture buffer (RGBA32UI, unit 8).
  The vertex shader `switch/xf_vs.h` does the XF stage.
- Skinned meshes with per-vertex texture matrices go through a small indirection record, so
  every vertex now takes the GPU path.
- **Check.** `gx_xf_eval` is a C copy of the shader. `MKDD_XFCHECK=1` on PC runs it next to the old
  CPU path for every vertex: 212 million vertices in a race, 0 mismatches. The shader passes
  Mesa's compiler (uam).
- **Fallbacks.**
  - An empty file `cpu_xform` in the app folder switches back to CPU transform.
  - If the shader fails to compile on the console, the CPU path is used automatically.

## Other decoder changes
- Draw setup is rebuilt only when state it read changes. Before, it was rebuilt on every XF
  write, and J3D reloads a matrix before most draws.
- XF writes of unchanged values are skipped. Matrix caches are invalidated per matrix.
- Vertex colours are read as RGBA8. Each attribute's format switch runs once per attribute,
  not once per component. Unused texcoords are skipped.
- Texture change detection reads 128 samples of 8 bytes instead of 512 single bytes, so it
  touches 4x fewer cache lines per texture per frame.

## Game thread
- **GX FIFO.** Writes are assembled directly in the staging buffer: no per-burst memmove or copy.
- **Function entry.** Only the ~100 functions the runtime can hook test the HLE table. The
  others use `ctx_enter_nh`. At boot the runtime logs any hook that lands on a function compiled
  without the check.
- **Switch only.** Runtime globals are hidden symbols, so the code addresses them PC-relative
  instead of loading each address from the GOT.

## New log lines (mkdd_trace.log, every 5 s)
`game core: X% busy, idle Y% (Z% while the decoder was busy), queue-full waits W ms/5s; gx setups S/5s, CPU-transformed verts V/5s`
- If Z is high, the game is waiting for the decoder.
- If X is near 100%, the game code itself is the limit.

## PC measurements (race, per game frame)
| build | game thread | decoder |
|---|---|---|
| Round 3 (v10) | 24.5 M cycles | 29.9 M cycles |
| GPU transform (v11) | 23.2 | 16.1 |
| + attribute / texture-hash work (v12) | 22.5 | 14.3 |

# Round 5, 2026-10-13 (evening): profiler, runtime fixes, `--abi`

## Switch profiler (`switch/prof_switch.c`, `tools/prof_report.py`)
- A thread samples the game thread and the GX decoder at about 500 Hz. Each sample pauses the
  thread, reads its context, walks the frame pointers and resumes it.
- It writes `mkdd_prof.bin`. To disable it, put a file named `noprof` in the app folder.
- To read a capture: `py -3 tools/prof_report.py mkdd_prof.bin debug/<elf>`.
- First capture (build 15:12): the game thread was busy 96% of the time.
  - 76% was generated code, and its self time was flat. No single function took more than 3%.
  - Within generated code, guest loads and stores took about 33%, register-file traffic about 20%,
    and stack spills about 13%.
  - The remaining 24% was runtime.

## Runtime fixes
- `rt_deliver` (interrupts) saves only the used register ranges, about 9 KB on the stack. It
  used to copy 64 KB.
- `ctx_enter_hooked` flushes the GX FIFO only before interrupt delivery and on the bypass path.
  `rt_hle` skips the flush for pure hooks (interrupt enable/disable, matrix and vector math).
- Gather-pipe bursts are copied into the RAM FIFO only while a display list is being recorded.
- `core_dispatch` has a 2048-entry direct-mapped cache for indirect-call targets.

## `--abi`: registers stay in host locals across calls
Calls and returns no longer store volatile registers (r5-r12, f0-f13 and their ps1 halves,
CTR, ...) that the other side never reads. Under the EABI:
- Indirect callees read only argument registers (r5-r10, f2-f8, ps1 of f1-f8, cr1).
- A call clobbers all volatile registers.
- A return hands back only r3, r4, f1 and ps1 of f1.

A per-function backward liveness pass gives:
- the volatile registers each function reads at entry, which are stored before every call to it;
- the registers read after each call site.

Calls to hookable functions and to the save/restore helpers still store everything.

- Bug found by the check build: the prologue helpers `_savefpr_*`/`_savegpr_*` run before a
  function copies its arguments to non-volatile registers. Treating them as clobbering calls
  dropped r5 and f2-f4 from such functions' entry sets (FUN_80182490 then read a poisoned r30).
  Liveness now passes through these helpers.
- `--abi-check` (PC) fills every skipped store with `ABI_POISON`, both before calls and at
  returns. It also logs indirect calls into functions that read more than arguments at entry, and
  call sites whose later reads find poison. The final scripted race completed with no unmapped
  reads.
  - About 80 reports remain. Sampled ones are analysis imprecision: computed jumps join every
    block, and liveness is path-insensitive in loops.
  - Unmapped reads now log the host return address, which maps to the generated `f_XXXXXXXX`.

## PC measurements (race, per game frame, game thread)
| build | game thread |
|---|---|
| v14 (Round 4) | 23.60 M cycles |
| runtime fixes | 20.95 |
| + `--abi` (v16) | 17.83 |

The Switch build is `debug/MKDD-NX-1757.elf`, generated from `E:/MKDD/genAR` (`--abi`).

## Hardware test of build 1757, and the music-stream fix (build 1931)

- Four races at stock clock. Average game speed in races was 39.9 fps, against 38.9 and 35.4 in earlier runs (the heavy course went from 35.4 to about 37.6). The game thread is busy 100% of the time, and its profile is flat: 81% of time is in generated game code.
- The one-second freezes on new courses are mid-race TEV shader compiles (about 1 s each on Mesa, 35 new programs this session). They are saved to shaders.bin, so they happen only the first time a course is played.
- Music stopping and staying off (it also happened before `--abi`, in StockLogs_UIHeavyMap) was caused by the DSP running ahead of the audio thread. JASDSPChannel::updateAll releases voice groups 0-3 after each sub-frame's channel update, and the real ucode waits for those releases. Our HLE rendered sub-frames back to back. When the CPU was busy, JASAramStream's loop-point move came too late, and it set `sFatalErrorFlag`, which stops every stream for the rest of the session (seen in the trace as DVD reads dropping to 0). dsp.c now renders a sub-frame only after release-halt group 3.

# Round 6, 2026-10-13 (night): native engine functions, register-preserving slow paths

Profile of build 1757 on hardware: CPU-bound game thread (~25 ms/frame in races). The engine layer (J3D, GX API wrappers, MTX library, course collision) is about a third of it. Per-instruction view of game code: 34% of samples on guest memory accesses, 12% on stack spills, 8% on register-file traffic.

## Step 1: native functions (runtime/native.h, native.c)
- `NATIVE_LIST`: guest functions with a C version. The generator (gen_c.py) turns direct calls into `n_<name>(c, M, a0..a7)`, with the arguments taken from the caller's register locals. There is no register-file flush, no hook dispatch and no reload. Tail calls are handled too. The recompiled function itself jumps to the native on entry, which covers indirect calls (virtuals) and the runtime. `c->bypass` keeps the original reachable.
- Flags: 1 = may call guest code (the caller flushes first; the native keeps the original's stack frame so callees run at the same addresses); 2 = writes MSR (callers that cache MSR make a normal call); 3 = writes the gather pipe.
- Verifier (`gen_c.py --native-verify`, `MKDD_NATIVE_VERIFY=1 MKDD_NATIVE_VERIFY_MS=1000`): runs the recompiled original and the native from the same RAM and register snapshot and compares RAM (except the dead stack below r1) and the result. For gather-pipe functions it compares the captured FIFO bytes. All 29 natives were checked in a race with 0 wrong. recursiveCalc showed a few float-close cases before it reserved the original's frame.
- 30 natives: PSMTX Copy/Concat/MultVec/Inverse/Quat, PSVECCross/Mag, OS interrupt enable/disable/restore, the J3D deformer blend, J3DAnmTransformFull::getTransform, keyframe Hermite interpolation, J3DMtxProjConcat, the 4-way animation blend (FUN_800089e4, devirtualises getTransform) with its quaternion helpers, J3DJoint::recursiveCalc, GXLoadPos/TexMtxImm, GXLoadLightObjImm, GXSetTevColor and GXSetArray, the CrsGround triangle test (FUN_8017ddd4) with its transform/plane helpers, and the collision flag loop.
- Found by the verifier: the old HLE versions of PSMTXMultVec/Cross/Mag/Inverse did not match the paired-single assembly (for example MultVec off by 0.003 on values near 36). They now follow the assembly operation by operation and match bit for bit.
- PC: 18.21 → 16.39 Mcyc/frame on the game thread (about 10%; PC run-to-run noise is about 5%).

## Step 2: slow paths that keep registers (runtime/mmio_tramp.c, gc_core.h mmio_rd/mmio_wr)
- Every guest load and store has a cold `bl rt_mmio_read/write`. As normal calls they clobber all caller-saved registers, so GCC kept most guest-register locals in stack slots. f_8017FD0C: 402 PPC instructions became 14,553 A64 instructions with 5,364 stack accesses.
- Now an assembly trampoline preserves every register except the result. On AArch64 it also appends gather-pipe writes in assembly, calling hw_gather_commit only per 32-byte burst. The same function is now 9,985 instructions with 630 stack accesses, and the .nro went from 64.0 to 51.2 MB. An x86-64 version exists for testing. The PC gains nothing from it (16 registers; no FIFO fast path), so the PC bench cannot show this step. `-DMKDD_NO_TRAMP` turns it off.
- `sc` (only DCFlushRange etc.'s cache sync, about 800k/s) is now an inline counter instead of a register flush, an intrinsic call and a mutex.

Switch build: debug/MKDD-NX-2232.elf.

Correction (found in Round 7): build 2232 compiled `E:/MKDD/gen2` (generated 12:55, before the natives and the inline `sc`), not genN4. `switch/build.sh` only ran cmake when `build-switch/Makefile` was missing, and `GEN_DIR` is a cache variable, so passing a new folder had no effect. 2232 therefore had the runtime changes (trampolines, the HLE-routed PSMTX/OS natives) but no direct native calls from game code. build.sh now configures on every run, and cmake prints the generated-C folder it uses. gen2 was also generated without `--abi` (the register liveness trimming from Round 5), so 2232 lacked that as well.

# Round 7, 2026-10-13/14 (night): more natives, options menu, widescreen

## Natives, batch 2 (runtime/native.c)
Picked from the build-1757 hardware profile, then checked against a fresh PC profile with batch 1 in place:
- Math: PSVECNormalize, J3DPSCalcInverseTranspose, J3DGetTranslateRotateMtx, J3DGetTextureMtx(Old), J3DGetKeyFrameInterpolation<f>, and J3DAnmTransformKey::calcTransform (FUN_8005826c: S/R/T key tables per axis, calling both interpolators natively).
- J3DMaterial::calcCurrentMtx: 8 virtual getTexCoord calls per material. Devirtualised for the two known accessors, with a guest call otherwise (flag 1).
- GD display-list writers, which the SDK writes one byte at a time through the list pointer in memory: the texture-matrix XF loads FUN_800546a4 and FUN_80054080, the TEV colour BP pairs FUN_800671c8 and FUN_80067338 (with GDOverflowed as a guest call), GDPadCurr32 and GDFlushCurrToMem.
- Gather pipe: FUN_80067a64 (a GXLoadPosMtxImm copy) and FUN_80067ae8 (normal matrix 3x3).
- Culling: J3DUClipper::clip (FUN_8000a3c8), the per-material visibility loop FUN_80183934 and its flag update FUN_80183afc.
- Course areas: FUN_8018af28, which finds the area of a kind containing a point (with area_local / area_weight for FUN_8017cea8 / FUN_8017cffc). Also stMakePlaneParam and FUN_8017b7ac (projection onto a course plane).
- Cache maintenance: DCInvalidateRange, DCFlushRange, DCStoreRange and the NoSync variants. Here they are loops of no-op dcbX instructions (one per 32 bytes; about 200k calls per race) plus the counted `sc`.
- Widescreen hooks: System::getPerspAspect{Div2,Div4,Sub} and C_MTXPerspective (see below).

Verifier (`MKDD_NATIVE_VERIFY_MS=4000`, scripted race, `pc/verify.sh`): 55 natives exercised, 0 wrong, 0 float-close. FUN_8017cea8/cffc and stMakePlaneParam ran only inside other natives. The Div4/Sub aspect getters (split screen) were not reached.

Generator pitfall found while measuring: `--abi` was opt-in, and tonight's first generations left it off. All of them were ~6% slower on PC than genN4, whichever natives they had (bisected with the new `--native-skip=`; the diff showed extra stores of r0/CR/CTR at every call and return). The ABI trimming is now on by default (`--no-abi` turns it off).

PC (game thread, race, no trampolines on either side, alternating runs): batch 1 only (genN4) 15.97 / 15.89 Mcyc per frame, with batch 2 (genN8) 15.43 / 15.20. That is about 4%. The hardware profile had these functions at 2-4x their PC share.

## Options menu (switch/menu.c)
- − opens it (input to the game is neutral while it is open, and until buttons are released after closing). Items: widescreen, FPS counter, D-pad mode (steering = acts as the left stick, full deflection, 72 on diagonals like the GameCube gate; or the GameCube D-pad), button mapping (GameCube A/B/X/Y/Z/L/R/Start ← any Switch button except −; reset to defaults), exit with confirmation. The old +/− exit combo is gone.
- Saved to `settings.ini` in the app folder.
- Drawn after the XFB blit by one small instanced program: each instance is a rectangle, solid or a 5x7 glyph from uniform bits. There are no buffers or textures, and the renderer's state cache is untouched (it resets after each frame).

## Widescreen
- The kart cameras take their aspect from System::getPerspAspect* at camera init (all modes, also the lens flare). The other 3D scenes (character select, menus, replays) build theirs with C_MTXPerspective. With widescreen on, both return aspect × 4/3, and render_present stretches the 4:3 XFB to 16:9. The game's own clipper uses the same camera aspect, so objects at the sides are not culled. The EFB size (640x528) is unchanged, so this is not a resolution change.
- The 2D layer (HUD, menus) is stretched by 4/3. A camera picks up the new aspect when it is created, so a change applies from the next race or screen.

Switch build: debug/MKDD-NX-0227.elf, from E:/MKDD/genN8 (ABI on, 63 natives, menu and widescreen); .nro 49.4 MB.

# Round 8, 2026-10-14: hardware test of 0227, fixes
Test of 0227: menu, FPS counter and D-pad steering work; the race in the second run held 60 fps presented (~53 game fps, game thread 100% busy). Boot took 120 s: 203 cached shaders at ~0.58 s each.
- **Crash (GPU thread, display-list call, FAR inside guest RAM; same as the 10-13 build-1118 crash; the 2232 one was in fseek).** libnx `threadCreate` without a caller stack allocates stack+TLS with an unaligned size but `svcMapMemory`s whole pages; the source pages become NotMapped for the thread's life, so the start of the next heap block faults when used. Guest threads get 32 MB stacks at spawn (THP movie threads mid-game). `pthread_start` now allocates page-exact stacks itself (`pthread_attr_setstack`). The crash handler logs the FAR's memory region (svcQueryMemory) and the guest RAM address, to confirm if it ever recurs.
- **Exit:** threads started by `pthread_start` are paused (svcSetThreadActivity) after SDL_Quit, before returning to the loader.
- **Shaders:** the XF vertex stage is one separable program (glCreateShaderProgramv); each TEV shader is a fragment-only program in a pipeline. Varyings have explicit locations. Cached shaders also draw one empty triangle at boot so Mesa's GPU translation happens then, not at the first race. Expected boot: seconds instead of 2 minutes; mid-race new shaders ~20-40 ms instead of ~0.6 s.
- **Widescreen 4:3 HUD (menu: WIDESCREEN HUD 4:3 / STRETCHED, default 4:3):** the decoder flags orthographic draws (rec bit 30, XV_HUD) except untextured full-width ones (fades); in the pass that ends in the XFB copy, the vertex shader scales their clip x by 0.75 (u_squeeze). HUD, menus and movies keep their shape, centred; 3D stays full width. Known limit: 2D drawn at projected 3D positions (lens flare, markers) is narrowed too; scissor rectangles are not narrowed.

Switch build: debug/MKDD-NX-0703.elf (genN8, same natives as 0227).

# Round 9, 2026-10-14: more natives, cheaper GX writes and virtual calls
Picked from the 0227 race profile (game thread 100% busy, ~53 game fps; flat: no unconverted function above 1%).

## Runtime
- **Gather pipe:** completing a 32-byte burst used to save all 32 SIMD and 16 integer registers and call hw_gather_commit, 8 times per 32 bytes of display list (~1.9% of the game thread in that save/restore, plus 0.7% in gather_commit). While the CPU FIFO is linked to the GP and the stage has room, the assembly now just moves the burst to the staged bytes and adds 32 to `g_gx_gp.pending`; hw.c advances the PI write pointer by the pending bytes (with wrap) before anything can read it: hw_gx_flush (every MMIO access, non-pure HLE call and interrupt delivery) and gather_commit. g_gx_bytes is counted there too. The stage/gather lengths are one struct (`g_gx_gp`) so the fast path needs one address.
- **Indirect calls:** ctx_call checks the dispatch cache itself and calls the target, instead of going through rt_dispatch and core_dispatch (1.67% + 1.27% + 0.64% self in those three).

## Natives, batch 3 (77 in total)
- J3D joints: FUN_80009398 (Basic), FUN_8005c08c (Maya), FUN_8005be04 (Softimage) per-joint matrices, FUN_8000a1b4 (animated J3DMtxCalc::calc). recursiveCalc calls FUN_8000a1b4 natively when it is the joint's calc (vtable +0x28), so an animated joint no longer leaves native code.
- FUN_8005b564 (deformer blend, 2.4%): sums kept in registers per position instead of a store and reload per key.
- CrsGround::search core FUN_8017fd0c with its quadtree cell (FUN_80181370) and leaf triangle loop (FUN_8017da80) as C helpers; FUN_8017b7ac split into crs_proj (host values).
- J3D: calcDiffTexMtx, J3DMaterial::diff (the 9 GD bytes in one write), J3DMaterialAnm::calc; J3DTexGenBlock::getTexMtx devirtualised (both implementations).
- GX: GXLoadTexObjPreLoaded (flag 4: calls the TLUT region callback and writes the FIFO), FUN_80067bc8 (3x3 XF matrix load).
- Loops: FUN_801826d4, FUN_80183c08, FUN_80183d2c; JAISoundHandles::setPos.
- New flag 4 = may call guest code and writes the gather pipe (gen_c.py flushes registers as for 1; the verifier captures the FIFO as for 3).

Verifier (`MKDD_NATIVE_VERIFY_MS=4000`, scripted race): 77 natives, 0 wrong, 0 float-close. Note: at 300 ms the snapshot/compare of 24 MB per check (77 functions) slows the game to ~1 fps and the pad script never reaches the race; use 4000.

## Hardware test of 0703 (no crash)
- Boot: "205 cached shaders ready in 1.7 s" (was ~120 s). Race: 60 fps presented, ~52 game fps (234-289 game frames/5 s), game thread 100% busy.
- Intro movie: game thread 83-92% busy, ~50 fps, AI buffers 244-257/5 s instead of 286, 9-11 audio underruns/5 s. Profile: thp_decode 73% + its Huffman decode 11% of the game thread. Earlier "the movie thread is mostly idle" was wrong.

## Also in this round
- **THP decoder (runtime/thp.c):** 64-bit MSB-first bit buffer, 9-bit Huffman lookup table (longer codes through the canonical walk), and the IJG "islow" integer IDCT (12-bit fixed point, columns without AC terms short-cut) instead of bit-at-a-time reads and a 1024-multiply float IDCT. Whole intro movie (2700 frames of /Movie/play1.thp, decoded from the ISO in a standalone test): 20.3 → 3.3 ms/frame on the PC, every frame decodes, output differs from the float IDCT by at most 1 level (0.07% of bytes).
- **DSP rendering on a worker thread (runtime/dsp.c, core 2):** a release (group 3) hands the sub-frame to the worker; its completion mail raises the DSP interrupt, whose handler only runs JAudio's callback. A mutex guards the mail queue and frame state. MKDD_DSP_SYNC=1 and the native verifier keep the old in-interrupt rendering. PC race at real time: 2002 sub-frames/5 s and 32 stream reads/5 s with either mode, 0 underruns. Under MKDD_TIMESCALE=4 the worker's handshake renders fewer sub-frames than the guest asks for (benchmark mode only).
- **PSMTXConcat / J3DMtxProjConcat:** two lanes at a time (NEON on the Switch, the same operation sequence in SSE2 on PC for the verifier). 10 M random cases against the scalar code: identical except NaN payloads.
- **Interrupt delivery** saved and restored 0x7000-0x7FFF (the 4xx DCRs, never used by Gekko code) besides the real registers: now ~5 KiB per delivery instead of ~9.
- PC (no trampolines on either side, alternating runs, game thread): genN8 16.9 / 16.6 Mcyc per game frame, genN9 15.6 / 15.6 (about 7%). The gather-pipe and register-save changes only help on AArch64.

Switch build: debug/MKDD-NX-0814.elf (genN9, 77 natives).

## Round 10: display modes, face culling (build 0919)

- **Face culling was never implemented.** GX gen mode bits 14-15 (hardware encoding: 1 = back,
  2 = front, 3 = all) now go into `TevState.pipe[2]` and the renderer applies them with
  `glFrontFace(GL_CW)`. The orientation was measured with `MKDD_CULLSTAT=1` (pc/main_pc.c):
  on-screen triangles of cull-back batches are about 10:1 clockwise by area in the y-up clip space.
  Triangle strips now alternate their winding. This fixes the item boxes: `mat_boxin` (opaque
  rainbow, cull front) was drawing its outside faces over `mat_question` (the "?").
- **Display modes** (− menu): 4:3, 16:9 stretched (original camera), 16:9 wide view (camera
  aspect x 4/3, as before). In wide view the optional 4:3 HUD no longer squeezes full-width 2D
  draws (backgrounds, dimming), except movies (`g_thp_frames`). The scissor of squeezed batches
  is narrowed too, so 2D the game parks beside the 4:3 screen stays hidden.
- `settings.ini`: `display=0|1|2` replaces `widescreen=` (old files map to wide view).

## Hardware tests of 0814 and 0919, fixes (build 0939)
- **0919, broken rendering** (black track and ground, black intro movie, mirrored sign text): the face culling was inverted. GX front faces wind clockwise in GX clip space, but xf_vs.h writes `-cp.y` (the EFB is drawn upside down), so GL sees them counter-clockwise. The "mirrored" text was the backs of signs showing through. Fixed with `glFrontFace(GL_CCW)`.
- **0814, audio crackled badly** while the pacing counters stayed normal (2002 sub-frames and ~286 AI buffers per 5 s, 0 underruns). The likely cause is the new DSP worker: each sub-frame needs a thread wake and an interrupt round trip, so a frame can be unfinished when JAudio mixes it into the AI buffer. Sub-frames render in the interrupt again (as in 0703); the worker is opt-in with MKDD_DSP_ASYNC=1. Unconfirmed: the PC harness plays no audio, and its counters were normal with the worker too.
- **0814, item icon cut in half** (16:9 wide view + 4:3 HUD): the item window's scissor was not narrowed with its squeezed vertices. 0919 already narrows the scissor of 4:3 HUD batches.
- 0814 race: 235-300 game frames per 5 s (0703: 234-289).

Switch build: debug/MKDD-NX-0939.elf (genN9).

# Hardware test of 0939, save-file fix (genN10)
Test of 0939: culling fixed (track, intro movie, signs correct), item boxes show "?". Race 52-60 game fps.
- **Save not found on one boot (CARDOpen -> -4, then the game offered to create a new file).** The card emulation compares a file's game code with the disc ID at 0x80000000. The SDK's `SetEffectivePriority` (from `OSSetThreadPriority`, ~1 boot in 3 when thread 0x8048b520 is lowered to priority 4 while ready) calls `UnsetRun`, which moves a ready thread between the SDK's run queues. This port's scheduler keeps no run queues, so the thread's queue pointer is null and `UnsetRun` wrote zeros over 0x80000000 (found with an x86 hardware write watchpoint armed per thread at start; slower instruments hid the race). Every save so far was created under a blank game code; a boot where the zeroing came after the card check looked for "GM4E01" and missed it. Fixes: `SetEffectivePriority` is an HLE (hle.c binds it; regenerated as genN10 so it gets the hook check) that sets the priority in the OSThread and in the scheduler (priority changes were also ignored by the scheduler before); `memcard.c find()` treats blank-code files as this game's (after an exact match). Verified: 30/30 PC boots keep the ID; the user's old save opens unchanged. Race benchmark unchanged (66.7 vs 66.8 game fps).
- **Atmosphere crash on launching 0939 without a reboot:** vi crashed, then our app died in libnx `__nx_win_init` (2345-0018) before main(). Same pattern as 05:37 after the 0227 crash. The 0919 run's log ends without a crash; the exit path now logs `exit: menu EXIT GAME` or `exit: close request` to tell how a session ended.
Switch build: debug/MKDD-NX-1219.elf (genN10).
- Race profile (0939, 58-88 s, game thread 96% busy): DriverModel::update 19.8% inclusive (J3DModel::calc, faceCallBack, Neck/LumbarCtrl, Feeling), J3D draw 10% (loadMtxConcatView 6.7%, J3DShapeMtxMultiConcatView::load 6%), GeographyObjManager ~6%, J2D/Race2D HUD ~5%+3%, JAudio updateDSP 3.9%; runtime: rt_mmio_write_tramp 2.8%, ctx_call 2.0%, memcpy 1.8%, gather copy/commit 1.9%.

# Round 11, 2026-10-14: animation, J3D draw chain, HUD, port overhead (genN11, 101 natives)
Picked from the 0939 race profile (flat: no unconverted function above 0.64%). tools/prof_report.py has `--under NAME` for the self time inside one subtree.
## Port overhead
- **Gather-pipe stores the compiler can see** (`lis rX,0xCC01` / `st -0x8000(rX)`, constant after folding): `mst` appends them in line (`gp_wr`, gc_core.h) while the 32-byte burst is incomplete; only the store that completes it calls the trampoline. GX code in the game (J3D packet setup, K2D quads, GXBegin users) no longer calls out per byte/word. PC race: 66.7 -> 72.8 game fps (the PC trampoline has no assembly fast path, so the Switch gain is smaller). `GxGatherPipe` is now a named type shared by hw.c, gc_core.h and mmio_tramp.c (layout unchanged).
- **hw_gather_copy** (all GX natives): one memcpy and the owed write-pointer advance when linked and the stage has room, instead of a copy and commit per 32 bytes.
## Natives, batch 4 (24 new, 101 in total)
- Animation: J3DFrameCtrl::update, the atan2 table lookup FUN_80026ae0, PSMTXScale, IKCtrl::calcJointScaleMtx and LumbarCtrl::calcScaleMtx (column lengths with the game's frsqrte idiom), stVecNormalize, DriverModel::calcMPos (two-segment limb), and MSL sin / cos (fdlibm kernels and the medium-range __ieee754_rem_pio2; |x| > 2^19*pi/2 runs the original, so they are flag 1).
- J3D draw chain, all natively when the virtual targets are the known ones: J3DDrawBuffer::drawHead -> J3DMatPacket::draw -> shape packet FUN_800507a0 (with J3DVertexBuffer::setArray and syncJ3DSysFlags inline) -> shape draw FUN_80050f70 / FUN_80050ed0 -> J3DShapeMtxConcatView::load / J3DShapeMtxMultiConcatView::load (with the matrix-load cache, __ptmf_scall resolved) -> loadMtxConcatView_PNGP -> loadNrmMtx (both kinds), FUN_80067b58 / FUN_80067c64, J3DScaleNrmMtx33 -> J3DShapeDraw / GXCallDisplayList (__GXSendFlushPrim inline; __GXSetDirtyState and the material load stay guest calls).
- HUD: K2DPicture::getNewColor.
- Verifier (scripted race, MKDD_NATIVE_VERIFY_MS=4000; 500 ms never reaches the race): every new native 0 wrong, 0 float-close; the loaders and shape draw run inside drawHead / J3DMatPacket::draw, whose checks cover them and their FIFO bytes. sin / cos ~3.4 M calls each, checked 22 times.
Switch build: debug/MKDD-NX-1444.elf (genN11, 101 natives).
- PC, game thread (MKDD_PROF, alternating runs): genN10 + inline gather 16.4 / 15.4 Mcyc per game frame, genN11 15.3 / 14.5 (about 6%).
- **Hardware test of 1444: huge stretched triangles from the drivers in races (arms).** calcMPos was registered with flag 0 but uses the original's stack frame (r1 - 0xe0) as scratch for its PSVEC/PSMTX temporaries. Callers of flag-0 natives keep r1 in a host local and do not store it, so the register file's r1 was stale (higher), and the scratch overwrote the caller's live frame (calc1stJnt's vectors): the limb IK got garbage. The verifier cannot see this class of bug: it runs the original from the same stale register file. Rule (native.h): any native that reads r1 needs flag 1 (or 4). Fixed: calcMPos and getTevColorReg flag 1, both loadNrmMtx flag 4. Every other r1 user already had 1 or 4.
- Verifier options: MKDD_NATIVE_VERIFY_ONLY=name,name (substrings) and MKDD_NATIVE_VERIFY_AFTER=seconds.
Switch build: debug/MKDD-NX-1612.elf (genN12, 107 natives: batch 5 + the flag fixes).
- Build 1659 (genN12): audio jitter buffer 100 ms with fades on underrun (1612: 11 underruns in 11 min, AI production on time at 286/5 s), previous-session logs kept (*.prev; newlib rename does not replace), batch pixel format / constant dst alpha recorded in TevState pipe[3]. Yellow outline around models in races (not in Dolphin): race frames draw kart shadow volumes into EFB alpha (RGBA6_Z24; GEQUAL, alpha-only, back faces subtract / front add, blend 0x5935 / 0x5135); suspected depth-precision difference at silhouette edges, unconfirmed.
- Build 1704: 1659 + "DEBUG SHADOWS" in the - menu (session only): NORMAL / OFF (skip shadow-volume batches: Z 0xD, alpha-only blend) / BIAS NEAR / BIAS FAR (glPolygonOffset -1,-4 or 1,4 on them).

## Build 1734: crash in the GX decoder (1704 runs)
- Both 1704 crashes: data abort (translation fault) in gx.c exec() reading the first byte of a nested
  display list (GX 0x40) inside guest RAM, on the decoder thread. Not the shadow debug option.
- Cause: disc_read fread straight into g_ram. On Switch a large fread is an fs IPC receive (B) buffer;
  the kernel unmaps its whole pages from the process (KernelReadWrite|NotMapped) until the read
  returns. The decoder, running behind the game (93-102% busy), read a display list in those pages.
- Fix: disc.c reads through a private 256 KB bounce buffer and memcpy's into guest RAM.

## Build 1830 / 1857 (genN13): widescreen HUD at the screen edges
- Replaces the centred 4:3 HUD in races (menu value "EDGES"; menus, movies and other 2D stay centred 4:3).
- Natives draw__6Race2DFv / drawCourse__6Race2DFv (flag 4) wrap the original (ctx_call_orig) in a FIFO
  marker, a CP write to register 0x01 (no such hardware register): 1 = race HUD, 2 = map. Single player only
  (RaceMgr r13-0x5c38 -> +0x38 -> s16 +0x20 = console count).
- Decoder: HUD draws inside the marker get XV_HUD_LEFT / XV_HUD_RIGHT (draw centre beyond +-0.25, map always
  right unless in the left third: drawCourse also draws the top-4 rank column); XV_REC_MASK is now 0x0FFFFFFF. Vertex shader: x * (1 - sq) + side * sq * w; scissor shifted the same.
- PC check (race_batches.txt with x extents): rank column, place, item box left; lap time, map and map icons right.
- Includes the 1734 disc-read bounce buffer (decoder crash fix).

## Build 2017 (genN14): wide view mid-race, options cleanup, audio
- **Wide view only stretched the picture after a mid-race switch.** The kart camera stores its aspect at +0x168 once, in FUN_8029e0a8 (camera setup). New native FUN_8029e0a8 records each camera; aspect_refresh (called from Race2D::draw, once per race frame) rescales a camera's aspect when g_aspect_scale changes, only while +0x168 still holds the value last stored there. PC: mid-race switch moves the projection x scale 0.8781 -> 0.6586, the same as a race started wide (MKDD_WIDE / MKDD_WIDE_AT=s in pc/main_pc.c).
- **Options:** DISPLAY is 4:3 or 16:9 (wider camera, race HUD at the screen edges, menus 4:3 in the middle). 16:9 stretched, the WIDE VIEW HUD choice, the D-pad option (the D-pad always steers) and DEBUG SHADOWS are gone. settings.ini `display=0|1` (older 1 and 2 both map to 16:9).
- **Audio.** New trace counters: AI buffers sent unchanged since their last use (stale), AI periods merged into a still-pending interrupt, max AI interrupt latency; "AI late" / "deliver took" log lines. PC: 286 AI buffers/5 s, 0 merged, latency < 0.5 ms even with the game thread at 100% and 15 game fps (MKDD_SLOW_NS=n spins n ns per PSMTXConcat). Bad numbers (merged 2-26/5 s, latency 15-33 ms) only appeared while another heavy process shared the CPU, i.e. when host threads are starved.
  - Switch: the SDL audio callback thread starts on the main (render) core at the main thread's priority; heavy effect frames could delay it and the device played gaps, which no counter saw. It now raises itself to priority 0x29 (above renderer, guest threads and timer) and counts late callbacks ("late audio callbacks" in the present: line).
  - DSP mixer: main-bus volumes ramp from current (+4) to target (+2) across the 80-sample sub-frame like the ucode, instead of stepping every 2.5 ms. PC recording: large discontinuities 558 -> 441 in the same 37 s.

Switch build: debug/MKDD-NX-2017.elf (genN14).

## Build 2017 hardware test -> next build: audio mixing, stereo, decoder crash guard
- **Crash (session before the 3 races):** data abort on the GX decoder thread in draw() (gx.c), FAR 0x676d628. At
  2498.5 s the decoder logged a run of "unknown FIFO command" bytes (desync) and then built a frame of 2,057,632
  vertices from garbage draws; the frame's vertex vector grew until realloc returned NULL and the vertex writes
  faulted. The race session that followed logged one more desync (2706 s, race end) without crashing. Both desyncs
  came right before a DVD read on a race transition; no earlier build's logs show any. Root cause not yet found.
  Guard: draws past 400k vertices in one frame are dropped (real frames < 100k); the unknown-command log now says
  depth (0 = FIFO stream, 1+ = display list in guest RAM), offset and the 8 bytes before, for the next occurrence.
- **Garbage one-sided voices.** Voices with the automatic (Dolby) mixer flag (+0x58) keep other data in the mixer
  slots at +0x10. The mixer read them as bus/volume pairs anyway (bus 0x0D60 with "volumes" like 0x8E32, i.e.
  negative = phase inverted, right only) and skipped the auto mix because those sums were non-zero, and wrote
  "current = target" into them every sub-frame, which corrupted the voices further. PC race: left clipped 368
  samples (right 0). Now the auto flag selects the auto mix alone and the slots are left untouched: 0 clipped.
- **Stereo.** EXI had no devices, so the SDK read an all-zero SRAM: sound mode bit 0 = mono, and the game mixed
  every voice centred (L == R exactly). hw.c now answers EXI0 device 1 (RTC/SRAM) with a session SRAM image:
  valid checksum, flags 0x04 = stereo; SRAM writes persist for the session. PC: per-voice L/R volumes now differ.
- Not changed: the "AI late" bursts (game thread holding the token 30-75 ms) at race starts, while the game
  core is ~100% busy; late audio callbacks stayed at 0 for the whole race session.

Switch build: debug/MKDD-NX-2105.elf (genN14).

## Build 2105 hardware test -> next build: decoder speed, input lag, logging option
- **2105 test.** 2.5 races, audio good, no FIFO desync or vertex-cap lines. Game frames per 5 s:
  277-300 mid-race (game thread 94-100% busy), 248-251 in race 2's countdown (decoder 95-101%
  busy; the game thread waited 330 / 786 ms per 5 s for queue space).
- **Input lag (runtime/gx.c gx_submit).** A race frame is 115-220 KB of FIFO; the fixed 4 MB
  queue let the game run 20-35 frames ahead of the decoder, so when the decoder fell behind the
  picture lagged the controls by up to ~0.5 s. The limit is now 1.5x the moving average of bytes
  per decoded frame (128 KB..4 MB), counting the chunk the decoder holds until it finishes it.
- **Compact vertices (gx.h).** GPU-path vertices are 36 + 8k bytes (k = texgen slots the draw's
  material uses) instead of 108: pos float3, rec/mtx, colours, normal int16 x 16384, k source uv
  pairs. Each batch has one layout and starts at a multiple of its stride, so indices count in
  strides; the renderer sets attribute pointers when the layout changes (slot_layout). The CPU
  transform path keeps the 108-byte layout (GX_LAYOUT_FULL). gx_vertex_unpack for diagnostics.
- **Vertex loop (build_ops).** Per setup, a list of the format's attributes in stream order
  (absent texcoords and unneeded normals become skips) replaces the per-vertex loops over 8 texcoords
  and the per-component format branches; s16/14 normals are copied as is; per-vertex texture
  matrix indirection is rebuilt only when indices or slots change; no divisions per draw.
- **Stale per-frame caches.** Frames come back from the pool, so pointer checks (record, matrix
  slots, light block, vertex index) could match an old frame; end_frame now resets them.
- PC race (bench.sh, per game frame): decoder 15.0 -> 12.1 Mcycles; MKDD_XFCHECK 196 M vertices,
  0 mismatches. Half the vertex bytes also halves the renderer's per-frame upload copy on core 0.
- **Not done: display-list vertex cache (#1).** The game rewrites some vertex arrays in place
  (J3DDeformData::deform, vertex colour animation), so a cache needs a content hash of every
  indexed element it read; that is most of the decode cost again. Revisit if the decoder is still
  the limit on hardware.
- **Logging option (− menu LOGGING: disabled / game / game + audio / audio, default game).**
  g_log_mask (rt.h): game = mkdd.log/stderr.log lines, mkdd_trace.log and the profiler samples;
  audio = lines starting DSP / audio / AI / AX. Saved as logging= in settings.ini.
- Switch build: debug/MKDD-NX-2210.elf (genN14). The boot line now carries the right build time (main_switch.c rebuilt).

## #4 game thread at stock: cheaper guest calls (genN15, build 0722)
- 2105 hardware profile, mid-race: generated game code is ~60% of the game thread's self time, flat
  (no function above ~0.6%). PC call counts: ~37k guest function entries per game frame, a long
  tail of tiny leaves (isFinish, getters, GXSetTev*, PSMTXTrans, clamp...). Each call paid the
  entry bookkeeping, a host prologue/epilogue, and the caller reloaded every volatile local it
  uses (often 10+ loads) from c->regs afterwards.
- **Callee write sets (gen_c.py mod_analyze, MOD).** Per function, the register-file bytes a call
  can change: its own writes plus its direct callees' / tail targets' (fixed point); indirect
  calls, computed jumps, HLE'd callees, guest-calling natives, runtime intrinsics and the
  save/restore routines count as anything. After a direct call only the volatile locals in the
  callee's set are reloaded (6713 of 13496 functions have a known set). Interrupts delivered at
  entry restore the whole register file, so they change nothing. `--no-mod` turns it off.
- **Leaf inlining (INLINE, gen/inl.h).** Leaves of up to 24 instructions (no calls or outgoing
  jumps, not hooked/native/save-restore; 3505 functions) are also emitted as always-inline
  `fi_<ADDR>` bodies that direct calls use; f_<ADDR> stays for indirect calls and the runtime.
  `--no-inline` turns it off. Inlined leaves do not update g_last_fn / show in profiles.
- ctx_enter_nh no longer checks g_count_hits (compile with MKDD_COUNT_HITS for call counts).
- **Check:** `--check-mod` snapshots every local whose reload a call site skips and reports
  (mod_report) if the callee changed it in c->regs: PC race, 285,798 checked call sites, 0 reports.
- PC bench (game Mcycles/frame, alternating runs, noisy machine): 15.26 -> 14.87, 15.90 -> 14.71,
  14.06 -> 13.06: about -5% on the game thread.
- Switch build: debug/MKDD-NX-0813.elf (genN15, boot line "Oct 15 2026 07:22:19"); the 2210 .nro is
  kept as debug/MKDD-NX-2210.nro.

## Build 0722 hardware test -> build 1408: movies, heavy scenes
- **0722 test.** Races 293 game frames / 5 s on average (169 samples; 151 at 285+, 7 below 270),
  up from 277-300 in 2105. No crash, no FIFO desync.
- **Movies (present 56.6 fps, render 11-12 ms/loop with 9 batches).** Each THP frame is three I8
  planes (THPDraw.c), which were expanded to RGBA8 and uploaded at 4 bytes per texel. New
  gx_texture_decode_compact: I4/I8 -> R8, IA4/IA8 -> RG8 (row copies), texture swizzle back to
  (I,I,I,I) / (I,I,I,A); the reuse pool is keyed by size and channel count. Checked against
  gx_texture_decode on 800 random textures (all four formats, odd sizes): identical.
- **Heavy scenes (860-1000 batches: present 47-55 fps, render 7-8.5 ms/loop; the other ~12 ms of the
  loop sat in render_present, i.e. the driver waiting for the GPU).** The draw loop never flushed,
  so nouveau handed the frame to the GPU only at the swap (CPU then GPU, no overlap): glFlush every
  32 draws. Also: glScissor only when the rect changes; a batch whose TEV uniform block equals the
  previous one reuses its copy and binding.
- **Item-heavy races.** Not audio (JAudio share as in normal racing): the game thread waited 25% in
  gx_submit (queue full) and the decoder ~16% for frames, i.e. downstream of the game. The long
  "deliver took" interrupts are handlers blocking on that queue. Same fix as heavy scenes.
- **Timing line** now splits the render loop: render (gpu wait = fence of the frame NSLOT back),
  present (blit + menu, where the driver blocks on the GPU), swap wait.
- Switch build: debug/MKDD-NX-1505.elf (genN15; 1408 + the build number "BUILD 1505  OCT 15" at the bottom of the - menu; build.sh always recompiles main_switch.c so the stamp is current).
