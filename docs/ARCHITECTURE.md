# MKDD-NX architecture

MKDD-NX runs Mario Kart: Double Dash!! (USA, GM4E01) on the Switch by static recompilation: the
game's PowerPC code is translated to C ahead of time, and a small console runtime stands in for
the GameCube hardware. It is a C/GLES port of [MKDD-Recomp](https://github.com/LeftInWalmart/MKDD-Recomp)
(Rust/Bevy, PC); most runtime files say which MKDD-Recomp module they port.

No game code or data is in this repository. The generated C is built from the player's own
disc image and never committed.

## Pipeline

```
disc image ──► MKDD-Recomp sheets (CSV: p-code per instruction, functions, memory, HLE list)
                 │
                 ▼
          tools/gen_c.py ──► gen/part_NN.c + table.c   (one C function per guest function)
          tools/gen_mod.py     (merges MKDD Plus: only the functions whose C differs)
                 │
                 ▼
   runtime/ + switch/ (or pc/) ──► MKDD-NX.nro  (or the headless mkdd_pc.exe)
```

`tools/build_from_iso.py` runs all of it in one step (see the wiki's Build Instructions).

## Code generator (`tools/gen_c.py`)

Each guest function becomes a C function over a CPU context (`Ctx`, `runtime/gc_core.h`). Guest
registers live in C locals for the whole function and are written back to the context only
where something else can read them:

- **Dataflow**: a per-function analysis tracks which registers may be dirty; stores happen at
  calls, returns and slow memory paths only.
- **ABI liveness** (on by default, `--no-abi` turns it off): under the PowerPC EABI, calls skip
  storing volatile registers the callee never reads, and returns hand back only r3/r4/f1.
- **Paired singles** (Gekko's 2-wide float SIMD) compile to inline C (`runtime/ps.c` for the rest).
- **Memory**: every guest load/store has an inline MEM1 fast path; anything else (hardware
  registers, the gather pipe, locked cache) calls a trampoline in `runtime/mmio_tramp.c`.
- **Natives**: calls to the 113 functions listed in `runtime/native.h` (matrix maths, J3D
  animation, collision, HUD) become direct calls to hand-written C in `runtime/native.c`.
- **Hooks**: functions in the sheets' HLE list check `core_hooked()` on entry and are routed to
  `runtime/hle.c` (OS, DVD, audio and GX SDK calls).

Generated code is disposable: change the sheets or the generator and regenerate.

## Runtime (`runtime/`)

| file | role |
|---|---|
| `machine.c` | MEM1/ARAM, boot, the hooks `gc_core.h` calls |
| `core.c` | dispatch map for indirect calls, hook table |
| `sched.c` | guest threads (one host thread each, one runs at a time), interrupts, alarms, time |
| `hle.c` | function-level emulation of SDK calls |
| `hw.c` | hardware registers outside MEM1 |
| `gx.c`, `gx.h` | GX command-stream decoder: FIFO and display lists → batched vertices and pipeline state, on its own thread |
| `dsp.c` | high-level emulation of the JAudio DSP microcode |
| `thp.c` | THP movie frame decoder |
| `disc.c`, `rvz.c` | disc images (.iso/.gcm, .ciso, .rvz), with a read-ahead thread |
| `memcard.c` | memory card at the CARD API level; saves are standard `.gci` files |
| `native.c` | native functions and their verifier |
| `diag.c` | log (each distinct line once, repeats counted) and watchdog trace |
| `plat.c` | threads, locks, time, files (PC and Switch) |

## Switch front end (`switch/`)

| file | role |
|---|---|
| `main_switch.c` | SD folders, logs, crash handler, disc lookup, boot, render loop, input, audio, the stutter log |
| `render_gl.c` | GLES 3.2 renderer: replays each decoded GX frame on an emulated EFB, EFB copies as GL textures, 4:3/16:9 output |
| `xf_vs.h` | the XF stage (transform, lighting, texgen) as a vertex shader |
| `tev_gen.c` | TEV fragment shaders specialised per material (an uber shader draws while they compile) |
| `menu.c`, `ui_font.c` | the − menu, drawn with the game's own lettering from the disc |
| `pause_art.c` | Switch controller art and button badges on the pause and player-count screens |
| `hdtex.c` | HD texture replacement (packs converted by `tools/hdtex_pack.py`) |
| `update.c` | in-game updater (GitHub release, size + SHA-256 + NRO header checks) |
| `prof_switch.c` | sampling profiler, reported by `tools/prof_report.py` |

Threads on the Switch: core 0 renders, core 1 runs the game (guest threads), core 2 runs the GX
decoder, the timer and the profiler. The audio callback runs above the renderer's priority.

## Widescreen

16:9 widens the game's projection (`g_aspect_scale`) instead of stretching the picture. 2D
draws are flagged by the decoder and squeezed back to 4:3 in `xf_vs.h` (`u_squeeze`), with
HUD elements pushed to the screen edges. Draws that span the whole screen, and the
`WipeManager` fades (zone 3 in `native.c`), stay full width.

## Testing

There is no unit-test suite: correctness is checked against the recompiled original code on
the PC build, which runs the full game headless.

**PC harness** (`pc/`): `pc/build_pc.sh <gen dir>` builds `mkdd_pc.exe`;
`mkdd_pc <disc> [seconds] [pad script]` boots the game with no window and replays a scripted
controller (see `pc/main_pc.c`). A normal check is to reach a race with 0 unresolved calls.

| check | how | what it proves |
|---|---|---|
| Natives | generate with `--native-verify`, run `pc/verify.sh` | sampled native calls are also run through the recompiled original from the same state; results and memory are compared |
| ABI liveness | generate with `--abi-check` | skipped register stores are filled with a poison value; any read of one is reported |
| Register assumptions | `--check-lazy`, `--check-mod` | callee-saved registers come back unchanged; registers not reloaded after a call are unchanged |
| Vertex transform | `MKDD_XFCHECK=1` | the GPU path's XF maths (`gx_xf_eval`, mirrored by `xf_vs.h`) against the CPU path, per vertex |
| Vertex decoder | `MKDD_VTXCHECK=1`, `MKDD_VCACHECK=1` | the fast vertex loops and the vertex cache against the general loop |
| Scene watchdog | `MKDD_TEST_LOST_LOAD=1` | injects a lost scene load on the title screen; the watchdog must recover |
| Shaders | `MKDD_DUMP_TEV=<dir>` | writes every specialised TEV shader for offline checks with Mesa's compiler (devkitPro `uam`) |

`pc/bench.sh` and `pc/verify.sh` race with a scripted controller on the disc image named by
`MKDD_DISC`. `pc/bench.sh` measures throughput (guest time runs 4× fast with `MKDD_TIMESCALE`);
`pc/prof_win.c` + `pc/prof_report.py` profile it.

### Debug environment variables (PC unless noted)

| variable | effect |
|---|---|
| `MKDD_MOD=<folder>` | play a mod (e.g. MKDD Plus) from the folder, with its own saves |
| `MKDD_WIDE=1`, `MKDD_WIDE_AT=<s>` | 16:9 from the start / from that second |
| `MKDD_DUMP_FRAME=<s>` | the frame at that second as text (`frame.txt`): every batch's pipeline, textures, a few transformed vertices |
| `MKDD_PROF=<s>` | sample all threads from that second, `prof.txt` at exit |
| `MKDD_TIMESCALE=<n>` | guest time runs n× fast (benchmarks) |
| `MKDD_SLOW_NS=<n>` | spin n ns per matrix multiply, to load the game thread like the Switch's CPU |
| `MKDD_NATIVE_VERIFY=1`, `_MS`, `_ONLY=a,b`, `_AFTER` | native verifier: on, gap between checks, which natives, start time |
| `MKDD_CPU_XFORM=1` | vertex transform on the CPU (Switch: a `cpu_xform` file in the config folder) |
| `MKDD_NOVCACHE=1` | vertex cache off |
| `MKDD_SYNC_DRAWDONE=1` | GXDrawDone waits for the decoder instead of letting it trail |
| `MKDD_DSP_ASYNC=1` | DSP mixing on a worker thread (off by default) |

## Switch performance notes

What mattered most on the Switch (1 GHz Cortex-A57, Mesa nouveau):

- The GX decoder on its own core, per-draw setup cached until the state changes, and vertex
  transform on the GPU.
- Streaming vertex/index/uniform data into persistent buffers: replacing a GL buffer per draw
  makes nouveau wait for the GPU.
- Specialised TEV shaders compiled a few per frame, with the uber shader in the meantime.
- Natives for the hottest engine functions, and the generator's register work above.
- Logs buffered in memory: SD card writes can stall for a quarter of a second.

With logging on, `logs/mkdd.log` gets a `hitch:` line for every dropped frame, saying where the
time went (texture uploads, shader compiles, game and GX busy time, disc reads, GPU drains).
