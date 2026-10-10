# mupen64plus-rdp-foir

A fast, low-level (LLE) **software RDP** video plugin for Mupen64Plus, built for
aarch64 retro handhelds. foir favours **speed over accuracy**: it renders the
N64's RDP command stream on the CPU with NEON SIMD kernels, and it takes
"good enough" shortcuts wherever they are hard to see.

> **Status:** pre-alpha, **M3 (static pipeline specialisation) implemented** on top of M2 (NEON + threads + fast VI blit).
> On a Rockchip RK3566 (4x Cortex-A55) the renderer is **1.25-1.65x faster than at the end of M2 on one thread and 1.2-1.5x faster with
> three worker threads** (replay of captured frames), i.e. 3.3-7x / 7-19x faster than the M1 scalar reference; pre-compiled variants of
> every pixel stage handle the render states real games use (281 variants at M3, 492 kernels with the shape and "any format" tiers), the generic NEON kernels handle everything else. In game
> (rsp-hle + parallel-rsp) Star Fox 64, Donkey Kong 64 and Banjo-Kazooie run at 60 fps with 12-32 % less CPU per frame than at M2, Super
> Mario 64 at 55-60 fps (RSP-bound or capped) and **Pokemon Snap goes from 30 to 40 fps** with the vectorised parallel-rsp build
> ([results](#measured-results), [RSP pairing](#rsp-pairing)). The optimised paths, specialised or not, are bit-exact with the scalar
> reference with exact knobs on every test scene and in the in-game checks, and the scalar reference is bit-exact with
> angrylion-rdp-plus on the synthetic scenes and on the real frames checked (Kirby 64: 25 px).
> The exact variant lists were made from 23 ROMs; the colour combiner of a game outside them runs in an exact variant for only about 70 % of its
> pixels ([coverage](#specialisation-coverage)). **Shape kernels** (147 combiner kernels that fix only the structure of the equations and take their operands from
> a per-draw table, see [below](#shape-kernels-combiner)) cover about 90 % (estimated out of sample) and make the replay of such games 7-21 % faster; they cost 1.15-1.65x
> an exact variant, so the exact variants stay first in the dispatch.
> **Three cheap wins after M3** ([results](#cheap-wins-after-m3)): COPY-cycle texture rectangles run in NEON (they were scalar: 15 % of Paper Mario's foir CPU time in game);
> the colour / Z buffer access is inlined, checks the RDRAM bounds once per batch and prefetches (3-7 % on the 3D replays); the texture unit has a second tier of
> 38 "any format" kernels behind the exact variants and the Z compare is compiled for every combination of its inputs, which takes the share of pixels that run in a
> specialised variant from 91.5 % to 100 % (texture) and 93.5 % to 100 % (Z compare) over 62 games, and from 91.7 % to 99.9 % / 97.1 % to 100 % out of sample (leave-one-game-out).
> **The mip-map (LOD) texture path after the cheap wins** ([results](#lod--mip-map-texture-path)): in Banjo-Kazooie and Donkey Kong 64, 45-62 % of the LOD batches ran in the generic kernel because the variants checked two tiles too many; that is fixed, the LOD is computed once per batch with
> the neighbouring pixel's divide reused, and the texture cycle takes a one-tile fast path: **replay -13 to -15 % (Banjo-Kazooie), -10 to -13 % (Donkey Kong 64)**, bit-exact.
> Not implemented yet: VI filters, a few rarely used RDP modes in NEON (see [known gaps](#known-gaps)).

---

## Goals

- **Full speed on Cortex-A53 and A55 class SoCs**, such as Allwinner H700 / A133,
  Rockchip RK3326 / RK3566, and similar 4-core handheld chips.
- **LLE rendering.** foir interprets real RDP commands rather than
  high-level display lists, so it avoids per-microcode HLE hacks.
- **NEON first.** Hot paths are hand-written NEON kernels. There is also a scalar C
  path for portability and testing.
- **Configurable accuracy.** Each shortcut is a toggle, so a game that breaks can
  get back the exact behaviour it needs.

## Non-goals

- Cycle- or pixel-exact output. [angrylion-rdp-plus] is the accuracy reference
  foir is tested against, not the target it tries to match.
- A GPU rendering path, for now. (If one is added later, the "mini-uber"
  approach described below would carry over as a small set of generic shaders
  that fill in while specialized ones compile.)
- Upscaling or enhancement features.

---

## How it fits into Mupen64Plus

Mupen64Plus has no separate "RDP plugin" type. foir is a **video plugin**
(`M64PLUGIN_GFX`) that does its real work in `ProcessRDPList()`
(`mupen64plus-core/src/api/m64p_plugin.h`). Any **LLE RSP** plugin calls that
function whenever the RSP hands the RDP a command list. foir reads that list
directly from RDRAM or DMEM, so no conversion layer is needed.

### RSP pairing

| Setup | Status on the RK3566 | Notes |
|---|---|---|
| **rsp-hle + parallel-rsp fallback** *(recommended)* | **Tested, works, fastest.** All 23 test ROMs run for 55-58 s without hang or crash; screenshots are bit-identical to the cxd4 runs (see below). | rsp-hle runs audio lists in HLE (fast), `DisplayListToGraphicsPlugin = False` sends graphics tasks to the fallback, which runs them in LLE with parallel-rsp's Lightning JIT. |
| **parallel-rsp as the RSP plugin** | **Tested, works.** Same screenshots, and the audio ucode output is bit-identical to cxd4 (7 games). | Runs audio in LLE too (a little slower than the HLE audio of the pairing above, bit exact). |
| **cxd4** (`DisplayListToGraphicsPlugin = False`) | **Tested, works.** The numbers in the first M2 tables use it. | An interpreter whose vector unit is SIMD only on x86. Slow: with the RDP on worker threads the RSP + CPU emulation, not the RDP, sets the frame time in heavy scenes. |

Example `mupen64plus.cfg` snippet (the plugin file is `mupen64plus-rsp-parallel.so`, built from the sibling `parallel-rsp` repo):

```ini
[Rsp-HLE]
DisplayListToGraphicsPlugin = False
AudioListToAudioPlugin = False
RspFallback = "/usr/lib/mupen64plus/mupen64plus-rsp-parallel.so"

[Video-Foir]
Threads = 0          ; 0 = auto (cores - 1), -1 = render on the emulation thread
```

Command line used for the tests (private config dir, stock Mupen64Plus 2.5.9 core and stock `mupen64plus-rsp-hle` 2.5.9; the sibling repo's rsp-hle 2.6.0
built for aarch64 behaves the same):

```sh
mupen64plus --configdir CFG --plugindir PLUGINS --gfx mupen64plus-rdp-foir.so \
  --rsp mupen64plus-rsp-hle.so --set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False \
  --set rsp-hle[RspFallback]=PLUGINS/mupen64plus-rsp-parallel.so ROM.z64
# parallel-rsp alone:  --rsp PLUGINS/mupen64plus-rsp-parallel.so
```

**Desktop (x86_64) notes.** With the stock Mupen64Plus core's *legacy* x86_64 dynarec, Indiana Jones and the Infernal Machine, Star Wars Episode I - Racer,
Excitebike 64, Iggy's Reckin' Balls (and Pilotwings 64, whose dynarec grows to many GB) never get past the IPL3 RDRAM current-control calibration and send no RDP
lists at all. `--emumode 1`, or a core built with `NEW_DYNAREC=1` (what the aarch64 devices use), is fine. The core fix is in `mupen64plus-core`
(`src/device/r4300/r4300_core.c`, `recomp.fast_memory` starts at 0). Donald Duck - Goin' Quackers and Excitebike 64 wait for a button press at their
"no Controller Pak" screens, so with the dummy input plugin they stop on a static screen (zero new RDP lists per 120 frames is normal there).

Building the aarch64 plugin (cross build on x86_64; add `-DM64P_API_INCLUDE_DIR=` if the core checkout is not next to `parallel-rsp`):

```sh
cd parallel-rsp && ./build_m64p_plugin.sh aarch64        # -> build-m64p-aarch64/mupen64plus-rsp-parallel.so
# or by hand
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64-linux-gnu.cmake -DCMAKE_BUILD_TYPE=Release \
  -DPARALLEL_RSP_M64P_PLUGIN=ON -DPARALLEL_RSP_DEBUG_JIT=OFF -DPARALLEL_RSP_TESTS=OFF
```

**Why M2 saw SM64 spin in `RSP::JIT::CPU::get_jit_block`:** the stock `parallel-rsp` CMake target is the test-harness library
(`rsp-runner`). It lacks `PARALLEL_INTEGRATION`, so an SP DMA write makes the JIT return `MODE_DMA_READ` to its caller instead of
copying the data (and `ProcessRdpList` / the SP status polling logic are compiled out), and nothing in the plugin shell handled that mode.
The ucode never got its code or data, polled forever and the emulation thread looped in the JIT dispatcher (reproduced on the rig by rebuilding the
plugin without `PARALLEL_INTEGRATION`: SM64 stops after three `UpdateScreen` calls and the main thread sits in RSP JIT code). The new
`PARALLEL_RSP_M64P_PLUGIN` target builds the plugin with `PARALLEL_INTEGRATION` and without the debug JIT (details in the `parallel-rsp` repo).

**Verification** (RK3566, `Core[RandomizeInterrupt]=False` so runs are deterministic, `Threads=3`, 23 ROMs, 58 s each, screenshots at VI frames
300 / 900 / 1500 / 2100 / 2700 when reached): 98 screenshots reached in all three setups (the rest were not reached in 58 s by the slower games), all **bit-identical** between cxd4,
parallel-rsp standalone and rsp-hle + parallel-rsp (`test-output/prsp/screenshots_vs_cxd4.txt`; contact sheets in
`test-output/prsp/`). Audio output of 7 games (SM64, Banjo-Kazooie, Zelda OoT, Mario Kart 64, Star Fox 64, Jet Force Gemini, Yoshi's Story, first ~40 s):
parallel-rsp standalone is **sample-identical** to cxd4 (`test-output/prsp/audio_vs_cxd4.txt`); rsp-hle's HLE audio differs
from LLE by a few LSB to a few hundred (expected, HLE mixing is not bit exact; compared numerically only, not listened to).

**Frame rate and RSP time** (foir `Threads=3`, default knobs, same windows as the table below, mean of 2 runs; `test-output/prsp/fps_rsp_time.md`).
"RSP" is the emulation-thread time inside the RSP plugin's `DoRspCycles` per frame; it contains foir's RDP command parsing / setup
(the RSP triggers `ProcessRdpList`), shown as "net" without it:

| game (window end) | cxd4 | parallel-rsp standalone | rsp-hle + parallel-rsp fallback |
|---|---|---|---|
| Super Mario 64 (600) | 28.0 ms / **36 fps**; RSP 24.6 (net 22.5) ms | 19.7 ms / **51 fps**; RSP 16.4 (net 14.1) ms | 17.8 ms / **56 fps**; RSP 14.4 (net 12.1) ms |
| Star Fox 64 (1080) | 16.6 ms / **60 fps**; RSP 11.5 (6.4) ms | 16.6 ms / **60 fps**; RSP 9.7 (4.4) ms | 16.6 ms / **60 fps**; RSP 7.9 (2.0) ms |
| Donkey Kong 64 (1080) | 16.7 ms / **60 fps**; RSP 7.8 (5.7) ms | 16.7 ms / **60 fps**; RSP 7.1 (3.9) ms | 16.7 ms / **60 fps**; RSP 6.2 (2.8) ms |
| Banjo-Kazooie (600) | 16.7 ms / **60 fps**; RSP 11.6 (5.6) ms | 16.6 ms / **60 fps**; RSP 11.4 (4.3) ms | 16.6 ms / **60 fps**; RSP 10.9 (3.6) ms |
| Pokemon Snap (600) | 43.8 ms / **23 fps**; RSP 40.8 (32.8) ms | 34.0 ms / **29 fps**; RSP 30.8 (22.1) ms | 32.5 ms / **31 fps**; RSP 29.3 (20.3) ms |

The JIT gives 1.3-1.7x less RSP time than cxd4 on the heavy scenes, not an order of magnitude: parallel-rsp's JIT calls a C function
for every vector / vector-load-store instruction (SSE2NEON-translated intrinsics on aarch64); in a `perf` profile of SM64 (hle +
parallel-rsp, emulation thread) 46 % of the samples are in those helper functions and about a further third in JIT-generated code
(RSP blocks and the R4300 dynarec). SM64 and Pokemon Snap
are therefore still RSP-bound (SM64: 12 ms of its 17.8 ms frame). Inlining the vector ops in the JIT (or a native NEON rewrite of the
load/store helpers) was the next lever, and is what the next paragraph measures.

**Faster parallel-rsp JIT (aarch64)**. The parallel-rsp JIT now emits vector ops, vector loads / stores, the COP2 / most COP0 moves and the block
dispatch inline instead of calling a C helper per instruction, packs its code regions (the old layout was 6x padding) and copies the SP DMA
with `memcpy` ([`parallel-rsp/README-m64p.md`](../parallel-rsp/README-m64p.md)). Output is bit-identical to the previous build: for all 23 ROMs, rsp-hle +
fallback, `Core[RandomizeInterrupt]=False`, the screenshots at VI frames 300 / 900 / 1500 / 2100 / 2700 (all frames both builds reached) and the stream of
DMEM+IMEM hashes taken after every `DoRspCycles` call (2 000-5 300 calls per ROM, `tools/prsp/prsp_cmp.sh`) are identical; the LLE audio
streams of the standalone plugin for the 7 audio games are identical (`tools/prsp/prsp_cmp_audio.sh`, common prefix 3-4.8 MB each). Same windows,
foir `Threads=3`, old and new `.so` alternated in the same sessions, mean of 2 runs (the rig is shared, so absolute numbers differ a little from the table
above; the pinned foir build is the same for old and new). "net" is RSP time without foir's RDP parse / wait:

| game (window end) | rsp-hle + fallback, before | rsp-hle + fallback, after | standalone, before | standalone, after |
|---|---|---|---|---|
| Super Mario 64 (600) | 17.7 ms / **57 fps**; RSP 14.2 (net 11.9) ms | 16.7 ms / **60 fps**; RSP 8.3 (net 5.9) ms | 19.8 ms / **51 fps**; RSP 16.4 (net 14.1) ms | 16.6 ms / **60 fps**; RSP 9.3 (net 6.9) ms |
| Star Fox 64 (1080) | 16.6 ms / 60 fps; RSP 8.2 (net 1.9) ms | 16.6 ms / 60 fps; RSP 7.6 (net 1.4) ms | 16.6 ms / 60 fps; RSP 10.0 (net 4.3) ms | 16.7 ms / 60 fps; RSP 8.4 (net 2.4) ms |
| Donkey Kong 64 (1080) | 16.7 ms / 60 fps; RSP 6.5 (net 2.8) ms | 16.7 ms / 60 fps; RSP 6.3 (net 1.4) ms | 16.7 ms / 60 fps; RSP 7.3 (net 3.6) ms | 16.7 ms / 60 fps; RSP 6.8 (net 1.8) ms |
| Banjo-Kazooie (600) | 16.6 ms / 60 fps; RSP 11.5 (net 3.5) ms | 16.5 ms / 60 fps; RSP 11.2 (net 1.6) ms | 16.6 ms / 60 fps; RSP 12.1 (net 4.3) ms | 16.6 ms / 60 fps; RSP 11.4 (net 1.9) ms |
| Pokemon Snap (600) | 34.5 ms / 29 fps; RSP 31.1 (net 20.5) ms | 34.1 ms / 29 fps; RSP 30.9 (net 10.3) ms | 35.9 ms / 28 fps; RSP 32.5 (net 22.0) ms | 34.3 ms / 29 fps; RSP 31.1 (net 10.9) ms |

SM64 now runs at the 60 fps cap in both setups (RSP time -42 % with the fallback, -43 % standalone). The RSP compute itself ("net") is about halved in
Snap too, but Snap does not get faster: the emulation thread now waits 10.9 ms per frame at SYNC_FULL for the three RDP workers (it was 4 ms), so
Snap is RDP-bound and its frame time has to come from the RDP side. For Banjo / DK64 / Star Fox the "RSP" figure is mostly that same wait. Not yet
looked at: stock rsp-hle's own cost in the pairing (about 4 % of the emulation thread in the SM64 profile, probably its per-task ucode checksum).

---

## Architecture

```
 ProcessRDPList()                              emulation thread ("parser")
       |
       v
 +--------------+   +-----------------+   +-------------------------------+
 | Command      |-->| Primitive setup |-->| Job: walker record (edge and  |
 | parser       |   | (decode edge /  |   | interpolant coefficients, one |
 | (state, TMEM)|   |  interpolant    |   | small struct) + constants +   |
 +--------------+   |  coefficients)  |   | (if changed) render-state and |
                    +-----------------+   | TMEM snapshots                |
                                          +-------------------------------+
                                                         |  lock-free ring
                  +--------------------------------------+------------------+
                  v                      v                                  v
            worker 0               worker 1                           worker N-1
   owns rows y with (y/4) % N == id  (its own FB / Z / hidden-bit rows)
                  |
                  v
   per row it owns: edge walker (4 subscanlines, NEON) -> span -> pixel batches (64 px, struct-of-arrays, several scanlines)
        texture -> combine -> zcmp -> blend -> store      (NEON kernels, 8 px per step)
```

### Pixel pipeline

Pixels of one primitive are collected into **batches** of up to 64 lanes in
struct-of-arrays form (`struct px_batch`, `src/rdp.h`), possibly spanning several
consecutive scanlines of the primitive, and run through the stage kernels of
`struct foir_kernels`: `texture`, `combine`, `zcmp`, `blend`, `store` (plus `fill` and
`copy` for the FILL / COPY cycle types). The kernel table is the swap-in point: the scalar C11
set (`kernels_scalar.c`, `kernels_tex.c`) is the reference, the NEON set replaces entries one by one
and falls back to the scalar kernel *per batch* for configurations it does not handle.
Every stage works in 16-bit lanes (32-bit only where a product needs the headroom).

| Stage | NEON (`kneon_*.c`) handles | Scalar fallback for |
|---|---|---|
| texture | every non-YUV format (RGBA 4/8/16/32, IA, I, CI), TLUT, point / bilinear / mid-texel, perspective divide, shift / clamp / mask / mirror, **LOD, detail and sharpen**, 1- and 2-cycle, next-pixel TEXEL1, the BI_LERP = 0 colour-conversion on point samples | YUV tiles, the conversion arithmetic on filtered or TLUT texels, `convert_one` |
| combine | 1- and 2-cycle, all inputs, dither (matrix / random), noise, shade coverage correction, alpha / coverage selects | chroma key |
| zcmp | 16- and 32-bit colour buffers, every Z mode, no lookup tables (the Z decode / encode and delta-Z comparator are computed), opaque / transparent fast path | 4- / 8-bit colour buffers |
| blend | both cycles, all selectors, the exact division table, alpha compare (also dithered), RGB dither | -- |
| store | 16- and 32-bit colour buffers, Z update, coverage / hidden bits | 4- / 8-bit colour buffers |
| fill | 16- and 32-bit rows of 16+ pixels | other sizes |
| copy | texture rectangles into 16-bit colour buffers: any 16-bit texel, CI8 / CI4 / 16-bit indices through the TLUT, every other non-YUV texel format through the texture decoder; alpha compare; eight pixels per step ([below](#copy-cycle-rectangles)) | 8- / 4-bit colour buffers, triangles drawn in COPY mode, YUV texels without TLUT, spans that start left of x = 0, are 1024 pixels long or lie within 16 pixels of the end of RDRAM |

Details that keep the vector code exact: combiner inputs A, B, D use the RDP's
quirky 9-bit extension (`0x100..0x17f` stay positive), the C input is a true
signed 9-bit value; equations are classified per batch (`D`, `A*C`, `(A-B)*C`, `A*C+D`, full) so zero operands cost
nothing; the 16-bit framebuffer is read / written with halfword-pair reversal (RDRAM is held as host
words) and per-pixel "hidden" bytes for coverage / delta-Z.

The pseudo random numbers behind noise, random dither and dithered alpha compare are a hash of the
framebuffer index and a per-primitive seed, so the result does not depend on batching or on which thread
renders a pixel (the hardware LFSR sequence is not reproduced; angrylion's sequence is not either, since it
depends on pixel order).

#### COPY-cycle rectangles

Games draw backgrounds, sprites and (Hydro Thunder, Mischief Makers) whole screens with COPY-cycle texture rectangles: no combiner, no blender,
no Z; texels are moved to the colour buffer. The scalar reference (`scalar_copy`) fetches and writes one pixel at a time through the generic TMEM
readback and `rdram_wr_pair16`, about 50 ns per pixel on the A55, and used to be the only scalar stage that mattered in game (15 % of the foir CPU
time of Paper Mario, 9 % of Rocket in the in-game profile). **What ran through it** (statistics of the 184 gameplay dumps of 62 games, 128 M copy pixels):
CI8 through the TLUT with alpha compare 39 %, RGBA16 with alpha compare 24 %, CI8 through the TLUT without alpha compare 16.5 %, RGBA16 without 10.8 %, CI4 through the TLUT with alpha compare 9.7 %,
I4 without TLUT (one game) < 0.01 %; all into 16-bit colour buffers, all texture rectangles (`flip = 1`), 30-250 pixels per span (Paper Mario 148, Mischief Makers 31).
`neon_copy` was a stub that handed every span to the scalar kernel, so the answer to "which cases fall back" was *all of them*.

`neon_copy` (`src/kneon_tex.c`) now takes every texture rectangle into a 16-bit colour buffer. The scalar kernel works in groups of four pixels (the texture
coordinates of the group's first pixel are divided, clamped and shifted once; pixel q of the group reads the texel at s + q, the tile's mask applies per texel, no clamp or mirror;
the texel is the raw RGBA5551 word, or the TLUT entry of an 8 / 4-bit index; with alpha compare a clear low bit skips the pixel; pixels past the row end are dropped), and the vector kernel does the same
in two phases. Phase 1 computes the (s, t) start of every group of the span, four groups per vector. Phase 2 walks the span in windows of eight pixels that start at an even framebuffer
pixel (one vector load / store of the byte-swapped RDRAM; an odd-aligned span starts with a half-used window), expands the group values to the lanes with a table lookup (`vqtbl1q_u8`), runs the address arithmetic in
16-bit lanes (only the low 11-12 bits of a TMEM address matter and every operation on the way is exact modulo 2^16), gathers the texels (8-bit index, then the palette entry; 4-bit: the nibble is selected before the palette lookup) and stores
the window with a lane mask (no read-modify-write when all eight pixels are written; a window that is not inside the row, because the span touches the first or last pixels, is written per pixel: the neighbouring rows may belong to
another worker). Other texel formats (I4, IA, 8-bit and 32-bit texels without TLUT) go through the texture unit's own decoder (`fetch_taps`) and are packed as the scalar kernel does ("best effort" RGBA5551 from the filtered colour).
What still takes the scalar kernel: 8- and 4-bit colour buffers, triangles drawn in COPY mode (`flip = 0`; no game in the corpus), YUV texels without TLUT, spans that start left of x = 0 or are 1024 or more pixels long, rows
within 16 pixels of the end of RDRAM. Not vectorised further: the texel and palette gathers are eight scalar loads each (a contiguous-texel fast path and a TLUT table lookup for CI4 were not tried).

#### Colour / Z buffer access

The `zcmp` and `store` stages read and write the colour buffer, the Z buffer and the per-pixel hidden bytes in groups of eight lanes. A group whose lanes have consecutive pixel indices is one vector window (a load,
or a load-modify-write with the lane mask; an odd-aligned window rewrites the 16-pixel frame it overlaps, which has to be inside one row because another worker may own the neighbouring rows). Every other group takes a per-lane scalar path
(`rdram_rd_pair16` / `rdram_wr_pair16`). **How many groups that is** (statistics of 60-frame gameplay windows): 79 % (Super Mario 64), 86 % (Donkey Kong 64), 79 % (Banjo-Kazooie), 88 % (Star Fox 64), 91 % (Turok), 54 % (Paper Mario) of the
groups are eight contiguous pixels; 17.7 / 12.3 / 17.2 / 10.7 / 8.6 / 30.5 % straddle two scanlines of the batch (exactly one run boundary, i.e. two runs), 1-15 % have three or more runs or are the short tail of a batch
(a batch holds consecutive scanline runs of a primitive, and a run rarely ends on a multiple of eight; the "about half of the groups" of the M3 notes was measured on the short first-minute frames). In the in-game profile `ld16` / `st16` were 13 % of foir's CPU time on average (17 % at most).

What was measured (Cortex-A55, `foir-replay`, Super Mario 64 / Donkey Kong 64 / Banjo-Kazooie / Paper Mario / Star Fox 64 / Turok windows, one thread, alternating builds):

| variant | result |
|---|---|
| **M3 code** (out-of-line `ld16` / `st16`, bounds check per window) | reference |
| access functions inlined into the kernels, bounds checks hoisted from the window to the batch (lowest / highest pixel index, kept by the batch builder), the odd-window row test unchanged, full-mask windows stored without the read | **-3 to -5 %** (the straddling groups still scalar) |
| + prefetch of the colour / Z / hidden lines of every run at batch-build time (`PRFM PSTL1KEEP`) | another -0.5 to -2 % |
| + straddling groups split into their runs, each run read / written with a vector window positioned so that its first lane lines up (generic loop over the runs, merged by lane masks) | **+3 % slower** than the line above (+2 % instructions: about 40 instructions more per access than the scalar lane loop) |
| + the same restricted to groups with exactly one run boundary, branch-free windows, bounds checks hoisted | -1.8 % instructions, **no change in cycles** (Cortex-A55 is in order: ld/rev/ext/bsl chains and 20 instructions of window set-up per run eat the saving; within noise on 7 dumps, 0.2 % slower on average) |
| rough upper bound: treat every straddling group as contiguous (wrong output, so the Z results and the number of writes differ, otherwise the same work) | -3.3 % (SM64), -6 % (DK64) of the time: that is all there was to gain from straddling groups |

So the shipped version keeps the scalar path for straddling groups and gets its gain from removing the call overhead, the per-window checks and the cache misses of the first touch. The restructure that was *not* tried: padding every scanline run of a batch to a multiple of
eight lanes (dead lanes cost the whole texture / combiner / blender pipeline, about 400 instructions per pixel, against 70 saved per straddling access).

### Threading

* The parser thread decodes commands, keeps TMEM, and runs primitive setup, which only *decodes* the primitive: the edge
  positions and slopes and the interpolant values and steps become a **walker record** (`struct prim_walk`, 192 bytes,
  `rdp.h`). Each primitive becomes a **job** (that record, the per-primitive constants and indices of the state /
  TMEM snapshots it needs). Snapshots (about 1.1 KB of render state, 4 KB of TMEM) are taken only when
  something changed since the previous job.
* **The edge walker runs on the workers, one row at a time** (`walk_row`, `src/walk.h`). The RDP walks a primitive in subscanlines
  (four per row) and everything it carries from one subscanline to the next is linear in the subscanline number (edge
  positions, interpolants; the left edge restarts from the lower edge at the middle vertex), while the per-row state (min / max
  edge, the all-over / all-under / all-invalid flags) is reset at every row. The span of row `j` is therefore a function of `j` and
  the record: the edge position of subscanline `k` is `start + (k - k0) * step` and the interpolants of row `j` are `a0 + (j - j0) * step`,
  in wrapping 32-bit arithmetic -- exactly what `k - k0` repeated additions produce, since unsigned addition and multiplication
  are ring operations modulo 2^32 (`foir-walkfuzz` checks it against the sequential walker, below). A worker computes the span of
  each row it owns (the four subscanlines in the four lanes of a NEON vector; a portable version for the scalar build), hands it
  to the batch builder and forgets it: no span records are written, queued or read by another core. The scalar and the inline
  (`Threads = -1`, `foir-replay -t 0`) paths use the same `walk_row`.
* Workers run the whole pixel pipeline for the **rows they own**: row `y` belongs to worker
  `(y >> 2) % N` (4-row stripes). A worker is a private `struct rdp` context whose render-state block is
  refreshed from the snapshot, so the kernels read `r->om`, `r->tile[]`, ... unchanged. Workers share nothing
  in the hot loop: framebuffer, Z and hidden-bit rows are private to their owner and no lock is taken per
  pixel or per primitive. (Pixels past the end of a row are dropped instead of wrapping into the next row,
  which another worker owns; vector stores that would rewrite pixels outside their row fall back to
  per-lane stores. ThreadSanitizer is clean on the test frames.)
* All workers are drained only when the parser needs a consistent RDRAM: `SYNC_FULL`, VI scan-out /
  `ReadScreen2` / `FBRead` / `FBWrite`, a texture load whose source overlaps memory queued primitives still
  write (texture-from-framebuffer), a change of render-target geometry that would break row ownership of memory
  still being written, and shutdown. Idle workers spin for 400 us, then sleep on a condition variable.
* `Threads` = 0 picks `cores - 1` workers (3 on a 4-core chip); `-1` renders on the emulation thread
  (the M1 behaviour, useful for debugging). The environment variable `FOIR_THREADS` overrides the setting.

### VI output

* foir emulates no VI filters. It crops the active framebuffer region and converts it to RGBA8888 with NEON
  (about 0.2 ms for 320x237), uploads it as **one texture** through the core's video extension (GLES2;
  about 0.7 ms including draw and swap on the RK3566), and scales it to the window.
* `InterlaceHack`: for interlaced output of frames the game rendered one field at a time (`SET_SCISSOR` field
  bits) only the freshest field is scanned out, line doubled, instead of weaving it with the previous field.
* A front-end or plugin that overrides VidExt, such as [mupen64plus-input-suxty](../mupen64plus-input-suxty), can draw its
  overlay on top of that texture.

### Specialised pixel kernels (M3)

The generic NEON kernels read the render state at run time: combiner operand tables, equation shapes, texel-format and blender
switches, a handful of branches per 8-pixel group. Games use only a few distinct states, so every pixel stage also has
**precompiled variants** with that state folded into the code at build time. Nothing is generated at run time (a JIT was
evaluated and dropped, see the [feasibility checkpoint](#feasibility-checkpoint)).

* **Stage keys.** Whenever the render state changes (and when a primitive samples a different tile), `foir_spec_select()`
  (`src/spec.c`) derives a 32-byte key per stage from the state of that context (the parser, or each worker for its own
  snapshot):

  | stage | key | generic kernel only for |
  |---|---|---|
  | texture | cycles, which texels the combiner reads, format and size of the tile (and of tile + 1), TLUT and its type, point / bilinear / mid-texel, perspective, BI_LERP, LOD. `TEX` / `TEXL`: all of that; `TEXA` / `TEXAL` ("any format"): everything but the formats and sizes of the tiles, the TLUT type and MID_TEXEL, which are read from the state | YUV, BI_LERP = 0 on filtered or TLUT texels |
  | combine | cycle count, the 16 `SET_COMBINE` selectors, shade needed, texel range. `COMB`: also the dither, alpha-compare and coverage options; `COMBX` ("any option"): those read from the state, one variant per selector set | chroma key |
  | zcmp | colour buffer size / format, image read, Z compare and mode, blender reads the memory alpha, RelaxedPrecision (anti-aliasing and force blend are read from the state; every combination of the rest exists for a 16-bit colour buffer) | 32-bit and 4 / 8-bit colour buffers |
  | blend | cycle count, the eight blender selectors. `BLEND`: also alpha compare, dither, coverage, force blend; `BLENDX`: those from the state | -- |
  | store | colour buffer size / format, coverage destination, Z update | 4 / 8-bit colour buffers |

  The tile's coordinate parameters (shift, clamp, mask, mirror), colours, constants and all per-pixel data stay run-time
  values in every variant.
* **Variants.** A variant is the kernel *body* instantiated with a literal state: `texture_body()`, `comb_body()`,
  `blend_body()`, `zcmp_body()`, `store_body()` are `always_inline` functions that take the state as a struct. The generic NEON
  kernel is the same body with the struct filled from the render state, a variant (`src/kspec_*.inc`) passes literals and the
  compiler folds operand selection, equation classes, format switches and option branches away. The variants of a stage are
  listed in `src/spec_*.def` (one `TEX(...)` / `COMB(...)` / ... line each) and compiled in ahead of time.
* **Dispatch and fallback.** The stages are independent: for each one, an exact key match wins, for combine and blend an "any
  option" variant with the same selectors is the second choice (for the texture unit the "any format" kernel of the same structure, for the combiner
  the shape kernels, below), and a stage without a variant keeps its generic NEON kernel
  (always available, handles every state: the "unterkernel"). `r->ktab` holds the choice, a hash lookup per stage and state
  change; a primitive costs one flag test. Each worker has its own table, so selection needs no synchronisation.
* **Exactness.** A variant must produce the same bytes as the generic kernel and as the scalar reference with exact knobs.
  This is tested per variant, not just on sample frames: see `foir-kfuzz -S` under [Tests](#tests).
* **Coverage** is the share of the pixels that ran through a variant per stage. The plugin logs it every 120 frames
  (`specialised pixels: ...`, with the share of the textured pixels that ran in an "any format" kernel), `foir-replay` prints it (and, per tier, `textured pixels: exact variants / "any format" kernels / generic kernel`,
  `combiner pixels: exact variants / shape kernels / generic kernel`), and the statistics build lists the keys that miss
  ([Adding variants](#adding-variants)).

#### Shape kernels (combiner)

The exact combiner variants are keyed on the full `SET_COMBINE` words and generalise poorly: on 39 games outside the 23 the lists were
made from, 71 % of the combiner pixels (mean) run in a variant, 10 of the 39 games below 50 %. What does generalise is the *shape* of the
equations. A **shape kernel** (`src/kspec_shape.inc`, list in `src/spec_shape.def`) compiles in only that structure and takes the operands
from a per-draw table.

* **Key** (`struct shape_key`, `src/spec.h`): cycle count and, per cycle, the shape of the RGB and of the alpha equation `(A - B) * C + D`.
  Every input has one of four letters: `0`, `1` (the constant 1.0), `C` (COMBINED, i.e. the cycle-0 result; cycle 1 of a two-cycle state only)
  or `V` (a variable operand: texel, shade, LOD fraction, noise, PRIM / ENV / K4 / K5 ..., COMBINED alpha). Equations that do not depend on A, B and C
  (C = 0, or A and B the same constant) are canonical "D only". One more bit, `ext`, says that a variable operand can leave 0..255
  (wide texels, or a sharpened LOD fraction in a multiply input) and has to be sign extended on load. Everything else (dither selects, alpha compare,
  coverage options, shade / noise needs) is read from the render state at run time, like the "any option" exact variants.
* **Operand table** (`struct sk_tab`, `src/rdp.h`): `foir_spec_select()` resolves, for every `V` input and colour channel, a 16-bit **byte offset into
  the px_batch** the kernel runs on. The per-pixel sources (`tex0` / `tex1` / `tex0n`, `lodfrac`) and the kernel's own scratch arrays (`shade`, `noise_s`,
  `c0a` = the cycle-0 alpha for an RGB multiply input) are fields of the batch; the **constants** (PRIM, ENV, KEY, K4, K5, PRIM LOD fraction) are splatted
  once per state into `b->kc[]`, already in the 9-bit extended form of their position, so a kernel never extends a constant. Reading a variable
  operand is the same instruction sequence whatever it is (an offset and a 16-byte load), the table is rebuilt only when the state changes, and it is
  independent of the batch address (the worker contexts and the tests use their own batches). The offsets are copied to locals at kernel entry so the
  compiler keeps them in registers.
* **Kernel set**: the 100 shape combinations that occur in the combiner statistics of 62 games (`test-output/m3/shapes/stats`), 8 `ext` forms of the ones
  with a LOD fraction multiply input, and the 39 one-cycle (RGB, alpha) pairs that follow from the one-cycle halves seen: **147 kernels, 298 KB of code** (the 110 exact
  variants are 275 KB). `python3 tools/shapegen.py --one-cycle-product test-output/m3/shapes/stats > src/spec_shape.def` regenerates the list. Compiling every
  valid shape is not possible with fused kernels: a half equation has 84 (RGB) / 56 (alpha) valid shapes in cycle 1 and 18 / 24 in cycle 0, and a kernel is
  the product of its four halves (about two million two-cycle combinations). Adding the product of all cycle-0 pairs with all cycle-1 pairs seen (663 kernels)
  would raise the leave-one-game-out coverage from 89.8 % to 93.6 %, so the rest of the space stays with the generic kernel.
* **Dispatch**: exact variant -> shape kernel -> generic kernel, per state change in `foir_spec_select()`. `FOIR_SPEC_COMB=auto|exact|shape|generic`
  restricts the choice (`exact` = the M3 behaviour, `shape` = shape kernels only, `generic` = neither); `FOIR_SPEC=0` still turns every variant off.
* **Operand access alternatives that were measured** (the table above is the "uniform load path"): a build with `-DFOIR_SK_SPLIT=1` makes the key say which
  variable operands are constants and loads them as registers (`vdup`) instead of from `kc[]`; 236 kernels cover the same states (leave-one-game-out coverage 85.2 % instead of 89.8 %) and the combiner stage
  was only 1-6 % faster (Toy Story 2 -1.5 %, Quest 64 -1.4 %, Conker -4 %, Mario Golf -6.4 %, measured with the 185-kernel list of that time) (see [results](#shape-kernels-cost-and-benefit)), so the uniform path is the shipped one. Pointers instead of offsets were not tried (they would
  tie the table to one batch address).

#### Texture and Z compare: the second tier ("any format")

The weak spots of the M3 lists were the games whose texture or Z compare *key* was not in the lists: on the 62 gameplay captures the texture stage ran in a variant for 0.1 % (Earthworm Jim 3D), 2.9 % (Hydro Thunder), 27 % (GoldenEye, one of the
23 test ROMs, in a later scene), 42 % (Pilotwings 64) and 54 % (Mario Golf) of its pixels, the Z compare for 18 % (Toy Story 2), 26 % (Turok), 44 % (Quake), 48 % (Tetrisphere) and 67-70 % (Star Wars Racer, Wipeout, Ridge Racer).
Nine more games were between 77 and 93 % in the texture stage and two more below 90 % in the Z compare; the generic kernel of a stage costs 1.4-1.8x the variant (texture) and 1.1-1.6x (Z compare, stage timer). **Why the keys miss** (`python3 tools/specstats.py`, the weak games' top missing keys):

| game | share of its pixels | missing key, and what is different from the nearest key in the list |
|---|---|---|
| Earthworm Jim 3D (texture) | 89 % | RGBA16, bilinear, perspective, **MID_TEXEL = 1**: no variant had MID_TEXEL set (the matrix only varies format, filter, perspective and cycle count) |
| Hydro Thunder (texture) | 87 % / 10 % | CI4 + TLUT / I4, bilinear, perspective, **MID_TEXEL = 1** (the rest is in the matrix) |
| Pilotwings 64 (texture) | 57 % | two cycles that read **both tiles**, both I4: the matrix has two-cycle states that read tile 0 only; states that read tile + 1 came from the statistics |
| Mario Golf (texture) | 43 % | two cycles, tile = I8, tile + 1 = RGBA16: **different formats** in the two tiles (the cross product of two formats is 81 pairs) |
| Dr. Mario 64, Zelda OoT (texture) | 23 % / 20 % | two cycles, RGBA16 + I4 point sampled; CI8 + CI4 with TLUT: the same |
| Toy Story 2 (Z compare) | 81 % | opaque Z compare with **AA = 1 and FORCE_BLEND = 1** and no image read |
| Turok (Z compare) | 74 % | **interpenetrating** mode without image read (the lists only had it with image read) |
| Quake / Tetrisphere / Racer / Wipeout / Ridge Racer | 56 / 52 / 29 / 20-30 % | no Z compare, no image read, AA and FORCE_BLEND off; image read without Z compare; no Z compare with AA only; with the memory-alpha shifts |

All of these are *combinations of independent bits* that none of the 23 ROMs the lists were made from happened to use. Two ways to generalise were compared with the leave-one-game-out estimate over the 62 games
(each game's pixels against the lists built from the other 61 games; 3 captured windows per game):

| texture design | kernels | coverage of a game's pixels (mean, min) | games below 90 % |
|---|---:|---|---:|
| M3: exact keys, lists from 23 ROMs + statistics (as shipped) | 106 | 91.7 %, 0.1 % | 12 (4 below 50 %) |
| exact keys + the 62 games' statistics (min share 0.02 %), any other key stays generic | 119 | 92.1 %, 0.1 % | 10 (4 below 50 %) |
| the same without MID_TEXEL in the key (read at run time) | -- | 95.5 %, 28 % | 6 |
| **key reduced to the structure** (cycles, texels read, filter, TLUT use, perspective, BI_LERP, LOD; formats, sizes, TLUT type, MID_TEXEL from the state) | **38** (30 seen + 8 structural) | **99.9 %, 98.4 %** | **0** |

The structure has few values (30 distinct in 62 games; 100 distinct exact keys without MID_TEXEL, 114 with every field), so a second tier of **"any format" kernels** (`TEXA` / `TEXAL` in `src/spec_tex.def`) is the generalising choice;
it is a key with `any = 1` looked up after the exact key, like `COMBX` / `BLENDX`: the tile formats and sizes, the TLUT type and MID_TEXEL are read from the tiles / the state (`struct tspec::anyfmt`), the rest is compiled in. The LOD kernels take the format of every tile the LOD selects
from that tile itself (the exact `TEXL` variants require one format along the whole mip chain and check it per batch). Dispatch is exact variant, then "any format" kernel, then the generic kernel;
`FOIR_SPEC_TEX=auto|exact|shape|generic` restricts it, as `FOIR_SPEC_COMB` does for the combiner. MID_TEXEL stays in the exact keys (a literal): making it a run-time value of the exact variants was tried and costs 6-11 % of the texture stage of every game
(6-8 % with the four-tap blend moved out of line, 2 % with the whole mid-texel sample handed to an out-of-line copy of the cycle function), so only the "any format" kernels read it from the state and take that route. The cost of the second tier, texture stage of a 60-frame replay (ms, same states in all three columns, `-DFOIR_STAGETIME`):

| 60-frame window | exact variant | "any format" kernel | generic kernel |
|---|---:|---:|---:|
| Donkey Kong 64 (1800) | 63.5 | 73.2 (+15 %) | 100.5 (+58 %) |
| Super Mario 64 (1800) | 140.4 | 161.3 (+15 %) | 222.3 (+58 %) |
| Pilotwings 64 (1800) | 276.7 | 341.2 (+23 %) | 479.5 (+73 %) |
| Mario Golf (1800) | 201.2 | 241.0 (+20 %) | 331.6 (+65 %) |
| Earthworm Jim 3D (3000) | 596.5 | 669.2 (+12 %) | 919.3 (+54 %) |
| Turok Dinosaur Hunter (1800) | 332.1 | 369.9 (+11 %) | 462.9 (+39 %) |

The Z compare (`ZCMP`) was treated the same way in the simplest form: ANTIALIAS_EN and FORCE_BLEND only choose the blend-enable lane mask at the end of the kernel, so they left the key (a few instructions per group, no measurable change of the stage time); what is left of the key is
image read x (RGBA / other colour buffer) x {no Z compare, four Z modes} x memory-alpha shifts x RelaxedPrecision, which is small enough to **compile in completely: 42 variants** for 16-bit colour buffers (`tools/specmatrix.py zcmp`), so the Z compare is always
specialised there (leave-one-game-out 97.1 % -> 100 %; the remainder of the stage is 32-bit and 4 / 8-bit colour buffers, 1.8 % of Conker's pixels).

#### Adding variants

```sh
# 1. a statistics build; run games with FOIR_STATE_STATS=1 FOIR_STATE_STATS_ALL=1 FOIR_STATE_STATS_FILE=/path/log.txt
#    (the plugin rewrites the file every 120 frames), or foir-replay dumps with 2> log.txt
CFLAGS=-DFOIR_STATS make all replay CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 POSTFIX=-stats
# 2. rank the keys over any number of logs (every log counts equally) and print the lines to add
python3 tools/specstats.py log1.txt log2.txt ...                      # per stage: ranked keys, '*' = covered
python3 tools/specstats.py --def --stage comb log*.txt >> src/spec_comb.def
python3 tools/specstats.py --def --stage tex --shapes log*.txt        # exact TEX / TEXL lines and the TEXA / TEXAL "any format" kernels
python3 tools/specmatrix.py zcmp > src/spec_zcmp.def                  # the complete Z compare list (no statistics needed)
python3 tools/specstats.py --loo 40 log*.txt                          # leave-one-log-out coverage estimate
# 3. rebuild; `make check` fuzzes every variant (kfuzz -S) against the scalar reference
#    (the statistics build also prints `LODSTAT ...` lines at exit: texture batches / pixels / groups per mode, distinct tiles per group, generic-kernel fallbacks of the LOD variants, next-pixel divides redone)
```

---

## Speed / accuracy knobs

`[Video-Foir]` section; front-ends can set them per game.

| Knob | Default | Effect |
|---|---|---|
| `FastCombiner` | on | Use the NEON combiner and blender (exact). Off: the scalar reference combiner and blender. |
| `RelaxedPrecision` | on | Depth compare of opaque / transparent Z modes without the delta-Z terms for partially covered pixels (affects only coplanar edge cases). |
| `FrameSkip` | `0` | `0` = off, `N` = render one frame in `N+1`, `auto` = skip when the achieved frame interval exceeds the VI period (levels: none / every 2nd / 2 of 3). Skipped frames are still parsed (state, TMEM loads, syncs) but not rendered. |
| `InterlaceHack` | on | Show only the freshest field of field-rendered interlaced frames (see above). |
| `Threads` | `0` | Worker threads; `0` = auto (cores - 1), `-1` = none. |
| `NEON` | on | Use the NEON kernels (`False` = scalar reference). |
| `Specialize` | on | Use the precompiled per-state pixel kernel variants ([below](#specialised-pixel-kernels-m3)); off = the generic NEON kernels only (bit-identical output, slower). Needs `NEON`. |

A `JIT` entry that an older config file may still contain is ignored: the Lightning pipeline compiler this knob was reserved for was
dropped in favour of the static variants (see the [feasibility checkpoint](#feasibility-checkpoint)).

A `SkipCoverage` knob (treat every span pixel as fully covered) existed during M2 and was removed. It made the edge pixels a
triangle doesn't actually cover pass the write test, so triangles sharing an edge both drew it, with colour and texture coordinates
extrapolated past the edge: one-pixel seams along polygon edges (very visible on SM64's cap and Pokemon Snap's terrain). It saved
only ~0.7-1.5 ms per frame at 3 workers (SM64, DK64, Snap; nothing measurable on Banjo), and at 3 workers the RDP is already off the
critical path, so it bought no frame rate.

Environment overrides for testing: `FOIR_THREADS=n`, `FOIR_NEON=0|1`, `FOIR_SPEC=0` (same as `Specialize = False`),
`FOIR_KNOBS=exact` (every knob at its accuracy-preserving setting), `FOIR_NEON_MASK` (bit per kernel: texture, combine,
zcmp, blend, store, fill, copy; a stage that is not NEON is not specialised either; `0x3f` = everything but the NEON COPY kernel),
`FOIR_SPEC_COMB=auto|exact|shape|generic` (combiner dispatch: exact variant -> shape kernel -> generic kernel, or only some of them, see
[Shape kernels](#shape-kernels-combiner)), `FOIR_SPEC_TEX=auto|exact|shape|generic` (texture dispatch: exact variant -> "any format" kernel -> generic kernel, see
[below](#texture-and-z-compare-the-second-tier-any-format)), `FOIR_NEON_VERIFY=1` (run every NEON stage, the specialised variants included, next to the scalar one and report differences;
combine it with `FOIR_KNOBS=exact`, the scalar kernels have no RelaxedPrecision), `FOIR_MT_STATS=1`, `FOIR_LOGFILE=path`.

### suxty interface

foir exports one extra function, `FoirSuxtyGetInterface()` (declared in `src/foir_suxty.h`, listed in `src/foir_api_export.ver`), so that the in-game
menu of [mupen64plus-input-suxty](../mupen64plus-input-suxty) can list and change foir's options. It returns a pointer to a static, `const`,
versioned table (`abi`, `size`, option count, option descriptors, `get`, `set`, `save_default`). It is one-way: foir never calls suxty, nothing in
the plugin API changes, and a foir that nobody asks for behaves exactly as before (the only cost is one thread id kept at `RomOpen`). suxty finds the symbol
by walking the loaded objects (the front-end loads plugins `RTLD_LOCAL`, under whatever file name the user chose) and checks `abi`.

| id | key (`[Video-Foir]`) | type | applies |
|---|---|---|---|
| 0 | `Widescreen` | bool | **at once** |
| 1 | `Filter` | bool | **at once** |
| 2 | `InterlaceHack` | bool | **at once** |
| 3 | `FrameSkip` | enum: Off / Every 2nd / 2 of 3 / 3 of 4 / Auto | **at once** |
| 4 | `VSync` | bool | next launch |
| 5 | `Threads` | enum: None, Auto, 1..7 (index = `Threads` + 1) | next launch |
| 6 | `NEON` | bool | next launch |
| 7 | `Specialize` | bool | next launch |
| 8 | `FastCombiner` | bool | next launch |
| 9 | `RelaxedPrecision` | bool | next launch |

*Why these are live.* `Widescreen` and `Filter` are read by `gl_out_present()` on every frame. `InterlaceHack` is read by `vi_render()` only (the
emulation thread). `FrameSkip` is read by the SYNC_FULL hook, which also runs on the emulation thread. None of them is in the worker threads' private
copy of the config. *Why the others are not.* `Threads`, `NEON`, `Specialize` and `FastCombiner` choose the kernel tables and the worker pool in
`rdp_init()` / `rdp_start_workers()`; `RelaxedPrecision` is part of the specialisation key and is copied into every worker (and overwritten per variant
there); `VSync` is a context attribute that is applied when the window is created. Changing them mid-game would mean rebuilding the pipeline.

*Thread safety.* `get()` / `set()` must be called on the emulation thread, not from inside a foir call. suxty's menu runs inside the buffer swap, which
`UpdateScreen()` issues last, after `rdp_sync()` has drained the workers and after the frame was uploaded: nothing else touches the live options
while the menu is open, and the next `ProcessRDPList()` starts after the menu returns. Once the ROM is open a call from another thread is refused
(`FOIR_SUXTY_E_THREAD`).

*What `set()` does.* It never writes `mupen64plus.cfg`. Before `RomOpen` it overrides what `load_config()` read from `[Video-Foir]` for the coming launch
(suxty hands over its stored per-game values from `InitiateControllers`, which runs before foir opens the ROM; this is how the "next launch" options get
applied without a restart). After `RomOpen` a live option takes effect and a next-launch option is only remembered (`FOIR_SUXTY_NEXT_LAUNCH`).
Environment overrides (`FOIR_NEON`, `FOIR_KNOBS=exact`) still win. `save_default()` writes one value into `[Video-Foir]` and saves that section; suxty only calls it
when the user picks "Save as foir default".

---

## Measured results

Rig: Rockchip RK3566, 4x Cortex-A55 (up to 1.8 GHz, schedutil), 975 MB RAM, ROCKNIX, stock Mupen64Plus 2.6 with
`rsp-cxd4` (`DisplayListToGraphicsPlugin=False`), gfx = foir. `aarch64-linux-gnu-gcc` 13, `-O3`.

### Renderer speed (offline replay)

**RDP time per frame, offline replay of captured frames on the rig** (`foir-replay`, ms of wall time per frame, mean of 15 runs of a two-frame dump; includes waiting for the workers):

| game (frame) | pixels/frame | M1 scalar | scalar (M2 code) | NEON | NEON + 1 worker | NEON + 2 | NEON + 3 | NEON + 3, fast knobs | speedup 1 thread | speedup 3 workers |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Super Mario 64 | 59k | 39.0 | 37.6 | 15.0 | 13.8 | 7.7 | **5.8** | 4.8 | 2.6x | **6.7x** |
| Star Fox 64 | 110k | 54.8 | 56.5 | 15.2 | 16.5 | 8.1 | **6.8** | 5.6 | 3.6x | **8.1x** |
| Donkey Kong 64 | 39k | 31.5 | 29.9 | 11.8 | 10.9 | 6.2 | **5.1** | 3.9 | 2.7x | **6.1x** |
| Banjo-Kazooie | 170k | 138.6 | 141.4 | 32.1 | 34.2 | 17.1 | **11.0** | 10.7 | 4.3x | **12.6x** |
| Pokemon Snap | 230k | 251.3 | 252.9 | 59.7 | 60.0 | 30.6 | **21.5** | 20.2 | 4.2x | **11.7x** |

`M1 scalar` is the M1 reference renderer built unchanged; `scalar (M2 code)` is the M2 code with `--scalar` (the same kernels
after the pixel-batch restructuring); `fast knobs` is `foir-replay --fast` (measured with the then-defaults `SkipCoverage`,
`FastCombiner` and `RelaxedPrecision` on; `SkipCoverage` has since been removed, see above). The other columns use the accuracy-preserving settings and are bit-identical to each other and to the scalar
reference. Each frame is rendered twice per dump (the second pass starts with warm caches); the table shows the per-frame mean.
NEON alone is **2.6-4.3x** faster than the M1 scalar renderer; with three workers it is **6.1-12.6x** faster. A single worker is
about as fast as the inline NEON path (parsing and primitive setup stay on the emulation thread, and everything the workers do is
waited for at `SYNC_FULL`); going from 1 to 3 workers is 2.1-3.1x on the four-core part, the rest being the serial parse / setup
share, the barriers at `SYNC_FULL` and band imbalance.

**M3: specialised kernels.** The same replay (RK3566, ms of wall time per frame; three alternating rounds of 10 runs of the
captured two-frame dumps, builds interleaved in one session; `foir-replay --fast`, i.e. the plugin's default knobs, for every
column; `test-output/m3/replay_bench.md` has the raw numbers). "M2" is the renderer before this milestone, built unchanged;
"generic only" is the M3 code with `Specialize = False`:

| game (frame) | M2, 1 thread | **M3, 1 thread** | speedup | M3 generic only, 1 thread | M2, 3 workers | **M3, 3 workers** | speedup |
|---|---:|---:|---:|---:|---:|---:|---:|
| Super Mario 64 | 14.9 | **10.9** | **1.37x** | 15.0 | 5.4 | **4.2** | **1.29x** |
| Star Fox 64 | 15.3 | **9.9** | **1.56x** | 15.2 | 6.3 | **4.6** | **1.37x** |
| Donkey Kong 64 (36 % of its pixels use LOD) | 11.2 | **8.9** | **1.26x** | 11.0 | 5.0 | **4.2** | **1.19x** |
| Banjo-Kazooie | 31.9 | **19.3** | **1.65x** | 32.4 | 10.8 | **7.3** | **1.49x** |
| Pokemon Snap | 57.7 | **39.8** | **1.45x** | 57.7 | 21.6 | **14.8** | **1.46x** |
| GoldenEye 007 (LOD) | 4.1 | **3.3** | **1.25x** | 4.0 | 1.9 | **1.4** | **1.40x** |
| Paper Mario | 11.9 | **7.9** | **1.50x** | 12.0 | 4.4 | **3.1** | **1.41x** |

The variants are 1.25-1.65x faster on one thread; the generic kernels are as fast as before (the refactoring into one body per
stage cost nothing). With three workers the gain is a little smaller because the serial part (parse, primitive setup, barriers)
does not shrink. In all of these frames 99.6-100 % of the pixels run through variants in every stage (they are among the frames
the lists were built from; see [Specialisation coverage](#specialisation-coverage) for games that are not).

### Shape kernels: cost and benefit

Same rig, `foir-replay` (inline, `-t 0`), default build (`-O3`, generic armv8-a), schedutil governor, one warm-up per dump, rounds of 3-10 runs with the
variants interleaved, medians. Per-stage times come from a timing build (`-DFOIR_STAGETIME`: `cntvct_el0` around every stage call of a batch, timer read
overhead subtracted; it prints the combiner time per kind of kernel). `FOIR_SPEC_COMB` selects the combiner path, so the *same states* run through
the exact variants (`exact`), the shape kernels (`shape`) or the generic kernel (`generic`). Raw logs and the table scripts are in `test-output/m3/shapes/kernels/`.

**Cost of the operand table (shape kernel vs exact variant on the same states).** The captured two-frame dumps of `test-data/dumps` are covered by exact
variants, so exact, shape and generic run the same pixels (Hydro Thunder has no combiner pixels: it is fills / copies):

| frame | combiner pixels | exact variant ns/px | shape kernel ns/px | shape / exact | generic ns/px | generic / shape | replay ms: exact | shape | generic |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Banjo-Kazooie (450) | 339k | 16.23 | 21.52 | 1.33x | 51.17 | 2.38x | 40.07 | 42.14 | 52.91 |
| Diddy Kong Racing (700) | 192k | 8.68 | 12.07 | 1.39x | 31.93 | 2.65x | 16.48 | 17.24 | 21.27 |
| Donkey Kong 64 (800) | 78k | 8.41 | 10.36 | 1.23x | 31.01 | 2.99x | 18.55 | 18.50 | 20.63 |
| F-Zero X (700) | 164k | 4.48 | 7.29 | 1.63x | 18.14 | 2.49x | 11.30 | 11.84 | 13.57 |
| GoldenEye 007 (900) | 18k | 8.87 | 11.45 | 1.29x | 35.73 | 3.12x | 6.64 | 6.74 | 7.19 |
| Hydro Thunder (1500) | 0 | -- | -- | -- | -- | -- | 228.8 | 228.8 | 228.6 |
| Jet Force Gemini (500) | 76k | 6.88 | 8.43 | 1.23x | 24.75 | 2.94x | 4.45 | 4.59 | 5.87 |
| Kirby 64 (600) | 327k | 6.46 | 9.21 | 1.42x | 26.88 | 2.92x | 43.53 | 44.79 | 50.82 |
| Paper Mario (900) | 135k | 9.74 | 11.68 | 1.20x | 28.45 | 2.44x | 16.30 | 16.67 | 18.82 |
| Pokemon Snap (300) | 457k | 13.74 | 17.26 | 1.26x | 42.83 | 2.48x | 84.74 | 87.09 | 99.20 |
| Star Fox 64 (1000) | 219k | 12.30 | 14.23 | 1.16x | 30.69 | 2.16x | 20.23 | 20.86 | 24.47 |
| Super Mario 64 (650) | 117k | 11.67 | 15.22 | 1.30x | 33.08 | 2.17x | 21.61 | 22.36 | 24.86 |

* The shape kernel costs **1.15-1.65x an exact variant** (median 1.3x) on the combiner stage: +1.5 to +3.6 ns per pixel, +0 to +2.1 ms per two-frame dump,
  i.e. **0-5 % of the replay time** (Banjo-Kazooie +5.2 %, Diddy Kong Racing +4.6 %, F-Zero X +4.8 %, Super Mario 64 +3.5 %, Pokemon Snap +2.8 %, Donkey Kong 64 within the noise).
  The generic kernel costs 2.2-3.1x a shape kernel and 2.5-4.2x an exact variant.
* Where the 30 % go was only partly attributed. Measured: keeping the shade array out of memory (a timing-only build without the shade stores) saves 0-5 %,
  constants as registers instead of loads from `kc[]` 1-6 % (the split build above), and unswitching the loops over the run-time options changes the speed not at all (it makes the
  code 27 % smaller, so the shape kernels are built with `no-unswitch-loops`). `perf stat` shows 22 more instructions per 8-pixel group (+2.1 cycles per pixel) for the shape kernel
  than for the exact variant on Super Mario 64, and 39 more (+5.7 cycles per pixel) on F-Zero X, whose exact variant also has the dither options compiled in (the shape kernel reads them from the state).
  The remainder is the run-time option branches and the extra loads and stores of a table-driven kernel; no single cause was found. An exact variant stays faster, hence the dispatch order.
* The exact path itself is unchanged: the M3 renderer built before these changes and the new one with `FOIR_SPEC_COMB=exact` or `auto` take the same time on the 12 dumps
  (differences within +-1 %, Jet Force Gemini 2.5 % = 0.1 ms; `px_batch` grew by 6 KB).

**Benefit on states without an exact variant (60-frame dumps of the unseen games).** `M3` = `FOIR_SPEC_COMB=exact` (exact variant or generic kernel: the renderer before this change),
`auto` = exact -> shape -> generic. Pixel columns: share of the combiner pixels in an exact variant / only in a shape kernel / in the generic kernel.
The shipped kernel list was made from the statistics of all 62 games, so for these dumps it is in-sample and leaves nothing to the generic kernel:

| frame (60 frames) | combiner pixels: exact / shape-only / generic | combiner ms: M3 (exact+generic) | auto (exact+shape) | generic only | replay ms: M3 | auto | generic only | auto vs M3 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Toy Story 2 (4200) | 0% / 100% / 0% | 150.4 | 63.7 | 150.7 | 544 | 449 | 542 | -17.5% |
| Quest 64 (3000) | 4% / 96% / 0% | 172.9 | 70.5 | 175.4 | 765 | 653 | 764 | -14.6% |
| Mario Golf (1800) | 11% / 89% / 0% | 184.9 | 78.8 | 194.0 | 1093 | 979 | 1105 | -10.4% |
| Conker (1800) | 41% / 59% / 0% | 100.8 | 54.0 | 118.7 | 621 | 566 | 639 | -8.9% |
| Conker (3000) | 0% / 100% / 0% | 133.2 | 60.0 | 134.2 | 642 | 556 | 642 | -13.5% |
| Pokemon Stadium 2 (1800) | 26% / 74% / 0% | 413.1 | 163.6 | 473.4 | 1228 | 975 | 1286 | -20.6% |
| Tonic Trouble (1800) | 22% / 78% / 0% | 84.7 | 34.6 | 99.0 | 607 | 562 | 620 | -7.4% |
| Gauntlet Legends (1800) | 36% / 64% / 0% | 269.8 | 124.8 | 334.4 | 1696 | 1547 | 1777 | -8.8% |
| Turok (1800) | 74% / 26% / 0% | 63.2 | 45.2 | 139.3 | 683 | 666 | 771 | -2.4% |
| Tetrisphere (1800) | 53% / 47% / 0% | 171.1 | 96.7 | 221.5 | 1438 | 1351 | 1499 | -6.1% |
| Super Mario 64 (4200) | 100% / 0% / 0% | 88.5 | 87.6 | 214.6 | 941 | 936 | 1093 | -0.5% |
| Banjo Kazooie (3000) | 100% / 0% / 0% | 122.6 | 120.7 | 386.8 | 1352 | 1349 | 1654 | -0.2% |
| Pokemon Snap (1800) | 100% / 0% / 0% | 39.3 | 38.9 | 120.4 | 389 | 388 | 470 | -0.5% |
| GoldenEye 007 (1800) | 52% / 48% / 0% | 181.6 | 106.4 | 268.7 | 1433 | 1351 | 1539 | -5.7% |
| Pilotwings 64 (4200) | 56% / 44% / 0% | 84.1 | 46.7 | 126.7 | 517 | 475 | 566 | -8.1% |

(Super Mario 64, Banjo-Kazooie, Pokemon Snap and GoldenEye 007 are games of the 23-ROM sample, the other rows are unseen games.) On the states only a shape kernel
covers, the combiner stage is **2.2-2.5x faster** than with the generic kernel (Toy Story 2 150 -> 64 ms, Quest 64 173 -> 71 ms, Conker (3000) 133 -> 60 ms, Pokemon Stadium 2 413 -> 164 ms per 60 frames),
and the whole replay **7-21 % faster** in the games with little exact coverage (Toy Story 2 -17.5 %, Quest 64 -14.6 %, Pokemon Stadium 2 -20.6 %, Conker -9 / -13.5 %, Mario Golf -10.4 %,
Gauntlet Legends -8.8 %, Tonic Trouble -7.4 %). With three worker threads (`-t 3`, 15-run medians, `rig_unseen_3workers_total.txt`) the gain remains: Toy Story 2 204 -> 175 ms, Quest 64 284 -> 254 ms,
Mario Golf 423 -> 380 ms, Pokemon Stadium 2 437 -> 350 ms, Gauntlet Legends 642 -> 580 ms. The two frames of the games fully covered by exact variants do not change.

A game nobody has looked at is not that well served, though: the same dumps with a kernel list built **only from the statistics of the 23 test games**
(`src/spec_shape_t23.def`, selectable with `-DFOIR_SHAPE_DEF=\"spec_shape_t23.def\"`). Games that use shape combinations the 23 lack stay generic
(Toy Story 2 and Quest 64 completely: their cycle-1 shapes `C0vC` and `10vC` do not occur in the 23 games), the others gain 2-13 %:

| frame (60 frames) | combiner pixels: exact / shape-only / generic | combiner ms: M3 | 23-game list | replay ms: M3 | 23-game list | change |
|---|---|---:|---:|---:|---:|---:|
| Toy Story 2 (4200) | 0% / 0% / 100% | 150.4 | 152.9 | 544 | 555 | +1.9% |
| Quest 64 (3000) | 4% / 0% / 96% | 172.9 | 174.7 | 765 | 774 | +1.2% |
| Mario Golf (1800) | 11% / 19% / 70% | 184.9 | 167.6 | 1093 | 1086 | -0.6% |
| Conker (1800) | 41% / 50% / 9% | 100.8 | 67.6 | 621 | 587 | -5.5% |
| Conker (3000) | 0% / 100% / 0% | 133.2 | 60.0 | 642 | 559 | -13.0% |
| Pokemon Stadium 2 (1800) | 26% / 33% / 40% | 413.1 | 311.3 | 1228 | 1124 | -8.5% |
| Tonic Trouble (1800) | 22% / 77% / 1% | 84.7 | 36.0 | 607 | 569 | -6.2% |
| Gauntlet Legends (1800) | 36% / 59% / 4% | 269.8 | 150.3 | 1696 | 1566 | -7.6% |
| Turok (1800) | 74% / 0% / 26% | 63.2 | 63.9 | 683 | 692 | +1.4% |
| Tetrisphere (1800) | 53% / 47% / 0% | 171.1 | 95.4 | 1438 | 1342 | -6.7% |
| Pilotwings 64 (4200) | 56% / 16% / 28% | 84.1 | 71.1 | 517 | 508 | -1.8% |

### Cheap wins after M3

Three performance items on top of M3, each measured on its own and together. Rig as above (RK3566, schedutil); `foir-replay --fast` (the plugin's default knobs) with the **median of 3-4 alternating rounds** of the builds in one session
(each dump: one warm-up, then `-n 5` repeats of the two-frame dump or one run of the 60-frame window per round; the spread between rounds was < 3 % at one thread, up to 10 % with three workers, so differences below about 2 % are noise). The columns are builds of the same sources:
*before* = the M3 renderer built unchanged; *+ COPY* = M3 plus the NEON COPY kernel only; *+ buffer access* = M3 plus the colour / Z buffer changes only (the NEON COPY kernel of that build switched off with `FOIR_NEON_MASK=0x3f`);
*+ tex / Z tiers* = M3 plus the second texture tier, run-time AA / FORCE_BLEND and the complete Z compare list only (old buffer access, scalar COPY, no prefetch); *all three* = this tree. Percentages are relative to *before* (negative = faster).

**The 12 captured frames of `test-data/dumps` (two-frame dumps, ms per dump), one thread:**

| dump | before | + COPY (NEON) | + buffer access | + tex / Z tiers | all three | all vs before |
|---|---:|---:|---:|---:|---:|---:|
| Banjo-Kazooie (450) | 37.9 | 38.5 (+1.4 %) | 36.6 (-3.5 %) | 39.2 (+3.3 %) | 36.9 (-2.6 %) | 1.03x |
| Diddy Kong Racing (700) | 15.4 | 15.5 (+0.3 %) | 14.6 (-5.5 %) | 15.6 (+1.0 %) | 14.5 (-6.3 %) | 1.07x |
| Donkey Kong 64 (800) | 17.7 | 17.6 (-0.3 %) | 16.6 (-6.0 %) | 17.9 (+1.0 %) | 17.1 (-3.5 %) | 1.04x |
| F-Zero X (700) | 10.8 | 10.8 (+0.2 %) | 10.1 (-5.9 %) | 10.8 (+0.5 %) | 10.2 (-5.5 %) | 1.06x |
| GoldenEye 007 (900) | 6.4 | 6.6 (+2.7 %) | 6.4 (-0.7 %) | 6.6 (+2.4 %) | 6.6 (+3.0 %) | 0.97x |
| Hydro Thunder (1500) | 230 | 86.7 (-62.3 %) | 227 (-1.0 %) | 229 (-0.3 %) | 86.1 (-62.5 %) | 2.67x |
| Jet Force Gemini (500) | 4.3 | 4.3 (-0.9 %) | 4.2 (-2.7 %) | 4.3 (-0.7 %) | 4.2 (-3.9 %) | 1.04x |
| Kirby 64 - The Crystal Shards (600) | 42.0 | 34.3 (-18.5 %) | 40.0 (-4.7 %) | 42.2 (+0.5 %) | 32.6 (-22.4 %) | 1.29x |
| Paper Mario (900) | 15.9 | 12.7 (-20.0 %) | 15.6 (-1.9 %) | 16.2 (+1.6 %) | 12.6 (-20.8 %) | 1.26x |
| Pokemon Snap (300) | 79.0 | 79.2 (+0.3 %) | 74.8 (-5.2 %) | 80.0 (+1.4 %) | 76.4 (-3.2 %) | 1.03x |
| Star Fox 64 (1000) | 19.6 | 19.5 (-0.2 %) | 18.6 (-5.0 %) | 19.7 (+0.8 %) | 18.8 (-3.9 %) | 1.04x |
| Super Mario 64 (650) | 21.6 | 21.0 (-2.6 %) | 20.5 (-5.3 %) | 21.6 (-0.0 %) | 20.9 (-3.4 %) | 1.04x |
| **geometric mean of the ratios to "before"** | 1 | 0.890 | 0.960 | 1.009 | 0.863 | **1.16x** |

Every one of these frames was already covered by exact variants in every stage, so the texture / Z tiers have nothing to add here: the column shows what they cost where they are not needed (+0.9 % geometric mean, -1 to +3 % per frame: the Z compare reads AA / FORCE_BLEND at run time and the
new code shifts the layout of the hot kernels; the exact texture variants are as fast as before, checked with the stage timer: 63.4 vs 63.5 ms (DK64), 139.0 vs 139.1 ms (SM64)).
The COPY kernel is the whole gain of Hydro Thunder (230 -> 86 ms: the two frames are COPY rectangles and fills), Kirby 64 and Paper Mario (-19 to -20 %); the buffer access is 3-6 % of the 3D frames.

**The same with three worker threads:**

| dump | before | + COPY (NEON) | + buffer access | + tex / Z tiers | all three | all vs before |
|---|---:|---:|---:|---:|---:|---:|
| Banjo-Kazooie (450) | 13.8 | 13.6 (-1.4 %) | 12.9 (-6.0 %) | 14.8 (+7.5 %) | 13.8 (+0.7 %) | 0.99x |
| Diddy Kong Racing (700) | 5.8 | 5.9 (+1.4 %) | 5.3 (-9.1 %) | 5.8 (-1.2 %) | 5.3 (-9.3 %) | 1.10x |
| Donkey Kong 64 (800) | 7.6 | 7.7 (+1.2 %) | 7.1 (-6.3 %) | 7.7 (+2.1 %) | 7.2 (-4.6 %) | 1.05x |
| F-Zero X (700) | 4.8 | 4.8 (+1.1 %) | 5.7 (+19.3 %) | 5.6 (+16.5 %) | 4.5 (-5.7 %) | 1.06x |
| GoldenEye 007 (900) | 2.9 | 3.4 (+17.9 %) | 2.9 (+0.6 %) | 2.9 (+1.1 %) | 2.9 (+0.0 %) | 1.00x |
| Hydro Thunder (1500) | 108 | 67.7 (-37.3 %) | 109 (+1.0 %) | 109 (+1.0 %) | 67.4 (-37.5 %) | 1.60x |
| Jet Force Gemini (500) | 2.1 | 1.8 (-14.0 %) | 1.9 (-10.6 %) | 1.9 (-12.5 %) | 1.9 (-10.8 %) | 1.12x |
| Kirby 64 - The Crystal Shards (600) | 14.9 | 12.7 (-15.1 %) | 14.1 (-5.7 %) | 15.2 (+2.0 %) | 11.8 (-20.9 %) | 1.26x |
| Paper Mario (900) | 6.4 | 5.2 (-18.1 %) | 6.2 (-2.5 %) | 6.3 (-0.8 %) | 5.1 (-20.1 %) | 1.25x |
| Pokemon Snap (300) | 29.6 | 30.0 (+1.2 %) | 29.8 (+0.5 %) | 29.5 (-0.4 %) | 27.9 (-5.7 %) | 1.06x |
| Star Fox 64 (1000) | 8.4 | 8.5 (+1.1 %) | 7.8 (-7.9 %) | 8.5 (+0.5 %) | 7.8 (-7.3 %) | 1.08x |
| Super Mario 64 (650) | 8.0 | 7.9 (-1.3 %) | 7.7 (-3.9 %) | 8.1 (+1.7 %) | 7.7 (-3.5 %) | 1.04x |
| **geometric mean of the ratios to "before"** | 1 | 0.937 | 0.972 | 1.013 | 0.889 | **1.12x** |

**60-frame gameplay windows of 15 games** (the captures behind the coverage statistics; the first seven are windows of ROMs whose first minute the variant lists were made from, the other eight are unseen games that show the second tiers), one thread:

| dump | before | + COPY (NEON) | + buffer access | + tex / Z tiers | all three | all vs before |
|---|---:|---:|---:|---:|---:|---:|
| Paper Mario (1800) | 915 | 815 (-10.9 %) | 892 (-2.5 %) | 917 (+0.3 %) | 814 (-11.0 %) | 1.12x |
| Super Mario 64 (1800) | 440 | 439 (-0.3 %) | 421 (-4.3 %) | 440 (-0.1 %) | 427 (-3.0 %) | 1.03x |
| Donkey Kong 64 (1800) | 305 | 308 (+1.0 %) | 290 (-5.0 %) | 303 (-0.7 %) | 286 (-6.4 %) | 1.07x |
| Banjo Kazooie (1800) | 951 | 958 (+0.7 %) | 930 (-2.3 %) | 963 (+1.2 %) | 927 (-2.6 %) | 1.03x |
| Mischief Makers (1800) | 1207 | 546 (-54.7 %) | 1190 (-1.4 %) | 1206 (-0.1 %) | 535 (-55.6 %) | 2.25x |
| Hydro Thunder (1800) | 209 | 63.7 (-69.5 %) | 208 (-0.6 %) | 209 (-0.1 %) | 62.0 (-70.3 %) | 3.37x |
| Rocket Robot on Wheels (1800) | 945 | 943 (-0.2 %) | 902 (-4.5 %) | 953 (+0.9 %) | 912 (-3.5 %) | 1.04x |
| Earthworm Jim 3D (3000) | 3039 | 3055 (+0.5 %) | 2928 (-3.6 %) | 2697 (-11.3 %) | 2599 (-14.5 %) | 1.17x |
| Pilotwings 64 (1800) | 893 | 895 (+0.3 %) | 863 (-3.3 %) | 693 (-22.4 %) | 674 (-24.5 %) | 1.33x |
| Mario Golf (1800) | 939 | 834 (-11.2 %) | 902 (-4.0 %) | 853 (-9.2 %) | 719 (-23.4 %) | 1.31x |
| Turok Dinosaur Hunter (1800) | 650 | 653 (+0.4 %) | 632 (-2.8 %) | 640 (-1.6 %) | 614 (-5.6 %) | 1.06x |
| Toy Story 2 Buzz Lightyear to the Rescue (4200) | 431 | 431 (-0.0 %) | 407 (-5.5 %) | 424 (-1.7 %) | 408 (-5.5 %) | 1.06x |
| Quake (1800) | 175 | 176 (+0.6 %) | 164 (-6.2 %) | 176 (+0.9 %) | 168 (-4.0 %) | 1.04x |
| Tetrisphere (1800) | 1302 | 1311 (+0.7 %) | 1294 (-0.6 %) | 1313 (+0.9 %) | 1257 (-3.4 %) | 1.04x |
| RR64 Ridge Racer 64 (1800) | 773 | 769 (-0.4 %) | 751 (-2.8 %) | 764 (-1.1 %) | 746 (-3.4 %) | 1.04x |
| **geometric mean of the ratios to "before"** | 1 | 0.865 | 0.967 | 0.968 | 0.806 | **1.24x** |

and with three workers:

| dump | before | + COPY (NEON) | + buffer access | + tex / Z tiers | all three | all vs before |
|---|---:|---:|---:|---:|---:|---:|
| Paper Mario (1800) | 350 | 325 (-7.2 %) | 336 (-4.1 %) | 349 (-0.3 %) | 311 (-11.3 %) | 1.13x |
| Super Mario 64 (1800) | 165 | 167 (+0.9 %) | 155 (-6.6 %) | 164 (-1.1 %) | 156 (-5.8 %) | 1.06x |
| Donkey Kong 64 (1800) | 121 | 126 (+4.6 %) | 115 (-4.9 %) | 122 (+1.4 %) | 115 (-4.7 %) | 1.05x |
| Banjo Kazooie (1800) | 345 | 347 (+0.8 %) | 344 (-0.3 %) | 348 (+1.0 %) | 343 (-0.5 %) | 1.00x |
| Mischief Makers (1800) | 448 | 238 (-46.8 %) | 431 (-3.7 %) | 450 (+0.6 %) | 236 (-47.2 %) | 1.90x |
| Hydro Thunder (1800) | 81.4 | 34.1 (-58.1 %) | 81.8 (+0.6 %) | 82.1 (+1.0 %) | 34.3 (-57.8 %) | 2.37x |
| Rocket  Robot on Wheels (1800) | 369 | 356 (-3.7 %) | 348 (-5.6 %) | 363 (-1.6 %) | 350 (-5.3 %) | 1.06x |
| Earthworm Jim 3D (3000) | 1158 | 1192 (+3.0 %) | 1102 (-4.8 %) | 1081 (-6.7 %) | 977 (-15.6 %) | 1.18x |
| Pilotwings 64 (1800) | 322 | 322 (+0.2 %) | 314 (-2.5 %) | 268 (-16.7 %) | 251 (-21.9 %) | 1.28x |
| Mario Golf (1800) | 369 | 336 (-8.9 %) | 359 (-2.7 %) | 382 (+3.7 %) | 296 (-19.8 %) | 1.25x |
| Turok  Dinosaur Hunter (1800) | 236 | 238 (+1.1 %) | 226 (-4.2 %) | 268 (+13.7 %) | 224 (-4.8 %) | 1.05x |
| Toy Story 2  Buzz Lightyear to the Rescue (4200) | 165 | 165 (-0.0 %) | 158 (-4.0 %) | 165 (-0.1 %) | 156 (-5.5 %) | 1.06x |
| Quake (1800) | 68.4 | 68.0 (-0.6 %) | 64.8 (-5.2 %) | 68.2 (-0.3 %) | 65.0 (-5.0 %) | 1.05x |
| Tetrisphere (1800) | 492 | 485 (-1.4 %) | 516 (+5.0 %) | 498 (+1.4 %) | 463 (-5.8 %) | 1.06x |
| RR64  Ridge Racer 64 (1800) | 324 | 286 (-11.6 %) | 271 (-16.3 %) | 288 (-11.3 %) | 272 (-16.1 %) | 1.19x |
| **geometric mean of the ratios to "before"** | 1 | 0.890 | 0.959 | 0.988 | 0.829 | **1.21x** |

**Summary** (geometric mean of the per-dump ratios to *before*, medians; the minimum over the rounds gives the same figures within 0.5 points):

| set | + COPY | + buffer access | + tex / Z tiers | all three | speed-up of all three |
|---|---:|---:|---:|---:|---:|
| 12 captured frames, one thread | -11.0 % | -4.0 % | +0.9 % | -13.7 % | 1.16x |
| 12 captured frames, three workers | -6.3 % | -2.8 % | +1.3 % | -11.1 % | 1.12x |
| 15 gameplay windows, one thread | -13.5 % | -3.3 % | -3.2 % | -19.4 % | 1.24x |
| 15 gameplay windows, three workers | -11.0 % | -4.1 % | -1.2 % | -17.1 % | 1.21x |

Reading the tables: **COPY** is a large win where a game uses COPY rectangles at all (Hydro Thunder -62 to -70 %, Mischief Makers -47 to -55 %, Kirby 64 / Paper Mario / Mario Golf -11 to -20 %) and costs nothing elsewhere; **buffer access** is -3 to -7 % of every 3D frame
(it also helps the games that were already 100 % specialised); the **tex / Z tiers** are the big win for the games the lists did not know (Pilotwings 64 -22 %, Earthworm Jim 3D -11 %, Mario Golf -9 %, Toy Story 2 / Turok -2 %: their Z compare was only 20-30 % specialised, but the generic Z compare is just 1.1-1.5x
the variant) and a small cost (+1 %) on frames that were covered already. With three workers the same items give about the same percentages (the two-frame dumps of 2-14 ms are dominated by the thread hand-off and are noisy to +-10 %;
single entries such as Turok's tiers column at three workers are such outliers). Not every 3D frame gets much faster: Banjo-Kazooie at three workers (-0.5 %) and the games whose only gain is the buffer access and a Z compare that was generic before (Quake, Tetrisphere, Ridge Racer 64, Toy Story 2: -3 to -6 % at one thread) are in the single digits.


**Coverage, 62 games** (`CFLAGS=-DFOIR_STATS`, `foir-replay --fast`, the 184 captured 60-frame windows of 62 games, three windows of a game summed, mean over the games; *before* = the M3 build, *after* = this tree; the variant lists of the *after* build were made from the same
statistics, so the "after" column is in-sample: the out-of-sample estimate is the leave-one-game-out figure of [the texture / Z compare section](#texture-and-z-compare-the-second-tier-any-format), texture 99.9 % and Z compare 100 %):

| games | stage | before: in a variant | after: in a variant | of which only in the second tier | lowest game before -> after | games below 90 % before -> after |
|---|---|---:|---:|---:|---|---:|
| 62 games | texture | 91.5 % | **100.0 %** | 0.2 % | 0.1 % -> 100.0 % | 13 -> 0 |
| 62 games | Z compare | 93.5 % | **100.0 %** | -- | 18.5 % -> 98.2 % | 9 -> 0 |
| 62 games | combine (exact + shape) | 100.0 % | **100.0 %** | 22.1 % | 100.0 % -> 100.0 % | 0 -> 0 |
| 62 games | blend | 99.8 % | **99.8 %** | -- | 95.5 % -> 95.5 % | 0 -> 0 |
| 62 games | store | 100.0 % | **100.0 %** | -- | 98.2 % -> 98.2 % | 0 -> 0 |
| 39 unseen games | texture | 89.5 % | **100.0 %** | 0.2 % | 0.1 % -> 100.0 % | 10 -> 0 |
| 39 unseen games | Z compare | 89.8 % | **99.9 %** | -- | 18.5 % -> 98.2 % | 9 -> 0 |
| 39 unseen games | combine (exact + shape) | 100.0 % | **100.0 %** | 29.0 % | 100.0 % -> 100.0 % | 0 -> 0 |
| 39 unseen games | blend | 99.8 % | **99.8 %** | -- | 95.5 % -> 95.5 % | 0 -> 0 |
| 39 unseen games | store | 99.9 % | **99.9 %** | -- | 98.2 % -> 98.2 % | 0 -> 0 |
| 23 test ROMs | texture | 95.1 % | **100.0 %** | 0.2 % | 27.1 % -> 100.0 % | 3 -> 0 |
| 23 test ROMs | Z compare | 99.6 % | **100.0 %** | -- | 91.6 % -> 100.0 % | 0 -> 0 |
| 23 test ROMs | combine (exact + shape) | 100.0 % | **100.0 %** | 10.4 % | 100.0 % -> 100.0 % | 0 -> 0 |
| 23 test ROMs | blend | 100.0 % | **100.0 %** | -- | 99.0 % -> 99.0 % | 0 -> 0 |
| 23 test ROMs | store | 100.0 % | **100.0 %** | -- | 100.0 % -> 100.0 % | 0 -> 0 |

| game | texture before -> after | Z compare before -> after |
|---|---|---|
| Earthworm Jim 3D | 0.1 % -> 100.0 % | 100.0 % -> 100.0 % |
| Hydro Thunder | 2.9 % -> 100.0 % | 98.8 % -> 100.0 % |
| Toy Story 2 Buzz Lightyear to the Rescue | 100.0 % -> 100.0 % | 18.5 % -> 100.0 % |
| Turok Dinosaur Hunter | 92.4 % -> 100.0 % | 26.3 % -> 100.0 % |
| GoldenEye 007 | 27.1 % -> 100.0 % | 100.0 % -> 100.0 % |
| Pilotwings 64 | 42.5 % -> 100.0 % | 100.0 % -> 100.0 % |
| Quake | 100.0 % -> 100.0 % | 44.5 % -> 100.0 % |
| Tetrisphere | 83.0 % -> 100.0 % | 48.5 % -> 100.0 % |
| Mario Golf | 54.3 % -> 100.0 % | 99.2 % -> 100.0 % |
| Star Wars Episode I Racer | 86.6 % -> 100.0 % | 66.8 % -> 100.0 % |
| Wipeout 64 | 100.0 % -> 100.0 % | 68.1 % -> 100.0 % |
| RR64 Ridge Racer 64 | 100.0 % -> 100.0 % | 69.7 % -> 100.0 % |
| Dr Mario 64 | 76.6 % -> 100.0 % | 100.0 % -> 100.0 % |
| Legend of Zelda The Ocarina of Time | 79.7 % -> 100.0 % | 100.0 % -> 100.0 % |
| Iggy s Reckin Balls | 82.3 % -> 100.0 % | 81.9 % -> 100.0 % |
| Banjo Tooie | 84.3 % -> 100.0 % | 99.5 % -> 99.6 % |
| Quest 64 | 100.0 % -> 100.0 % | 85.6 % -> 100.0 % |
| Paper Mario | 88.8 % -> 100.0 % | 100.0 % -> 100.0 % |
| Conker s Bad Fur Day | 90.0 % -> 100.0 % | 98.2 % -> 98.2 % |

**In game: did the hotspots move?** rsp-hle + vectorised parallel-rsp fallback, `Threads = 3`, default knobs, the M3 plugin and this one one after the other in the same session; `perf record -F 499 -p <pid>` for 30 s starting 18 s after the launch
(Paper Mario: the intro and menus, a 16.7 ms frame in both runs; Rocket: its demo loop, whose scene differs a little between the two runs because the game runs in real time). Shares of foir's CPU time without the idle spin of the workers (the metric of
`test-output/m3/profile/`); the profiles are noisy to about +-10 % between runs, so only the large entries mean something:

| share of foir's CPU time (without the idle spin of the workers) | Paper Mario before | Paper Mario after | Rocket before | Rocket after |
|---|---:|---:|---:|---:|
| COPY: `scalar_copy` + its TMEM readback (`tmem_fetch_texel`, `tmem_tlut_raw`) before, `neon_copy` after | 20.4 % | 6.0 % | 13.1 % | 3.1 % |
| `ld16` / `st16` (out of line before, inlined after) | 7.3 % | 0.0 % | 10.6 % | 0.0 % |
| `spec_zcmp_*` (+ inlined buffer access after) | 5.2 % | 7.3 % | 6.4 % | 10.9 % |
| `spec_store_*` (+ inlined buffer access after) | 3.1 % | 8.6 % | 1.9 % | 5.3 % |
| texture | 21.7 % | 25.9 % | 28.6 % | 33.4 % |
| combine | 7.1 % | 8.6 % | 5.1 % | 6.0 % |
| blend | 4.1 % | 5.2 % | 2.8 % | 3.7 % |
| batch build (`raster_render_job`) | 17.4 % | 22.6 % | 17.1 % | 20.1 % |
| parse / setup / TMEM | 7.3 % | 9.1 % | 8.0 % | 9.6 % |
| CPU time of the whole process per frame, last three 120-frame windows (ms) | 31.8 / 32.0 / 32.0 | 30.3 / 30.3 / 28.9 | 19.2 / 20.2 / 25.2 | 18.4 / 23.8 / 24.5 |

The COPY rectangles fall from 20 % to 6 % (Paper Mario) and from 13 % to 3 % (Rocket) of foir's time, i.e. from 3.5 to 0.9 ms of CPU per frame in Paper Mario; `ld16` / `st16` are gone from the profile (their work is in the zcmp / store kernels now: together 15.6 -> 15.9 % of a foir
total that is 11 % smaller in Paper Mario, 18.9 -> 16.2 % in Rocket); the batch builder grows by about 0.5 ms per frame (the prefetches are issued there). The process uses 32.0 -> 29.8 ms of CPU per frame in Paper Mario (-6.7 %), the emulation thread's wait for the workers at
`SYNC_FULL` falls from 2.8 to 2.1 ms. Both games were at the 60 fps cap before; the gain is CPU headroom (and battery), not frame rate. Rocket's frames are not comparable one to one (different scenes in the two runs), its profile only shows where the time moved.


### Batch builder: edge-zone coverage and fixed-size ramps

The batch builder (`raster_render_job` and what it inlines, `src/raster.c`) was the second largest item of the in-game profile (17-23 % of foir's CPU time) and, at one thread, instruction bound
(IPC 0.95, about 800 cycles per span plus 17 per pixel; spans are 9 pixels long in Donkey Kong 64, 17-18 in Snap and Rocket, 40-50 in Paper Mario and Banjo-Kazooie). Inside it the cycles went to
`ramp4` 26 %, `compute_cvg` 20 % (about 214 instructions per span: four byte read-modify-write loops per span, a `memset`, `leftcvghex` / `rightcvghex`), `fill_cvg` 15 %, `fill_idx` 10 %,
`prefetch_piece` 8 % and the rest of `render_span_cycle` 16 %, and the function had ~40 % of all branch mispredictions of a replay. Two changes, each measured on its own and together:

* **Coverage from the edge zones (`cvg_edges_init`, `cvg_mask_at`, `cvg_patch`).** Subscanline *i* covers the pixels strictly between its edges completely and the two edge pixels with the partial mask of `leftcvghex` /
  `rightcvghex`; as a mask that is `A_i(x) & B_i(x)` with `A_i = x > le_i ? F_i : x == le_i ? L_i : 0` and `B_i = x < re_i ? F_i : x == re_i ? R_i : 0` (`F_i`: the subscanline's bits of the 8-bit mask), which also gives
  the old answer for `le_i == re_i` (`L & R`) and for crossed edges (nothing); an invalid subscanline has no bits. The span runs from the smallest valid left edge to the largest valid right edge, so every pixel right of all left
  edges and left of all right edges has one and the same mask (all bits of the valid subscanlines: `0xff`, or less when `invalyscan` is set). The interior therefore needs no coverage computation at all: its `cvg`, `cvbit`,
  `offx`, `offy` are four constant vector stores (splatted from `cvarray[mask]` with one `ld4r`), and only the pixels of the two edge zones (up to the largest left edge, from the smallest right edge; normally one or two
  pixels each, more for shallow edges) are evaluated and patched in afterwards: the four subscanlines in the four lanes of a vector (edge pixel, `F`, `L`, `R` per lane, the `L` / `R` values from a 32-byte `vqtbl2q` table), a few
  compares, a select, a horizontal add, then `cvarray[mask]`. The per-scanline coverage buffer, its byte loops and the `memset` are gone from the NEON path (the portable builder keeps `compute_cvg`; it is the reference).
  `all_full` is cleared per piece when the interior mask is not `0xff` or an edge pixel is not.
* **Fixed-size ramps (`piece_fill`).** `fill_idx`, `fill_cvg` and the eight `ramp4` loops (nine variable-trip loops per piece, each ending in a data-dependent branch) are one loop over groups of four lanes in which the
  ramps of all channels advance together (one exit branch per piece), plus whole-vector (16-lane) stores for the byte arrays; the arrays have 16 lanes of slack (`FOIR_LANES = FOIR_BATCH + 16`, a piece starts below lane 64
  and rounds up to at most lane 79) and the lanes past the piece are ignored. The channel set (shade rgb + alpha / alpha only, Z, texture) is a compile-time constant of each of 12 instances selected by one `switch` per
  piece, so the body has no per-channel branches and the independent channels interleave.

The old and the new builder are both in the tree: `render_span_ref` (portable C, with `compute_cvg`) builds the batches of the scalar renderer and is the reference of `foir-kfuzz -m 64` (below).

**Timing.** `foir-replay --fast` (the plugin's default knobs) on the rgdsplus, `performance` governor (restored to `schedutil`), the builds of one session run alternately (the order rotated every round), median of 5 rounds
of `-n 8..150` repeats per dump and thread count; *before* = `devel` HEAD built from a clean copy, *+ coverage zones* = only the coverage change (the old `ramp4` / `fill_idx` loops), *+ ramps* = only the new ramps (old `compute_cvg` /
`fill_cvg`), *both* = this tree. Same flags (`-O3`, generic armv8-a). Spread between rounds: < 1 % at one thread, up to 10-15 % with three workers.

One thread (`-t 0`), ms per run:

| dump | before | + coverage zones | + fixed-size ramps | both (this tree) |
|---|---:|---:|---:|---:|
| Paper Mario (900) | 12.52 | 12.16 (-2.9 %) | 12.31 (-1.7 %) | 11.95 (-4.6 %) |
| Banjo-Kazooie (450) | 36.86 | 36.12 (-2.0 %) | 36.12 (-2.0 %) | 35.27 (-4.3 %) |
| Donkey Kong 64 (800) | 16.68 | 15.76 (-5.5 %) | 16.61 (-0.4 %) | 15.68 (-6.0 %) |
| Pokemon Snap (300) | 75.91 | 73.60 (-3.0 %) | 75.14 (-1.0 %) | 72.16 (-4.9 %) |
| Rocket Robot on Wheels (1500) | 28.49 | 27.20 (-4.5 %) | 27.96 (-1.8 %) | 27.01 (-5.2 %) |
| Super Mario 64 (650) | 20.98 | 19.98 (-4.8 %) | 20.37 (-2.9 %) | 19.21 (-8.4 %) |
| Star Fox 64 (1000) | 18.70 | 18.22 (-2.6 %) | 18.54 (-0.8 %) | 18.15 (-2.9 %) |
| GoldenEye 007 (900) | 6.43 | 5.95 (-7.6 %) | 6.45 (+0.3 %) | 6.03 (-6.2 %) |
| **geometric mean of the ratios to "before"** | 1 | **0.959** | **0.987** | **0.947** |

Three workers (`-t 3`; the parser thread is the other bottleneck, so only part of the worker time shows; noisy):

| dump | before | + coverage zones | + fixed-size ramps | both (this tree) |
|---|---:|---:|---:|---:|
| Paper Mario (900) | 5.06 | 4.99 (-1.3 %) | 4.92 (-2.6 %) | 5.32 (+5.1 %) |
| Banjo-Kazooie (450) | 14.01 | 12.77 (-8.9 %) | 12.56 (-10.3 %) | 12.34 (-11.9 %) |
| Donkey Kong 64 (800) | 7.12 | 6.99 (-1.8 %) | 7.52 (+5.6 %) | 6.56 (-7.8 %) |
| Pokemon Snap (300) | 29.62 | 26.77 (-9.6 %) | 28.26 (-4.6 %) | 25.63 (-13.5 %) |
| Rocket Robot on Wheels (1500) | 11.59 | 11.46 (-1.1 %) | 11.31 (-2.4 %) | 10.57 (-8.8 %) |
| Super Mario 64 (650) | 8.51 | 7.80 (-8.4 %) | 7.58 (-10.9 %) | 7.23 (-15.1 %) |
| Star Fox 64 (1000) | 7.62 | 8.23 (+8.0 %) | 7.39 (-3.1 %) | 8.07 (+5.8 %) |
| GoldenEye 007 (900) | 2.93 | 2.65 (-9.5 %) | 2.93 (+0.0 %) | 2.67 (-9.1 %) |
| **geometric mean of the ratios to "before"** | 1 | **0.957** | **0.963** | **0.928** |

A second session with only *before* and *both* (8 more rounds, 13 rounds pooled) gives the three-worker medians 5.06 -> 5.09 (Paper Mario), 13.42 -> 12.41 (Banjo-Kazooie), 7.18 -> 6.56 (Donkey Kong 64), 29.62 -> 26.05 (Pokemon Snap), 11.64 -> 10.57 (Rocket Robot on Wheels), 8.33 -> 7.27 (Super Mario 64), 7.59 -> 7.92 (Star Fox 64), 2.93 -> 2.73 (GoldenEye 007): geometric mean **0.933 (-6.7 %)**.
Paper Mario and Star Fox 64 do not move at three workers (their frames are limited by the parser thread, 5 ms and 8 ms); single entries change by +-5 % between sessions.
At one thread the gain is **-5.3 %** of the replay time (1.06x), -4.1 % of it from the coverage change; the ramps alone are worth -1.3 % (+0.3 % in GoldenEye, whose spans are the shortest) and add -1.2 % on top of the coverage change.

**Counters** (`perf stat`, one thread, `performance` governor; whole replay per run, by difference of `-n 4` and `-n 24`; "per span": instructions saved per span of the dump, all of it in the batch builder and the libc `memset` it called):

| dump | instructions before -> both | branch mispredictions before -> both | cycles before -> both |
|---|---:|---:|---:|
| Paper Mario (900) | 21.28 M -> 19.81 M (-6.9 %), 449 per span | 77 K -> 49 K (-37 %) | 25.97 M -> 24.82 M (-4.4 %) |
| Banjo-Kazooie (450) | 59.66 M -> 55.76 M (-6.5 %), 615 per span | 224 K -> 128 K (-43 %) | 63.78 M -> 62.68 M (-1.7 %) |
| Donkey Kong 64 (800) | 27.42 M -> 25.26 M (-7.9 %), 259 per span | 124 K -> 84 K (-33 %) | 32.49 M -> 30.93 M (-4.8 %) |
| Pokemon Snap (300) | 119.60 M -> 110.04 M (-8.0 %), 387 per span | 682 K -> 386 K (-43 %) | 127.04 M -> 119.78 M (-5.7 %) |
| Rocket Robot on Wheels (1500) | 44.36 M -> 40.83 M (-7.9 %), 385 per span | 242 K -> 146 K (-40 %) | 51.28 M -> 49.01 M (-4.4 %) |

Inside `raster_render_job` alone (sampled `instructions` / `cycles` / `br_mis_pred` of the -g build, divided by the 31 passes and the span count of the dump; sampling error 5-10 %; the `memset` of the old
`compute_cvg` is a libc call and not counted in *before*):

| dump (spans per run) | instructions per span | cycles per span | mispredictions per span | share of all cycles |
|---|---:|---:|---:|---:|
| Paper Mario (3,272) | 1075 -> 782 | 1124 -> 862 | 9.6 -> 3.0 | 13.2 % -> 10.4 % |
| Banjo-Kazooie (6,334) | 1414 -> 996 | 1605 -> 1215 | 18.0 -> 5.6 | 15.2 % -> 12.0 % |
| Donkey Kong 64 (8,360) | 708 -> 498 | 702 -> 534 | 6.3 -> 2.4 | 16.9 % -> 13.4 % |
| Pokemon Snap (24,686) | 1006 -> 663 | 1064 -> 777 | 13.9 -> 4.3 | 19.7 % -> 15.2 % |
| Rocket Robot on Wheels (9,149) | 963 -> 651 | 1067 -> 777 | 12.4 -> 3.8 | 17.9 % -> 13.7 % |

What is left inside (Donkey Kong 64, ~500 instructions and ~530 cycles a span, rough split of the samples): the span set-up and the piece bookkeeping a third, `piece_fill` (about 45 vector stores per 16-lane piece) a quarter,
the two edge-zone pixels 2.6 per span and `cvg_edges_init` together a quarter, `prefetch_piece` 9-13 %, the row loop 6 %.

**Tried and left out** (each a build of its own against the one it would extend, same method, 3-5 rounds; differences below ~1 % are noise):
* *16-lane blocks for the ramps* (the first version of the ramps: whole 16-lane vectors for every array, a second pass only for pieces longer than 16 pixels): the loop over groups of four lanes above is 0.9 % / 0.7 % faster
  in two sessions of five rounds (Donkey Kong 64 -1.5 %, GoldenEye -2 to -2.4 %, Rocket -1 %, SM64 -2 %, Star Fox 64 +1 %); the 16-lane version alone was worth -0.8 %.
* *Per-channel run-time branches instead of the 12 instances* (the same block with `if (f_rgb)` ...): +0.7 % slower geometric mean over the 8 dumps (5 rounds, 16-lane version); the 12 instances are kept.
* *Splatting channels whose step is zero* (store the base vector four times, no adds): no gain (Donkey Kong 64 -0.9 %, Banjo-Kazooie -0.9 %, Paper Mario 0.0 %, Snap -0.1 %, Rocket 0.0 %), although the
  steps are zero often (Banjo-Kazooie: r, b and a in 99-100 % of the pieces, g in 82 %; Paper Mario: r, g, b 55 %, a 77 %, t and w 73 %; Donkey Kong 64: t and w 27 %; Snap: a 67 %) -- the add is not what the ramp costs.
* *Eight-lane half blocks* for pieces of up to 8 pixels (one more branch per piece): same instruction count, Donkey Kong 64 +2.1 %, Paper Mario +0.2 %, Snap -1.6 % (16-lane version): not kept.
* *`render_span_cycle` out of line* (it is inlined into the job loop and keeps ~20 vector registers live): +0.7 / +0.6 / +1.3 % (Paper Mario, Donkey Kong 64, Snap; 16-lane version).
* *Prefetch* (`prefetch_piece`, ~50 instructions a piece): switching it off costs Donkey Kong 64 **+3.8 %** (Paper Mario -0.6 %, Snap 0.0 %), so it still pays; cheaper versions -- first and last line of each range only, or
  without the hidden-bit lines -- are equal within noise (Donkey Kong 64 +0.7 % / -0.6 %) and were not taken over. Issuing it per span instead of per piece changes nothing (a span is 1.1 pieces on average).
* *Visiting only the worker's own stripes* in the row loop instead of testing `(y >> 2) % nw` for every row (one integer division per row, 3 of 4 rows skipped by each of three workers): no difference at three workers
  within the noise (-6.4 % with the stripe walk, -7.6 % with the per-row test, 5 rounds, 16-lane version); not kept.

**Exactness.** The builder was not covered by the existing checks on aarch64: `foir-replay --scalar` and the other "reference" configurations of `neoncheck.py` / `mtfuzz.py` replace the kernels, not the batch builder, so a wrong NEON
builder makes the reference and the candidate wrong in the same way (the mutations below pass `neoncheck.py`). Three checks cover it now:
* `foir-kfuzz -m 64` (new; `make check` runs 40,000 iterations): random primitives and rows of edge-walker spans -- four subscanlines with 13-bit edge positions in the shapes of steep, shallow, wide, thin and one-pixel spans, crossed edges,
  invalid subscanlines (also all four), both directions, spans that run off the row end, 16-1015 pixel wide framebuffers, random pipe flags (shade rgb / alpha, Z, texture), random interpolant steps (zero, small, huge), `z_source_sel`, 1-4 workers --
  through `raster_render_job` and through `raster_render_job_ref` with the pixel stages replaced by a recorder; every batch (all lanes of `fbidx`, `ditidx`, `xpos`, the enabled ramps, `cvg`, `cvbit`, `offx`, `offy`, `n`, `all_full`,
  `fbmin`, `fbmax`) must be identical, the rows rendered by a worker must belong to its stripes and the workers together must render as many pixels as one context. 40,000 iterations: 84,681 batches compared, 70,743 of them with partially covered pixels, **0 differences**.
* End to end across architectures: the final RDRAM of every one of the 12 captured frames from the x86_64 scalar build (`NEON=0`, portable builder) and from the aarch64 NEON build (qemu), inline and with 3 workers: **24 of 24 byte-identical**.
* `make all replay ref NEON=0 && python3 tools/regress.py` (the portable builder against angrylion): 15 of 15 scenes, 0 differing pixels.

Also run: `make check` as a whole (`neoncheck.py`: 12 captured frames + 15 synthetic scenes, 27 scenes, 0 differing bytes in every configuration; `mtfuzz.py` 60 + 40 + 40 scenes, 0 differences; `foir-kfuzz`, `-m 32`, `-S`, `-S -T 5`,
`-S -T 0`: 0 differences) and `FOIR_NEON_VERIFY=1 FOIR_KNOBS=exact` on the 12 captured frames at 0 and 3 workers: 504 K batches over the five stages, **0 mismatching**.

**Mutations** (a deliberately wrong version is built; `foir-kfuzz -m 64`, the aarch64-vs-x86 cross check and `neoncheck.py` on the synthetic scenes are run on it):

| mutation | `kfuzz -m 64` | cross check (24 renders) | `neoncheck.py` synthetic scenes |
|---|---|---|---|
| right-edge value of subscanline 1 at `x & 7 == 5`: bit `0x40` flipped in `cvg_rtab` | 20 failing iterations within 353 | 14 of 24 differ | 0 differences |
| interior mask: `0x05` -> `0x04` in the mask of subscanline 3 | 20 within 46 | 14 of 24 differ | 0 differences |
| Z ramp of a piece built from the `w` step instead of the `z` step (`dI[7]` -> `dI[6]`) | 20 within 67 | 12 of 24 differ | 0 differences |

(A change of the right-edge value that only touches bits outside the subscanline's own mask is not a mutation: `A & B` hides it, as it should -- a first attempt, `^ 1`, passed everything.)

### Edge walker on the workers (`walk_row`)

The edge walker (`walk_edges` of `setup.c`) walked every subscanline of every primitive on the emulation thread, inside parallel-rsp's `ProcessRdpList` callback, and wrote one 96-byte `struct span` per scanline into an arena
that the workers, on other cores, then read. In the in-game profiles it was 2.8-10 % of foir's CPU time (mean 6.5 %, Paper Mario 10.1 %), about 60 % of the emulation thread's own RDP work, and the span records
were a source of cross-core stalls in `raster_render_job` (backend load stalls 16-23 % of its cycles). The emulation thread's time in the RDP per frame was 5.5 ms in Donkey Kong 64, 3.6 Banjo-Kazooie, 3.3 Paper Mario, 2.6 Mario Kart 64, 2.4 Super Mario 64,
1.9 Ocarina of Time, 1.8 Star Fox 64.

**Design.** The span geometry of a primitive is a pure function of its edge coefficients and the scanline (see [Threading](#threading) for the argument), so the parser now only *decodes*: `setup_walk()` (`setup.c`) turns the 44-word edge record
into the per-primitive constants (`r->pc`, as before) and a **walker record**, `struct prim_walk` (192 bytes: the start, step and middle-vertex switch of the three edges, the scissor, the interpolants at the first row with their per-row steps and the two adjustment terms,
the interlace field). The record travels in the job (`mt.c`; the 16 MB span arena is gone, the job ring grew from 208 to 368 bytes per job). Each worker calls `walk_row()` (`src/walk.h`) for the rows of its stripes -- row `y` still belongs to worker `(y >> 2) % N` --
into a local `struct span` and feeds the same batch builder as before; the inline path (`-t 0`, `Threads = -1`) does the same on the emulation thread. `walk_row` computes the four subscanlines in the four lanes of NEON vectors
(positions `start + k * step` with the left edge taken from the lower-edge formula from the middle vertex on, the clipping and crossing tests lane-wise, horizontal min / max / flags), the interpolants as two vectors of four
(`a0 + (j - j0) * step`, masked, adjusted by the sub-pixel position of the right edge), and returns early for an invalid row without touching the interpolants; `walk_row_c` is the same in portable C for the x86 / scalar build. All arithmetic
is unsigned 32-bit, so a jump by multiplication equals the sequential additions modulo 2^32 for any coordinates, however extreme.
What `prefetch_piece` and the batch builder expect from `struct span` is unchanged (`r g b a s t w z` are now also an array, `att[8]`, for the vector store).

**Memory traffic** (per replay run of the dump = one frame; 96 B per span record, 192 B per primitive):

| dump | primitives | scanlines (valid) | span records written by the parser, before | read by the workers, before | walker records, after (written once, read by each of the 3 workers) |
|---|---:|---:|---:|---:|---:|
| Paper Mario | 183 | 4523 (4314) | 424 KB | 404 KB | 34 KB |
| Banjo-Kazooie | 156 | 6811 (6766) | 639 KB | 634 KB | 29 KB |
| Donkey Kong 64 | 531 | 10382 (10242) | 973 KB | 960 KB | 100 KB |
| Pokemon Snap | 1214 | 27075 (26606) | 2538 KB | 2494 KB | 228 KB |
| Rocket Robot on Wheels | 870 | 10428 (9573) | 978 KB | 897 KB | 163 KB |
| Super Mario 64 | 823 | 10364 (10088) | 972 KB | 946 KB | 154 KB |
| Star Fox 64 | 475 | 3534 (3493) | 331 KB | 327 KB | 89 KB |
| GoldenEye 007 | 172 | 5771 (5717) | 541 KB | 536 KB | 32 KB |

The parser used to write every scanline of every primitive (plus a pass that cleared the `validline` of each row of the arena) and the workers read the valid ones from another core's cache; now each primitive is one record in the job, which the three workers read (3 x 192 B each),
and the spans are made and consumed in the registers and L1 of the core that renders the row.

**Exactness.** The walker is not exercised against a reference by the replay-based checks (`neoncheck.py`, `mtfuzz.py` and `foir-kfuzz` compare the NEON or threaded renderer with the scalar one, and both sides use the same walker; the mutations below pass them), so the walker has its own test:
`foir-walkfuzz` ([Tests](#tests)) compares `walk_row` / `walk_row_c` with the sequential walker of commit c73eb8e, kept verbatim in `tools/walk_ref.inc`, over random primitives: `make check` runs 40,000, and 1,000,000 (seed 7) were run on x86_64 and on aarch64 (qemu): 543,842 visible
primitives, 156,777,712 scanlines compared (60,795,882 valid, 30.7 M with a wrapped edge product, 54.4 M with a wrapped interpolant product, 74,766 with the left-edge switch inside the row, 8.7 M on a scissored interlace field) by both walkers, **0 differences**.
Also, all at 0 differences: `make check` (`neoncheck.py` 27 scenes, `mtfuzz.py` 60 + 40 + 40 scenes, `foir-kfuzz` including `-m 64` and `-S`); `make all replay ref NEON=0 && python3 tools/regress.py` (15 of 15 scenes, 0 differing pixels);
`FOIR_NEON_VERIFY=1` on the 13 dumps at 0 and 3 workers (26 runs, 538,438 batches, 0 mismatching); the final RDRAM of every dump from the x86_64 scalar build and from the aarch64 build, inline and with 3 workers (26 comparisons: 0 differ);
the final RDRAM of the aarch64 build against a build of the previous commit (26 comparisons: 0 differ); ASan / UBSan on `foir-walkfuzz` and the replay with 3 workers, and ThreadSanitizer on the replay with 3 workers on four dumps: no reports.

**Mutations** (a deliberately wrong walker is built in a copy of the tree, in both the NEON and the portable version; the tests are run on it):

| mutation | `foir-walkfuzz` (first failing primitive) | `regress.py` (scenes with 0 differing pixels) | RDRAM vs previous commit (26 runs) | `neoncheck.py`, `mtfuzz.py`, `kfuzz -m 64` |
|---|---|---|---|---|
| edge positions jump one subscanline too far (`k0 - ycur + 1`) | fails, primitive 5 | 6 of 15 | 20 differ | 0 differences |
| interpolants jump one row too far (`j - j0 + 1`) | fails, primitive 5 | 4 of 15 | 26 differ | 0 differences |
| left edge switches to `xl` one subscanline late (`k == ym + 1`) | fails, primitive 39 | 12 of 15 | 14 differ | 0 differences |
| `ym == ycur` does not count as "switch inside the walk" (`ym > ycur`) | fails, primitive 57 | 13 of 15 | 18 differ | 0 differences |

**Timing, offline replay** (`foir-replay --fast`, rgdsplus, `performance` governor restored to `schedutil`, build of `devel` HEAD c73eb8e vs this tree, run alternately with the order rotated every round, 6 rounds, median (min-max); one session; `-n 10..100` repeats per run;
the rows of the `-t 3` table are the emulation thread's time in the replay: its own work -- parse, setup, queueing -- and the wait for the workers at `SYNC_FULL`, which in a replay also holds all the pixel work the workers have not yet finished, so the sum is the frame time):

**One thread (`-t 0`)**, ms per run, median (min-max):

| dump | old | new | new vs old |
|---|---:|---:|---:|
| Paper Mario (900) | 12.00 (11.95-12.13) | 11.89 (11.85-11.97) | -0.9 % |
| Banjo-Kazooie (450) | 35.28 (35.16-35.63) | 35.79 (35.60-35.97) | +1.5 % |
| Donkey Kong 64 (800) | 15.75 (15.69-15.79) | 14.89 (14.83-14.95) | -5.4 % |
| Pokemon Snap (300) | 72.52 (72.02-72.67) | 71.56 (70.64-71.71) | -1.3 % |
| Rocket Robot on Wheels (1500) | 26.96 (26.87-27.39) | 26.59 (26.33-26.75) | -1.3 % |
| Super Mario 64 (650) | 19.40 (19.29-19.52) | 18.82 (18.72-18.90) | -3.0 % |
| Star Fox 64 (1000) | 18.15 (17.99-18.18) | 17.95 (17.89-18.10) | -1.1 % |
| GoldenEye 007 (900) | 6.05 (6.01-6.15) | 5.61 (5.57-5.64) | -7.4 % |
| **geometric mean of the ratios** | | | **0.976** |

**Three workers (`-t 3`)**, ms per run, median (min-max); the emulation thread's own work and its wait at `SYNC_FULL`, ms (change of the own work in brackets):

| dump | old | new | new vs old | emulation thread own work old -> new | wait at SYNC_FULL old -> new |
|---|---:|---:|---:|---:|---:|
| Paper Mario (900) | 4.81 (4.66-5.01) | 4.69 (4.54-5.19) | -2.6 % | 1.80 -> 0.80 (-55 %) | 3.00 -> 3.88 |
| Banjo-Kazooie (450) | 11.99 (11.86-12.75) | 12.50 (12.15-13.17) | +4.3 % | 2.34 -> 0.90 (-62 %) | 9.66 -> 11.61 |
| Donkey Kong 64 (800) | 6.69 (6.16-6.77) | 6.33 (6.18-6.87) | -5.5 % | 3.38 -> 1.26 (-63 %) | 3.26 -> 5.06 |
| Pokemon Snap (300) | 26.50 (25.16-26.89) | 25.55 (25.03-27.38) | -3.6 % | 8.70 -> 2.61 (-70 %) | 17.79 -> 22.95 |
| Rocket Robot on Wheels (1500) | 10.59 (10.12-10.82) | 10.26 (10.12-10.88) | -3.1 % | 4.60 -> 2.32 (-50 %) | 5.96 -> 7.95 |
| Super Mario 64 (650) | 7.37 (6.94-7.98) | 7.15 (6.96-7.78) | -2.9 % | 3.58 -> 1.36 (-62 %) | 3.77 -> 5.80 |
| Star Fox 64 (1000) | 7.32 (7.14-7.53) | 7.21 (7.09-7.89) | -1.5 % | 1.86 -> 1.10 (-41 %) | 5.47 -> 6.12 |
| GoldenEye 007 (900) | 2.67 (2.50-2.78) | 2.45 (2.41-2.61) | -8.1 % | 1.60 -> 0.42 (-74 %) | 1.07 -> 2.04 |
| **geometric mean of the ratios** | | | **0.971** | **0.392** | |

The sequential walk cost 173-175 ns a row (~275 cycles; `--setup-only`, old minus new, Paper Mario, Donkey Kong 64 and Snap alike); `walk_row` costs 41-113 ns a row on the real primitives (median 80 ns, ~125 cycles; measured by calling it twice per row
and taking the difference of the one-thread replay times, 4 rounds), and it no longer writes or reads a 96-byte record per row. At one thread that is -2.4 % overall (-5.4 % Donkey Kong 64, -7.4 % GoldenEye, +1.5 % Banjo-Kazooie, the one dump that gets slower). With three workers
the emulation thread's own work falls by 61 % (a replay has nothing else to overlap with, so the wait at `SYNC_FULL` absorbs what the parser no longer does: the workers set the frame time, which is -2.9 % within the noise of that configuration, +-3 %).

**Timing, in game** (rgdsplus, rsp-hle with parallel-rsp as RspFallback, rsp-timing, `FOIR_THREADS=3`, default knobs, scripted input, 75 s runs, VI frames 1440 on, the method of the batch-builder sweep; 3 runs of each build in alternating order, in sessions of three runs;
median (min-max) over the runs; "RDP" is the emulation thread's time in the RDP per frame -- parse + setup + the wait at `SYNC_FULL`; CPU is the whole process, workers' idle spin included):

| game | RDP ms/frame, emulation thread (parse + setup + wait) old -> new | of which own work old -> new | wait at SYNC_FULL old -> new | process CPU ms/frame old -> new | fps old -> new |
|---|---|---|---|---|---|
| Banjo-Kazooie | 3.36 (3.33-3.36) -> **2.64 (2.63-2.66)** (-21 %) | 2.67 (2.67-2.68) -> 1.58 (1.56-1.58) (-41 %) | 0.68 (0.65-0.69) -> 1.06 (1.05-1.10) | 34.37 (34.35-34.37) -> **31.34 (31.26-31.37)** (-8.8 %) | 59.3 (59.3-59.3) -> 59.5 (59.5-59.5) |
| Donkey Kong 64 | 5.28 (5.26-5.36) -> **4.30 (4.29-4.34)** (-18 %) | 3.02 (3.00-3.04) -> 1.34 (1.34-1.35) (-55 %) | 2.27 (2.24-2.32) -> 2.96 (2.94-3.00) | 40.60 (40.59-40.93) -> **36.00 (35.98-36.02)** (-11.3 %) | 59.3 (59.2-59.3) -> 59.8 (59.8-59.8) |
| Mario Kart 64 | 2.23 (2.21-2.25) -> **1.63 (1.63-1.65)** (-27 %) | 2.13 (2.12-2.13) -> 1.49 (1.49-1.52) (-30 %) | 0.11 (0.08-0.12) -> 0.13 (0.13-0.14) | 26.01 (26.01-26.09) -> **23.70 (23.57-23.77)** (-8.9 %) | 59.0 (59.0-59.0) -> 59.0 (59.0-59.0) |
| Paper Mario | 3.06 (2.97-3.07) -> **2.42 (2.36-2.52)** (-21 %) | 2.38 (2.37-2.40) -> 0.98 (0.96-0.99) (-59 %) | 0.67 (0.60-0.68) -> 1.44 (1.39-1.54) | 25.17 (25.02-25.17) -> **22.58 (22.44-22.84)** (-10.3 %) | 59.9 (59.9-59.9) -> 60.0 (59.9-60.0) |
| Pokemon Snap | 2.17 (2.16-2.19) -> **2.19 (2.19-2.20)** (+1 %) | 0.50 (0.49-0.50) -> 0.35 (0.33-0.35) (-31 %) | 1.68 (1.66-1.69) -> 1.85 (1.85-1.86) | 11.80 (11.79-11.80) -> **11.97 (11.91-11.97)** (+1.4 %) | 60.0 (60.0-60.0) -> 60.0 (60.0-60.0) |
| Star Fox 64 | 1.60 (1.58-1.64) -> **1.22 (1.17-1.23)** (-24 %) | 1.17 (1.17-1.17) -> 0.67 (0.67-0.68) (-42 %) | 0.43 (0.42-0.47) -> 0.55 (0.50-0.55) | 20.46 (20.42-20.66) -> **19.22 (19.06-19.23)** (-6.1 %) | 59.8 (59.7-59.8) -> 59.8 (59.8-59.8) |
| Super Mario 64 | 2.11 (2.08-2.13) -> **1.62 (1.61-1.70)** (-23 %) | 1.50 (1.50-1.52) -> 0.59 (0.59-0.60) (-60 %) | 0.61 (0.58-0.61) -> 1.03 (1.01-1.10) | 23.94 (23.88-24.12) -> **21.86 (21.62-22.00)** (-8.7 %) | 59.9 (59.9-59.9) -> 59.9 (59.9-59.9) |
| Toy Story 2 | 1.96 (1.95-1.98) -> **1.98 (1.97-2.02)** (+1 %) | 1.17 (1.17-1.18) -> 1.01 (1.01-1.03) (-14 %) | 0.79 (0.78-0.80) -> 0.96 (0.96-0.99) | 12.86 (12.83-12.87) -> **12.82 (12.81-12.86)** (-0.3 %) | 58.6 (58.5-58.6) -> 58.6 (58.6-58.6) |
| Zelda: Ocarina of Time | 1.84 (1.83-1.87) -> **1.36 (1.34-1.38)** (-26 %) | 1.43 (1.23-1.43) -> 0.78 (0.78-0.78) (-45 %) | 0.41 (0.40-0.64) -> 0.58 (0.56-0.60) | 17.89 (15.86-17.90) -> **16.01 (15.98-16.05)** (-10.5 %) | 59.9 (59.7-59.9) -> 60.0 (59.9-60.0) |
| **geometric mean of the new / old medians** | **0.817** | 0.562 | | **0.929** | |

* The emulation thread's RDP time falls by 18-27 % in seven of the nine games (geometric mean of the medians of all nine **-18 %**), its own work (parse + setup + queueing) by 30-60 % (**-44 %** over the nine), and the process CPU time per frame by 6-11 % in the same seven games
  (**-7 %** over the nine). The CPU saving is larger than the emulation thread's (1.3-4.6 ms against 0.5-1.0 ms a frame): the other threads' CPU time falls by 1.5-3.5 ms a frame too, with the walk now on them -- most likely the stalls on span records written by another core
  (and the idle spin that follows the faster job hand-over). All nine games run at the frame-rate cap (59-60 fps, Toy Story 2 58.6); what changes is the headroom: Donkey Kong 64 goes from 5 slow 120-frame windows of 24 to 1 of 25 (59.3 -> 59.8 fps), Banjo-Kazooie 59.3 -> 59.5 fps.
* The wait at `SYNC_FULL` grows by about what the own work shrinks wherever the workers are what the emulation thread waits for at the end of the frame: Pokemon Snap (RDP-bound; RDP 2.17 -> 2.19 ms, process CPU 11.80 -> 11.97 ms, +1.4 %) and Toy Story 2 (1.96 -> 1.98 ms, CPU -0.3 %) do not gain
  on the emulation thread, because the frame ends when the workers have rendered it, and the workers now also walk. In the other games the wait is mostly the RSP finishing, not the RDP, and the emulation thread gets the time back.
* Zelda old run 1 is the scripted run that took a different path (cpu 15.9 against 17.9 ms in the other two, the same outlier as in the batch-builder sweep); medians are used.

**Tried and left out** (all with the same replay method, 3-4 rounds, geometric mean of the new / old ratios of the eight dumps; the in-game numbers are only for the version that is in the tree):

| variant | replay `-t 0` | replay `-t 3` | why not |
|---|---:|---:|---|
| `walk_row_c` (the portable C walker, inline in the row loop) on aarch64 | +1.2 % / +0.9 % (two sessions) | +2.0 % / +1.8 % | jumping by multiplication in scalar code costs 190-320 ns a row on the A55 (the sequential walker, which adds, is 175; in-order core, four dependent chains per row), so the vector version is the one kept; the portable version stays for x86 / the scalar build |
| the same, gathering the rows of 16 into a worker-local span array first (`noinline` gather function; nothing is written for other cores, but the 96-byte rows go through L1) | +3.8 % | +3.4 % | the extra copy costs more than the registers it frees |
| NEON walker + the gather of 16 | -1.0 % | +2.0 % | -2.3 % for the inline version in the same session |
| **NEON walker inline in the row loop (kept)** | **-2.3 % / -2.4 %** (two sessions) | +0.6 % / -2.9 % | |
| decode of the 44-word record on the workers instead of on the parser (job = the 176-byte record + scissor) | not built | not built | upper bound measured: skipping the whole decode (`setup_walk`) in `--setup-only` saves 0.04-0.22 ms per dump (Paper Mario 0.04, Donkey Kong 64 0.08, Star Fox 64 0.11, Super Mario 64 0.14, Rocket 0.17, Snap 0.22 ms), i.e. at most 2-8 % of the emulation thread's remaining RDP time, in exchange for decoding every primitive three times; not worth the second copy of the state the decode reads (scissor, cycle type, interlace field, seed) |

### LOD / mip-map texture path

**The request and what was found.** The in-game profile of the batch-builder sweep (`test-output/sweep-bb1/analysis/perf_shares.txt`) lists "texture helpers" -- `lod_group`, `tex_cycle*`, `fetch_decode`, `neon_texture` -- as 31.4 % of foir's CPU time in Banjo-Kazooie, 23.8 % in
Donkey Kong 64 and 5.9 % in Paper Mario, and they had been read as scalar LOD code. **They are not scalar.** Everything on the LOD path was already NEON: `lod_group` computes the level of detail of eight pixels in `int32x4_t` halves, the texture cycles run per tile on the lanes that selected it.
The names are the out-of-line (not inlined into a variant) NEON functions that the *generic* texture kernel `neon_texture` calls. Two things sent most LOD batches there (statistics build, `-DFOIR_STATS`, 6 gameplay dumps captured at VI frames 1800 / 2400 / 3000 of Banjo-Kazooie and Donkey Kong 64
-- the 12 old dumps hold almost no mip-mapped draws -- plus the three old dumps with a LOD):

* **The variants checked too many tiles.** A `TEXL` / `TEXAL` variant requires every tile the LOD can select to have the texel format it was built for, and checked tiles `tilenum .. tilenum + max_level + 2`. The LOD selects at most `tilenum + max_level` (cycle 1 is level + 1 unless distant or magnified, both
  capped at the max level), and `+ 1` with detail textures. The two extra tiles are whatever the game left in its tile descriptors (in these dumps the tiles after the chain have other formats), the check failed, and `tex_generic()` handed the batch to the generic kernel, which is 1.4-1.8x the cost of a variant:
  **60-62 % of the LOD batches in the three Banjo-Kazooie dumps, 46 % in Donkey Kong 64 (3000), 8-15 % in its other dumps, 8 % in Rocket**, with all their pixels. In the profile that is `neon_texture` + `tex_cycle` + `fetch_decode` + `tex_cycle_tiles`: 18.8 % of Banjo-Kazooie's foir time.
* **Point-sampled LOD had no variant.** Donkey Kong 64 draws part of its scenes with TEX_LOD_EN and SAMPLE_TYPE = point (IA8, RGBA16 with perspective; I4 without): the keys were in neither the exact list nor the "any format" list (which only had bilinear LOD forms): 39.5 % (2400) and 21.5 % (800) of the textured pixels of those dumps ran in the generic kernel.

What the real LOD draws are (6 dumps + Rocket + GoldenEye 007): all TEX_LOD_EN; no detail or sharpen; perspective except a few DK64 draws; two-cycle in Banjo-Kazooie (cycle 0 and 1 on adjacent levels, blended by the LOD fraction), one-cycle in GoldenEye and part of DK64; bilinear except DK64's point-sampled ones;
Banjo-Kazooie CI4 + TLUT 79 % of its LOD batches (frame 2400), RGBA16 11 %, RGBA32 8 %, CI8 1 %; DK64 (3000) IA8 49 %, RGBA16 21 %, CI4 + TLUT 20 %, RGBA32 9 %, I8 2 %. **Tile mix per group of eight lanes:** all eight lanes select the same tile of cycle 0 in 98.5-99.6 % of the groups (Banjo-Kazooie 2400 99.6 %, DK64 3000 98.5 %), the same tile of cycle 1 in 99.5 % / 93.1 %;
the two cycles' tiles together are one tile in 86-91 % of the groups, two adjacent ones in 8-12 %, three in 0.4-1.4 %. A lane mix is therefore not the case to build for; it is the case to keep correct and out of line.

**Changes** (`src/kneon_tex.c`, `src/kspec_tex.inc`, `src/spec_tex.def`; everything bit-exact with the scalar reference):

1. **The chain bound** is `lod_chain_last() = max_level + (detail ? 1 : 0)` in the variants' check and in the YUV test of `neon_texture`. Alone: replay -8 % (Banjo-Kazooie) / -8 % (DK64 3000).
2. **`lod_pass()`: the LOD of the whole batch in one out-of-line call** (4 instances: perspective x tile selection; was one `lod_group` call per group of eight). Phase 1 stores the divided coordinates of every lane (the texture cycles take their `sss` / `sst` from there, the divide of the main loop is gone for LOD kernels).
   Phase 2, per group: the *next pixel* divide (`s + dsinc`, ...) is taken from the neighbouring lane's phase-1 result (lane + 1 for `flip`, else lane - 1) **when that lane really holds `s + dsinc`, `t + dtinc`, `w + dwinc`** (three vector compares; a divide is a function of its inputs, so the reuse is exact whatever the batch looks like). Only a half-group that has a lane
   at a scanline-run boundary or at the batch end divides again (20-25 % of the half-groups in the Banjo-Kazooie / DK64 gameplay dumps, 63-69 % in the two dumps with very short spans, DK64 800 and GoldenEye), the *row below* divide is always computed. The four |differences| of the 17-bit sign/magnitude coordinates come from one halving subtract each (`vhsubq`: the 18-bit difference << 14, so no overflow; a negative difference is flipped by `v ^ (v >> 31)`),
   their maximum is saturated to 15 bits, and the rest (`lodfrac_lodtile`: level, distant / magnify, fraction, detail / sharpen, tiles) runs on **eight 16-bit lanes** (it needs `clz` of a byte and shifts by at most 7 only; the fold of `lod_4x17_to_15` collapses to "largest difference, or overflow if >= 0x4000"). Roughly 90 NEON instructions fewer per group, one call per batch instead of per group.
3. **One-tile fast path** (`tex_cycle_tiles`): if all eight lanes select the same tile (93-99.6 % of the groups) the cycle runs once straight into the output; mixed groups go to `tex_cycle_multi()`, out of line, one pass per distinct tile merged by lane masks (the old loop). The variants shrank (TEXL 112 -> 101 KB for 21 -> 24 kernels).
4. **`tc_divide4`** (perspective divide, 4 lanes; the lookups stay scalar): takes the raw 16.16 interpolants (the arithmetic shift already sign-extends, so the three `sign16` go), and the out-of-bounds test is "the top shift + 1 bits of the product are not all equal" as shift / sign-extend / compare instead of building `tempmask` (51 instead of 65 instructions).
   -2 to -3 % on top for the LOD dumps, neutral elsewhere.
5. **Variants for point-sampled LOD**: `TEXL` for DK64's IA8 and RGBA16 (persp) and I4 (no persp) one-cycle forms, and `TEXAL(0, 1,0,0, 0, tlut, persp, 1,0, 1)` x {TLUT, no TLUT} x {persp, none}. The share of textured pixels that run in the generic kernel is **0.0 % in every dump** (was up to 39.5 %).

**Fallback rate.** Batches that went from a LOD variant to the generic kernel: Banjo-Kazooie 1838 / 2967, 1981 / 3252, 1684 / 2823 (frames 1800 / 2400 / 3000), Donkey Kong 64 335 / 4318 (2400, plus 39.5 % of the pixels without a variant), 1523 / 3298 (3000), 236 / 1536 (800), Rocket 243 / 2972 -> **0 in all of them**.
In the new path, the only per-lane fallbacks are the mixed-tile groups (`tex_cycle_multi`: 0.4-1.5 % of the groups for cycle 0, 0.5-7 % for cycle 1 in the two games) and the half-groups that divide the next pixel again (20-25 %, perspective only).

**Variant count and code size** (`.text` of `mupen64plus-rdp-foir.so`, aarch64, `-O3` generic armv8-a): 2,080,328 -> 2,087,384 bytes (+7,056, +0.34 %); table entries 492 -> 499 (texture 157 -> 164: +3 `TEXL`, +4 `TEXAL`; the `TEXL` kernels are 101 KB for 24 entries (112 KB for 21 before), the `TEXAL` 235 KB for 22 (206 KB for 18 before; one is 10.5 KB));
the shared LOD code is `lod_pass` x 4 (7,540 bytes, was `lod_group` 1,568 + `tex_cycle_tiles` 880) + `tex_cycle_multi` 884. The generic kernel is no longer on the LOD path for any state seen.

**Exactness** (all at 0 differences; log files `test-output/lodvec/exactness/`):

* `make check` (aarch64 under qemu, 2 GB-capped scope): `foir-walkfuzz` 40,000 primitives; `neoncheck.py` **47 scenes** (18 captured frames, 25 + 4 synthetic), `mtfuzz.py` 60 + 40 + 40 scenes; `foir-kfuzz` 3000, `-m 32` 20,000, `-m 64` 40,000, `-S` 15,000 over all **499** entries (0 never exercised), `-S -T 5` 15,000, `-S -T 0` 8,000, and the new `-L -m 1` 20,000 (16,067 ramp scenes, 18,362 LOD batches verified, 128 mode combinations)
  and `-L -S -T 0` 8,000 (every texture variant, 1,981 ramp scenes). 7 min 48 s.
* `make all replay ref NEON=0 && python3 tools/regress.py`: **25 of 25** scenes with 0 differing pixels (15 old + 10 new mip-map scenes).
* `FOIR_NEON_VERIFY=1 FOIR_KNOBS=exact`, all 18 dumps at `-t 0` and `-t 3` (36 runs): **105,667 texture batches verified, 0 mismatching, of which 28,025 computed a LOD** (all TEX_LOD_EN; perspective 27,493; two-cycle 19,103; `lod_verify.txt`).
* x86_64 scalar build vs aarch64 `foir-replay -o` RDRAM, 18 dumps x {`-t 0`, `-t 3`}: **36 of 36 identical**.
* Detail and sharpen are not in any captured frame; they are covered by the fuzzers (`-L`: 6,058 / 6,076 of 18,362 batches), the synthetic scenes (angrylion-exact) and `mtfuzz.py`.

**Mutations** (a deliberately wrong build in a copy of the tree, `test-output/lodvec/scripts/mutate_lod.sh`; numbers are failing iterations of 20,000 / 8,000 / 3,000 and dumps that differ from the scalar replay, of 9 LOD dumps):

| mutation | `kfuzz -L -m 1` | `kfuzz -S -T 0` | `kfuzz` (default) | 9 LOD dumps |
|---|---|---|---|---|
| level off by one (`15 - clz` -> `14 - clz`) | 20 fail | 21 fail | 20 fail | 6 differ |
| fraction shifted one bit too far | 20 | 20 | 20 | 6 |
| wrong tile in one lane (cycle 1 tile ^ 1 in lane 5) | 20 | 20 | 20 | 6 |
| chain bound without the detail tile | 20 | 20 | 0 | 0 |
| chain bound one tile short (max level >= 1) | 20 | 20 | 3 | 0 |
| neighbour-lane reuse ignores `w` | 4 | 3 | 0 | 0 |
| run-boundary guard lanes accepted as neighbours | 20 | 20 | 20 | 6 |
| out-of-bounds test of the perspective divide inverted | 20 | 21 | 20 | 8 |
| LOD saturation (`vqshrn` -> `vshrn`) | 20 | 20 | 10 | 0 |

The captured frames alone catch only the first, second, third, seventh and eighth mutation: the chain-bound, `w`-check and saturation errors need the fuzz scenes that were added (a disagreeing tile inside the chain; ramps with one lane off; steps beyond 0x4000).


**Timing, offline replay** (`foir-replay --fast`, rgdsplus, `performance` governor restored to `schedutil`, `2af08de` vs this tree, run alternately with the order rotated every round, 6 rounds in two sessions, median (min-max), `-n 10..100` repeats per run; the first eight rows are the LOD dumps; in the `-t 3` table the emulation thread's wait at `SYNC_FULL` holds all
the pixel work the workers have not finished, so the sum is the frame time):

-t 0: ms per run, median (min-max)
| dump | old (2af08de) | new | new vs old |
|---|---:|---:|---:|
| Banjo-Kazooie (1800) | 40.66 (40.20-40.82) | 34.43 (34.24-34.88) | -15.3 % |
| Banjo-Kazooie (2400) | 45.56 (45.41-46.02) | 39.02 (38.87-39.10) | -14.3 % |
| Banjo-Kazooie (3000) | 38.42 (38.01-38.54) | 33.20 (33.13-33.38) | -13.6 % |
| Donkey Kong 64 (2400) | 44.63 (44.41-44.91) | 40.33 (40.06-40.60) | -9.6 % |
| Donkey Kong 64 (3000) | 37.97 (37.70-38.50) | 33.20 (32.91-33.36) | -12.6 % |
| Donkey Kong 64 (800) | 14.91 (14.80-15.06) | 13.91 (13.78-13.95) | -6.7 % |
| Rocket Robot on Wheels (1500) | 26.57 (26.43-26.70) | 25.56 (25.41-25.61) | -3.8 % |
| GoldenEye 007 (900) | 5.57 (5.53-5.63) | 5.31 (5.26-5.36) | -4.6 % |
| Super Mario 64 (650) | 18.83 (18.73-19.11) | 18.93 (18.77-19.04) | +0.5 % |
| Paper Mario (900) | 11.88 (11.86-11.91) | 11.73 (11.71-11.82) | -1.3 % |
| Pokemon Snap (300) | 71.18 (71.14-71.59) | 71.83 (71.51-72.24) | +0.9 % |
| Star Fox 64 (1000) | 17.95 (17.92-18.00) | 17.73 (17.62-17.92) | -1.3 % |
| **geometric mean of the ratios** | | | **0.930** |

-t 3: ms per run, median (min-max); emulation thread: own work / waiting at SYNC_FULL
| dump | old (2af08de) | new | new vs old | emulation thread own work old -> new | wait at SYNC_FULL old -> new |
|---|---:|---:|---:|---:|---:|
| Banjo-Kazooie (1800) | 14.88 (13.82-15.75) | 12.15 (11.80-12.75) | -18.3 % | 2.73 -> 2.75 (+1 %) | 12.11 -> 9.38 |
| Banjo-Kazooie (2400) | 16.27 (15.72-18.18) | 14.00 (13.76-15.65) | -14.0 % | 2.56 -> 2.51 (-2 %) | 13.77 -> 11.49 |
| Banjo-Kazooie (3000) | 13.90 (13.67-14.58) | 12.20 (11.87-12.82) | -12.2 % | 2.66 -> 2.69 (+1 %) | 11.19 -> 9.34 |
| Donkey Kong 64 (2400) | 17.14 (16.49-17.88) | 15.07 (14.55-15.91) | -12.1 % | 2.82 -> 2.86 (+1 %) | 14.31 -> 12.20 |
| Donkey Kong 64 (3000) | 14.22 (13.67-14.53) | 11.93 (11.79-12.99) | -16.1 % | 1.36 -> 1.33 (-2 %) | 12.87 -> 10.60 |
| Donkey Kong 64 (800) | 6.33 (5.96-6.43) | 5.94 (5.61-6.24) | -6.1 % | 1.27 -> 1.27 (+1 %) | 5.06 -> 4.64 |
| Rocket Robot on Wheels (1500) | 10.70 (10.07-11.26) | 10.38 (9.65-10.93) | -3.0 % | 2.32 -> 2.33 (+1 %) | 8.39 -> 8.06 |
| GoldenEye 007 (900) | 2.40 (2.33-2.54) | 2.33 (2.29-2.50) | -3.0 % | 0.41 -> 0.42 (+2 %) | 1.99 -> 1.91 |
| Super Mario 64 (650) | 7.27 (6.91-7.77) | 7.12 (6.96-7.87) | -2.0 % | 1.38 -> 1.35 (-2 %) | 5.90 -> 5.77 |
| Paper Mario (900) | 4.76 (4.68-4.83) | 4.83 (4.66-4.94) | +1.5 % | 0.81 -> 0.80 (-1 %) | 3.95 -> 4.04 |
| Pokemon Snap (300) | 26.03 (25.29-27.54) | 26.37 (25.15-28.75) | +1.3 % | 2.58 -> 2.59 (+0 %) | 23.42 -> 23.79 |
| Star Fox 64 (1000) | 7.13 (7.03-7.39) | 7.24 (6.96-7.47) | +1.5 % | 1.09 -> 1.11 (+1 %) | 6.02 -> 6.10 |
| **geometric mean of the ratios** | | | **0.929** | **1.001** | |

The LOD dumps get **6.7-15 % faster at one thread** (Banjo-Kazooie -13.6 to -15.3 %, Donkey Kong 64 -9.6 to -12.6 %, DK64 800 -6.7 %, Rocket -3.8 %, GoldenEye -4.6 %) and 6-18 % with three workers (the emulation thread's own work does not change; the wait is the workers). The dumps without LOD are within +-1.5 % (Super Mario 64 +0.5 %, Pokemon Snap +0.9 %
at one thread: the new `tc_divide4` / code layout is not free for them; a variant that keeps the old divide for the non-LOD kernels was +1.5 to +5 % slower, so the layout of the 164 texture kernels moves things by about that much either way). Development steps, Banjo-Kazooie 2400 / DK64 2400 / DK64 3000 at one thread:
old 45.6 / 44.6 / 38.0 ms -> chain bound only 42.0 / 43.8 / 34.6 -> `lod_pass` + one-tile path 40.4 / 41.0 / 33.1 -> new `tc_divide4` 38.9 / 40.3 / 33.2 ms (`replay/r1.txt .. r4.txt`).

**Timing, in game** (rgdsplus, rsp-hle with parallel-rsp as RspFallback, rsp-timing, `FOIR_THREADS=3`, default knobs, scripted input, 75 s runs, VI frames 1440 on; the method of the batch-builder sweep, fresh private configuration `/storage/foir-dev/lodvec/cfg`, `HOME` / `XDG_*` private; 3 runs of each build, alternating, three runs per
session under the device lock, governor `performance` -> `schedutil`; median (min-max); "RDP" is the emulation thread's time in the RDP per frame -- parse + setup + the wait at `SYNC_FULL`; CPU is the whole process, the workers' idle spin included; `2af08de` vs this tree):

| game | RDP ms/frame, emulation thread (parse + setup + wait) old -> new | of which own work old -> new | wait at SYNC_FULL old -> new | process CPU ms/frame old -> new | fps old -> new |
|---|---|---|---|---|---|
| Banjo-Kazooie | 2.63 (2.61-2.66) -> **2.30 (2.25-2.31)** (-13 %) | 1.57 (1.56-1.58) -> 1.61 (1.61-1.61) (+3 %) | 1.07 (1.04-1.08) -> 0.69 (0.64-0.70) | 31.18 (31.15-31.23) -> **30.48 (30.38-30.54)** (-2.2 %) | 59.6 (59.6-59.6) -> 59.6 (59.6-59.6) |
| Donkey Kong 64 | 4.24 (4.23-4.30) -> **3.55 (3.52-3.55)** (-16 %) | 1.35 (1.34-1.36) -> 1.42 (1.40-1.42) (+5 %) | 2.89 (2.87-2.96) -> 2.13 (2.10-2.15) | 36.01 (36.00-36.01) -> **33.90 (33.90-33.98)** (-5.9 %) | 59.8 (59.8-59.9) -> 59.9 (59.9-59.9) |
| Paper Mario | 2.54 (2.50-2.54) -> **2.23 (2.22-2.31)** (-12 %) | 0.98 (0.98-0.99) -> 0.99 (0.99-1.00) (+1 %) | 1.55 (1.52-1.56) -> 1.24 (1.24-1.31) | 22.80 (22.77-22.82) -> **21.93 (21.91-22.08)** (-3.8 %) | 59.9 (59.9-59.9) -> 60.0 (59.9-60.0) |
| Zelda: Ocarina of Time | 1.38 (1.35-1.47) -> **1.37 (1.36-1.37)** (-1 %) | 0.78 (0.64-0.78) -> 0.81 (0.80-0.81) (+4 %) | 0.60 (0.57-0.82) -> 0.56 (0.56-0.57) | 16.02 (14.24-16.02) -> **16.19 (16.12-16.22)** (+1.1 %) | 59.9 (59.7-60.0) -> 60.0 (60.0-60.0) |
| Mario Kart 64 | 1.62 (1.61-1.63) -> **1.58 (1.58-1.60)** (-2 %) | 1.49 (1.48-1.49) -> 1.47 (1.45-1.48) (-1 %) | 0.13 (0.13-0.14) -> 0.13 (0.11-0.13) | 23.59 (23.58-23.66) -> **23.37 (23.30-23.47)** (-0.9 %) | 59.0 (59.0-59.0) -> 59.0 (59.0-59.0) |
| Star Fox 64 | 1.21 (1.21-1.23) -> **1.15 (1.14-1.20)** (-5 %) | 0.67 (0.66-0.67) -> 0.66 (0.66-0.67) (-2 %) | 0.55 (0.54-0.55) -> 0.49 (0.47-0.54) | 19.19 (19.17-19.21) -> **19.00 (18.93-19.11)** (-1.0 %) | 59.8 (59.8-59.8) -> 59.8 (59.8-59.8) |
| Super Mario 64 | 1.61 (1.60-1.67) -> **1.57 (1.56-1.58)** (-3 %) | 0.59 (0.59-0.60) -> 0.60 (0.60-0.61) (+2 %) | 1.02 (1.02-1.07) -> 0.96 (0.95-0.98) | 21.86 (21.77-21.97) -> **21.76 (21.60-21.83)** (-0.5 %) | 59.9 (59.9-59.9) -> 59.9 (59.9-59.9) |
| Toy Story 2 | 1.99 (1.97-2.01) -> **1.96 (1.96-1.96)** (-2 %) | 1.02 (1.01-1.03) -> 1.00 (1.00-1.01) (-1 %) | 0.97 (0.96-0.97) -> 0.96 (0.95-0.96) | 12.82 (12.81-12.83) -> **12.74 (12.72-12.78)** (-0.6 %) | 58.6 (58.6-58.6) -> 58.6 (58.6-58.6) |
| Pokemon Snap | 2.19 (2.18-2.19) -> **2.18 (2.16-2.18)** (-0 %) | 0.34 (0.34-0.34) -> 0.34 (0.34-0.34) (+1 %) | 1.85 (1.84-1.85) -> 1.84 (1.82-1.84) | 11.91 (11.90-11.92) -> **11.86 (11.82-11.87)** (-0.5 %) | 60.0 (60.0-60.0) -> 60.0 (60.0-60.0) |
| **geometric mean of the new / old medians** | **0.938** | 1.012 | | **0.984** | |

* The emulation thread waits at `SYNC_FULL` for the workers, so faster texture work shows up there: Donkey Kong 64 **-0.69 ms of 4.24 (-16 %)**, Banjo-Kazooie -0.33 ms of 2.63 (-13 %), Paper Mario -0.31 ms of 2.54 (-12 %: CI4 LOD draws); its own work is unchanged to +5 % (not looked into). Process CPU per frame falls 5.9 % (DK64), 3.8 % (Paper Mario), 2.2 % (Banjo-Kazooie): less than the replay's 10-15 %,
  because the workers' idle spin fills what they no longer need. The games without a LOD path (Mario Kart, Star Fox, Super Mario, Toy Story 2, Snap, Zelda) are within -5 to +1 % on RDP time and -1.0 to +1.1 % on CPU (Zelda +1.1 % is an old-build outlier run again: old run 1 was 14.24 ms against 16.02 in the other two). All nine games stay at the frame-rate cap (Banjo-Kazooie 59.6, DK64 59.8 -> 59.9 fps).
* Geometric mean over the nine games: RDP time **0.938**, own work 1.012, CPU 0.984.

**Where the in-game time is now** (`perf record -F 499` of the whole process, 30 s from second 28 of a scripted run, one run per build and game with `-g` builds whose `.text` is byte-identical to the measured ones; shares of the foir work without the workers' idle spin; `test-output/lodvec/perf/`):

| game | texture helpers (`lod_group` / `lod_pass*`, `tex_cycle*`, `fetch_decode`, `neon_texture`) old -> new | of which `lod_group` -> `lod_pass*` | LOD variants (`spec_texl*`, `spec_texal*`) old -> new | whole texture stage old -> new |
|---|---:|---:|---:|---:|
| Banjo-Kazooie | 32.5 % -> **14.8 %** | 13.1 % -> 14.3 % | 5.4 % -> 15.1 % | 42.5 % -> 35.3 % |
| Donkey Kong 64 | 25.4 % -> **12.4 %** | 11.3 % -> 10.8 % | 5.8 % -> 11.6 % | 44.6 % -> 38.6 % |

(Shares of a smaller total: the generic kernel and its helpers, 19.4 / 14.1 % of the old profiles, moved into the variants, the whole texture stage lost 7-6 points; `lod_pass` is now the single largest function after the batch builder. In absolute terms the replay says it costs 5.4 ms of Banjo-Kazooie 2400's 38.9 ms, own divide included.)

**Tried and left out**

| variant | result | why not |
|---|---|---|
| table lookups of `tc_divide4` as lane loads (`vgetq_lane` + `vld1q_lane`) instead of store / reload | 0 to +1 % | neutral on the A55 |
| old `tc_divide4` kept for the non-LOD kernels | +1.5 to +5 % slower | code layout; one divide for everything |
| LOD tail in 32-bit halves as before | not built | the 16-bit tail is exact (checked by the mutations) and half the instructions |
| NEON merge of the lanes of a group that select different tiles (several fetches combined) | not built | 0.4-7 % of the groups; `tex_cycle_multi` stays out of line |
| `TEXAL` for two-cycle point-sampled LOD, and exact `TEXL` for more formats | not added | not seen in any dump; 10.5 KB each; they run the (correct) generic kernel |
| skipping the row-below divide | not possible | no lane holds the pixel below; its coordinates are `s + dsdy` with the masked `dsdy`, not the next row's span |
| vector `vqtbl`-based reciprocal instead of the 128 KB `k_tcdiv` table | not built | needs 12 table registers; the lookups are 14 of 51 instructions |

**Deviations from the workspace rules:** none that change a result. Six new dumps (Banjo-Kazooie and Donkey Kong 64 at VI frames 1800 / 2400 / 3000; DK64 1800 has no textured draw and is not kept) and `Rocket_1500` were added to `test-data/dumps` (git-ignored, regenerate as described in `test-data/README.md`); they were captured on the rgdsplus with the HEAD plugin
and the sweep's scripted input. The `-DFOIR_STATS` build (`POSTFIX=-st`) was built in the tree once and from a copy otherwise; the `-st` object directory was removed afterwards.


### In-game frame rate

| game (VI frames ending at) | M2 scalar | NEON | NEON + 1 worker | NEON + 2 | NEON + 3 |
|---|---:|---:|---:|---:|---:|
| Super Mario 64 (600) | 35.6 ms / **16 fps** | 13.9 ms / **25 fps** | 2.1 ms / **35 fps** | 2.1 ms / **35 fps** | 2.1 ms / **36 fps** |
| Star Fox 64 (1080) | 55.2 ms / **16 fps** | 14.6 ms / **43 fps** | 13.5 ms / **45 fps** | 7.1 ms / **60 fps** | 5.0 ms / **60 fps** |
| Donkey Kong 64 (1080) | 48.5 ms / **17 fps** | 22.1 ms / **30 fps** | 17.5 ms / **34 fps** | 7.8 ms / **50 fps** | 4.9 ms / **58 fps** |
| Banjo-Kazooie (600) | 135.7 ms / **7 fps** | 29.4 ms / **26 fps** | 23.4 ms / **31 fps** | 10.0 ms / **53 fps** | 5.8 ms / **59 fps** |
| Pokemon Snap (600) | 345.0 ms / **3 fps** | 75.3 ms / **9 fps** | 39.2 ms / **13 fps** | 9.1 ms / **22 fps** | 7.8 ms / **23 fps** |

Cells show the RDP time per frame (ms, mean of the 120-frame window ending at the given VI frame) and the total frame rate
(60 = capped at the VI rate), `Threads=3` default knobs, cxd4 as RSP. "RDP time" is the time the emulation thread spends in the
RDP per frame (command parsing, primitive setup, and waiting for the workers at `SYNC_FULL`); with worker threads the pixel work
overlaps the rest of the emulation, so it is not the work done, which is what the replay table above measures. Runs were made with
the plugin as of the in-game benchmark (before the final LOD / hazard / 5551-present fixes, which do not change the work done). Frame
windows are the same VI frame numbers in every column, but the games run in real time, so scenes drift by a few frames between
runs and the scalar column (3-17 fps) sees less of the game per second than the others. Totals are bounded by the emulation thread:
Super Mario 64 stays at about 28-30 ms per frame whatever the worker count, because cxd4 + the CPU interpreter take that long
(rsp-hle + parallel-rsp first brought it to 17.8 ms / 56 fps and Pokemon Snap to 32.5 ms / 31 fps; with the vectorised parallel-rsp
build SM64 reaches the 60 fps cap, and with the M3 renderer Snap reaches 40 fps, see [RSP pairing](#rsp-pairing));
in all five games the emulation thread (cxd4 + CPU interpreter + command parsing, e.g. 370k commands and 75k primitives per frame in
Pokemon Snap), not the RDP pixel work, is what limits the frame rate at three workers.

**M3, in game** (rsp-hle + parallel-rsp fallback, `Threads = 3`, default knobs; mean of the 120-frame window ending at the given VI
frame in two runs, the old and the new plugin alternating on the rig; `test-output/m3/ingame_*.md`, logs in `test-output/m3/logs/`).
"RDP" is the emulation thread's time in the RDP (parse, setup, and waiting for the workers at `SYNC_FULL`: with the workers
faster this wait shrinks), "CPU" is user + system time of the whole process per frame (`getrusage`, the log now has a `cpu:` line;
it includes the workers' 400 us idle spin, so it falls less than the pixel work does). Two parallel-rsp builds were used, because
a vectorised build became available during the milestone (the output of both is bit-identical):

| game (window end) | plugin | parallel-rsp 12:38 build: frame time (fps) | RDP | RSP | CPU ms/frame | parallel-rsp vectorised build: frame time (fps) | RDP | RSP | CPU ms/frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Super Mario 64 (600) | M2 | 17.8 (56) | 2.34 | 14.2 | 54.5 | 16.6 (60) | 2.30 | 8.1 | 33.7 |
| | **M3** | 18.0 (55) | 2.31 | 14.3 | 53.6 | 16.7 (60) | 2.25 | 8.1 | 33.3 |
| Star Fox 64 (1080) | M2 | 16.6 (60) | 6.16 | 8.2 | 31.2 | 16.6 (60) | 6.24 | 7.6 | 29.0 |
| | **M3** | 16.6 (60) | 4.64 | 6.8 | **25.5** | 16.7 (60) | 4.82 | 6.3 | **24.0** |
| Donkey Kong 64 (1080) | M2 | 16.7 (60) | 3.81 | 6.6 | 27.3 | 16.7 (60) | 4.92 | 6.3 | 27.0 |
| | **M3** | 16.7 (60) | 2.72 | 5.7 | **24.1** | 16.7 (60) | 3.70 | 5.3 | **22.7** |
| Banjo-Kazooie (600) | M2 | 16.6 (60) | 7.75 | 11.4 | 44.5 | 16.8 (60) | 9.25 | 11.0 | 43.9 |
| | **M3** | 16.6 (60) | 3.78 | 7.6 | **30.8** | 16.6 (60) | 5.48 | 7.4 | **29.9** |
| Pokemon Snap (600) | M2 | 34.0 (29) | 10.20 | 30.7 | 117.2 | 33.2 (30) | 19.55 | 29.9 | 114.1 |
| | **M3** | **31.4 (32)** | 7.38 | 27.9 | 107.7 | **24.9 (40)** | 11.04 | 21.5 | 82.8 |

* Frame rate: with the vectorised RSP, Pokemon Snap becomes RDP-bound (the emulation thread waits ~10 ms per frame at `SYNC_FULL`
  for the workers) and M3 takes it from **30 to 40 fps** (24.9 ms vs 33.2 ms per frame) and cuts the process CPU time per frame
  by 27 %. Super Mario 64 is capped at 60 fps by that RSP build already, and with the older build it stays RSP-bound (56 vs 55
  fps: the RDP is not its bottleneck). Star Fox 64, Donkey Kong 64 and Banjo-Kazooie were already at the 60 fps cap.
* CPU headroom: in the games that are capped at 60 fps the M3 renderer needs 12-32 % less CPU per frame (with the old / the
  vectorised RSP build: Star Fox 64 -18 % / -17 %, Donkey Kong 64 -12 % / -16 %, Banjo-Kazooie -31 % / -32 %); the emulation
  thread's RDP time falls by 1-4 ms per frame. Super Mario 64 shows no change in either column: its frame is RSP- or cap-bound
  and its pixel work is a small part of it.
* The windows are 120 frames of real-time play of demos/menus; scenes drift by a few frames between runs. Repeat counts are low (2 per
  cell), the spread between the runs was within 1-3 % for the frame times.

### Specialisation coverage

(The table below and the leave-one-ROM-out estimate are the M3 figures. The texture and Z compare numbers for 62 games after the "any format" tier and the complete Z compare list
are in [Cheap wins after M3](#cheap-wins-after-m3).)

Share of the pixels that ran through a specialised variant, per stage, in 120-frame windows of the first ~60 s of each ROM (frames
240 on; Threads = 3, the final variant lists; from the plugin's own `specialised pixels:` log lines, `test-output/m3/coverage_per_game.md`).
"Texture" is the share of the *textured* pixels (the number in brackets is the share of all pixels that needed the texture unit).
**These numbers are in-sample**: the variant lists were built from the statistics of exactly these runs, so they say that
nothing in these scenes falls through, not how a game outside the lists fares.

| game | texture (share of pixels textured) | combine | zcmp | blend | store | windows |
|---|---|---|---|---|---|---|
| Banjo Kazooie | 100 % (71 %) | 100 % | 100 % | 100 % | 100 % | 21 |
| Blast Corps | 100 % (79 %) | 100 % | 100 % | 100 % | 100 % | 20 |
| Bomberman Hero | 100 % (82 %) | 100 % | 100 % | 100 % | 100 % | 19 |
| Buck Bumble | 100 % (86 %) | 100 % | 100 % | 100 % | 100 % | 18 |
| Diddy Kong Racing | 100 % (68 %) | 100 % | 100 % | 100 % | 100 % | 22 |
| Donkey Kong 64 | 100 % (95 %) | 100 % | 100 % | 100 % | 100 % | 17 |
| F Zero X | 100 % (99 %) | 100 % | 100 % | 100 % | 100 % | 22 |
| Glover | 100 % (100 %) | 100 % | 100 % | 100 % | 100 % | 18 |
| GoldenEye 007 | 95 % (74 %) | 100 % | 100 % | 100 % | 100 % | 21 |
| Jet Force Gemini | 90 % (95 %) | 95 % | 100 % | 95 % | 100 % | 21 |
| Kirby 64 The Crystal Shards | 100 % (79 %) | 100 % | 100 % | 100 % | 100 % | 14 |
| Legend of Zelda The Ocarina of Time | 100 % (92 %) | 98 % | 100 % | 100 % | 100 % | 22 |
| Mario Kart 64 | 100 % (45 %) | 100 % | 100 % | 100 % | 100 % | 21 |
| Mischief Makers | 100 % (100 %) | 100 % | 100 % | 100 % | 100 % | 17 |
| Mystical Ninja Starring Goemon | 94 % (89 %) | 100 % | 100 % | 100 % | 100 % | 18 |
| Paper Mario | 100 % (84 %) | 100 % | 100 % | 100 % | 100 % | 22 |
| Pokemon Snap | 100 % (98 %) | 100 % | 100 % | 100 % | 100 % | 11 |
| Rocket Robot on Wheels | 84 % (90 %) | 100 % | 100 % | 100 % | 100 % | 22 |
| SpaceStation Silicon Valley | 100 % (86 %) | 100 % | 100 % | 100 % | 100 % | 21 |
| Star Fox 64 | 100 % (90 %) | 100 % | 100 % | 100 % | 100 % | 23 |
| Super Mario 64 | 100 % (90 %) | 100 % | 100 % | 100 % | 100 % | 21 |
| Wetrix | 100 % (98 %) | 100 % | 100 % | 100 % | 100 % | 15 |
| Yoshi s Story | 100 % (100 %) | 100 % | 100 % | 100 % | 100 % | 22 |
| **mean of 23 games** | 98 % | 100 % | 100 % | 100 % | 100 % | |

**Out of sample.** Leave-one-ROM-out estimate (`tools/specstats.py --loo-policy`): the lists are rebuilt with the same policy from 22
of the 23 logs and applied to the 23rd; mean over the 23 games (min):

| stage | texture | combine | zcmp | blend | store |
|---|---|---|---|---|---|
| pixels in a variant | 90 % (47 %) | **59 % (8 %)** | 98 % (63 %) | 96 % (74 %) | 100 % (99 %) |

The texture, Z, blender and store states are shared between games (they come from a few formats and modes, and the lists contain
the whole cross product of the common texture formats), but the colour combiner is not: most games use selector sets that no
other game of the sample uses (for 9 of the 22 games examined more than half of their combiner pixels use a selector set that appears in no
other game of the sample), so the generic combiner stays in use for roughly 40 % of the combiner pixels of an unseen game (it runs at the M2
speed). Adding the logs of more games to the lists (`tools/specstats.py --def`) is one fix; the other is the
[shape kernels](#shape-kernels-combiner) below, which fix only the shape of the equations and take their operands from tables.

**With shape kernels.** Combiner pixels by kind of kernel, per game (the 3 captured 60-frame windows of a game summed), mean over the games, from the statistics build
(`CFLAGS=-DFOIR_STATS`, `foir-replay --fast`, 184 dumps of 62 games; `test-output/m3/shapes/kernels/coverage_per_game.txt`). "Shape list from 62 games" is the shipped
list (in-sample for all of these games); "shape list from the 23 test games" was built only from the statistics of the 23-ROM sample (`src/spec_shape_t23.def`), so for the 39 unseen games
it is the honest out-of-sample number (the exact variants were made from the 23-ROM sample as well):

| games | kernel list | exact variant | shape kernel only | generic kernel | games with > 50 % generic |
|---|---|---:|---:|---:|---:|
| 39 unseen | exact only (M3) | 71.0 % | -- | 29.0 % | 10 |
| 39 unseen | + shape list from the 23 test games | 71.0 % | 13.5 % | **15.5 %** (median 2.5 %) | 4 |
| 39 unseen | + shape list from 62 games (shipped, in-sample) | 71.0 % | 29.0 % | 0.0 % | 0 |
| 23 test | exact only (M3) | 89.6 % | -- | 10.4 % | 2 |
| 23 test | + shape list (either) | 89.6 % | 10.4 % | 0.0 % | 0 |

The in-sample rows say that every state the 62 games used has a kernel; the out-of-sample estimate for a game outside all 62 is the leave-one-game-out figure
(`python3 tools/shapegen.py --loo --one-cycle-product test-output/m3/shapes/stats`): the 147 kernels built from the other 61 games cover **89.8 %** of the combiner pixels on average
(median 100 %, 6 of 62 games below 50 %; kernels built without the one-cycle products: 89.0 %). The estimate of 92 % from the equation-shape analysis counted each half equation on its own;
a fused kernel needs the *combination* of its four halves to occur, which costs about 3-5 points.

### Bit-exactness

* `tools/neoncheck.py`: 11 captured real frames + 14 synthetic scenes; scalar reference vs the default kernels (variants on) inline and
  with 1, 2 and 3 workers, vs the generic NEON kernels (`--nospec`) inline and with 3 workers, and, with the plugin-default knobs, variants vs
  generic kernels: **25 scenes, 0 differing bytes of RDRAM** in every configuration.
* `foir-kfuzz -S`: every one of the **281 variants** (texture 106, combine 110, zcmp 29, blend 28, store 8) is written over a random render
  state with random TMEM / framebuffer / Z contents and random pixel batches and compared with the scalar chain (the generic NEON chain for
  the RelaxedPrecision variants): 60,000 iterations (about 210 per variant, 69.6 M value checks), **0 differences**, every variant was selected as intended; a
  deliberately wrong constant in one combiner operand is caught within 2,000 iterations (checked).
* **Shape kernels.** `foir-kfuzz -S` now walks through 428 variants (the 281 of M3 and the 147 shape kernels). A shape kernel is run with random combiner operands
  that have its shape (`foir_shape_apply`: for every `V` input a random selector of that kind, the canonical "D only" forms reached through a zero multiply input or equal
  A / B constants, `ext` forms through a sharpened LOD fraction or wide texels), random dither / alpha compare / coverage options, random batches, and compared with the scalar
  chain: about 28 iterations per kernel in the walk over all 428 variants (12,000 iterations, 14.2 M value checks) and 100 per kernel in the shape-only walk (`-S -T 5`, 15,000 iterations, 18.5 M checks; 21,600 / 26.7 M in a longer run), **0 differences**. `make check` passes as a whole. Mutations are caught: a wrong sign in the `A = 0` form fails 15 of 600 iterations, dropping the
  `ext` extension fails the `ext` kernels immediately. The selection must pick exactly that kernel (checked). `tools/neoncheck.py` replays the 11 real frames and 14 synthetic scenes with
  `FOIR_SPEC_COMB=shape|exact|generic` (inline and 3 workers) and with `--fast`: **26 scenes, 0 differing bytes**. `mtfuzz.py --comb shape`: random scenes whose combiners come from the shape list
  with random operands (plus random sharpen / BI_LERP = 0 states): 40 scenes, 0 differences. `FOIR_NEON_VERIFY=1 FOIR_SPEC_COMB=shape` (every batch of every stage next to the scalar kernel,
  exact knobs) on the 181 non-empty 60-frame dumps of the 62 games: **17.2 M combiner batches, 0 mismatching** (texture 14.8 M, zcmp and blend 17.2 M batches, 0 mismatching as well).
* `foir-kfuzz` (generic kernels, random states): 40,000 more iterations in this round (51 M value checks), 0 differences. Earlier rounds: 90,000
  iterations, 115 M checks.
* `tools/mtfuzz.py`: random scenes (fills, shaded / textured / Z-buffered triangles with random render modes, half of the combiner
  and blender modes taken from the variant lists, buffer switches with different widths and overlap, texture-from-framebuffer loads,
  syncs) with scalar inline vs the default kernels on 1-4 workers: 400 more scenes in this round, **0 differences** (2500 in the M2 round;
  ThreadSanitizer on x86_64 with the scalar kernels and 3 workers is clean on the M3 code too, run with `setarch -R` because of the kernel's mmap randomisation).
* **In game on the device:** `FOIR_NEON_VERIFY=1 FOIR_KNOBS=exact` (every batch of every stage, variants included, computed next to the
  scalar kernel, 3 workers) for ~70 s each of ten ROMs (SM64, SF64, DK64, Banjo-Kazooie, Pokemon Snap, GoldenEye, Blast Corps, Zelda OoT, Diddy Kong
  Racing, Kirby 64; 1.8-4.3 M batches per stage and ROM): **0 mismatching batches** (`test-output/m3/ingame_verify.txt`). Screenshots at plugin
  frames 300 and 900 of 12 games (deterministic interrupts) from the old and the new plugin are **byte-identical** (24 of 24; the other 11 ROMs ran the
  same time without a crash or hang; `test-output/m3/screenshots_vs_baseline.txt`, ten of the new plugin's frames and a contact sheet are in
  `test-output/m3/`; several games show a black frame at plugin frame 900, both builds alike).
* **Cheap wins (COPY kernel, buffer access, texture / Z tiers).**
  * `foir-kfuzz -m 32` fuzzes the COPY span kernel: random tile (format, size, line, TMEM address, palette, masks, shifts, clamp bits), TLUT on/off, alpha compare, perspective, colour buffers of 16 / 8 / 4 bit with widths 16-320 and an odd halfword address,
    random spans (also those that touch the first / last pixel of a row, start left of 0, have a negative length or `flip = 0`), a colour buffer moved to the first / last pixels of RDRAM in one iteration of eight; compared on the whole colour buffer region and the hidden bytes
    (plus the first and last 32 KB of RDRAM): 60,000 iterations in a longer run (46,500 spans wrote pixels: 13,400 RGBA16, 5,200 TLUT8, 5,000 TLUT4, 14,600 16-bit TLUT, 11,500 other texel formats; 10,400 took the scalar fallback), **0 differences**; `make check` runs 20,000. The harness reports how many spans wrote pixels and by which kind of texture they went (RGBA16, TLUT8 / 4 / 16, other texels, scalar fallback), so the mix is visible.
  * The memory kernels of `foir-kfuzz` (all stages, default and `-S`) now run one iteration in eight with the colour buffer or the Z buffer at the first or last pixels of RDRAM, so the batch-level bounds check and the per-lane fallbacks are exercised.
  * `foir-kfuzz -S` walks the table entries of the new design as well: 15,000 iterations over all 492 entries in `make check` (about 30 per entry, 17.7 M value checks) and 40,000 in a longer run (47.3 M checks), **0 differences**, every entry hit (texture 157: exact `TEX` / `TEXL`, "any format" `TEXA` / `TEXAL`; the any-format entries are run with random tile formats, sizes and MID_TEXEL; Z compare 42; the selection must pick exactly that entry), 8,000 (9.1 M checks) in `make check` and 30,000 (34.1 M checks) in a longer run alone, 0 differences more on the texture table alone.
  * `tools/neoncheck.py`: 12 captured frames + 15 synthetic scenes (the new `copytlut` scene draws CI8 / CI4 / RGBA16 COPY rectangles through a TLUT, with and without alpha compare and masks; the scalar reference is bit-identical with angrylion on it, 15 of 15 scenes with 0 differing pixels), now also with `FOIR_SPEC_TEX=shape|exact|generic` (inline and 3 workers) and with `--fast` + `FOIR_SPEC_TEX=shape`: **27 scenes, 0 differing bytes of RDRAM** in every configuration.
  * `mtfuzz.py` scenes contain COPY-cycle texture rectangles (RGBA16, CI8 / CI4 with TLUT, 16-bit CI / IA with TLUT, I4, IA8, RGBA32; random masks, shifts, flipped rectangles, partly off-screen; the tile is then reused by later triangles, which puts all those formats through the texture unit), `--tex shape` forces the "any format" kernels: 60 + 40 (`--comb shape`) + 40 (`--tex shape`) scenes with 1-4 workers, **0 differences**.
  * `FOIR_NEON_VERIFY=1` now also checks the **store** stage (the pixels of the lanes with wen set, colour and Z, after the NEON kernel against the scalar one) and the **COPY** kernel (the row of the colour buffer the span can touch). With exact knobs on the 181 non-empty 60-frame dumps of the 62 games: texture 16.1 M batches, combine / zcmp / blend 18.7 M, store 18.7 M, COPY 2.1 M spans, **0 mismatching** in every stage; with `FOIR_SPEC_TEX=shape FOIR_SPEC_COMB=shape` (every
    texture and combiner batch through the second-tier kernels): texture 16.9 M batches, combine / zcmp / blend 19.6 M, store 19.6 M, COPY 2.1 M spans, **0 mismatching**. In game on the device (rsp-hle + vectorised parallel-rsp, 3 workers, 60-75 s each): Paper Mario (2.0 M texture / 2.3 M other batches, 320,000 COPY spans), Rocket (2.0 / 2.1 M, 134,000), Mischief Makers (2.5 M, 1.83 M), Super Mario 64 (2.9 / 3.1 M, 82,000), Donkey Kong 64 (3.0 / 3.2 M) and Banjo-Kazooie (1.7 / 2.2 M): **0 mismatching batches** in every stage (Paper Mario ran 6.9 % of its textured pixels in "any format" kernels in that window).
  * **Mutations caught** (a deliberately wrong version of the code is built and `foir-kfuzz` must fail): COPY: palette base 0x400 -> 0x404, no half-used first window for an odd start pixel, alpha compare ignored, texture mask limit 10 -> 9 (20 failing iterations of 3,000 each);
    buffer access: the odd-window frame merge (lane 0 of the old vector), the hidden-byte layout, the batch bounds check (allowing windows 8 KB past the end of RDRAM: caught by the zcmp reads of the edge iterations, 2 of 3,000 iterations at the edge); texture / Z tiers: MID_TEXEL ignored, AA ignored in the Z compare variants, `anyfmt` off in the any-format kernels,
    the LOD tiles taking the TLUT type from the key (20 failing iterations each). The row-ownership test (an odd window must stay in its row) has no content-level mutation test: rewriting a neighbour's pixel with its old value changes nothing in a single-threaded fuzzer; it is exercised by the multi-worker `mtfuzz` scenes only for the case where the value races.
* The scalar reference itself: 15 synthetic scenes and 8 real frames are bit-identical with angrylion-rdp-plus (Kirby 64: 25 pixels
  differ, inherited from M1); re-run after the combiner-selector refactoring and after these changes (`make all replay ref NEON=0`, `tools/regress.py`): 15 of 15 scenes with 0 differing pixels.
* `RelaxedPrecision` (on by default) is *not* exact; `FastCombiner` is. With `FOIR_KNOBS=exact` the output is
  identical to the reference. Screenshots of both settings for five games (and a contact sheet of the first seconds of all 23 ROMs
  on the device, all without crashes or hangs) are in `test-output/m2/`.

---

## Feasibility checkpoint

Lightning emits only scalar glue code, so its benefit had to be measured, not assumed. With M2 in place
the data for the decision is:

1. **Few distinct pipeline states.** A per-batch statistics build (`-DFOIR_STATS`, `FOIR_STATE_STATS=1`) counted the pixels rendered
   under each distinct `{other_modes, combiner, tile format}` key in 12 captured frames of real games: **2-7 states per frame**
   (50 distinct in total across the 12 games, most of them game specific). The 24 most common cover 92 % of all pixels, the 32 most
   common 98 %; per game, 2-5 states cover 90 % of the pixels. States are few, so a cache of precompiled variants (static or JIT) would hit
   almost always, and a variant needed on the first frame of a new game is rare enough that a generic fallback covers it.
2. **The generic NEON kernels already take the fast shapes.** Combiner equations are classified per batch (`D`, `A*C`, `(A-B)*C`, `A*C+D`),
   Z compare has an exact opaque/transparent fast path, texture fetch is specialised by tile size and format by plain branches that
   predict perfectly. Per-batch glue (state decode, operand tables, constant splats, kernel dispatch) is the only thing a scalar-only
   JIT such as Lightning could remove. It was measured by rebuilding with batch sizes of 32 / 64 / 128 / 256 pixels (`-DFOIR_BATCH=`):
   64 -> 256 pixels per batch makes a replay only **3-4 % faster** (Super Mario 64 29.1 -> 28.1 ms, Banjo-Kazooie 74.4 -> 72.7 ms,
   Pokemon Snap 144.6 -> 140.6 ms per two frames), i.e. all per-batch overhead together is about 4 % of the run time. That is the upper bound
   for what Lightning glue can buy; the NEON bodies it would chain are the same ones that run today.
3. **Where the time goes** (perf on the RK3566, replay, one thread): texture (coordinates, TMEM gathers, decode, filter) 25-37 % of a frame,
   span / batch building 8-12 %, combiner 8-22 %, Z compare + framebuffer / Z access 14-19 %, blender 5-9 %, store 5-6 %, primitive setup
   and TMEM loads on the parser 2-7 %. A *static macro-specialised* variant (state constants folded into the NEON bodies at build time) can
   shave the branches and operand-table lookups of the combiner / blender / Z stages and the format switches of the texture stage; from the
   instruction counts (about 390 instructions per pixel on Super Mario 64, IPC 0.85-0.9 on the A55) that is plausibly **10-25 %**, not a multiple (an estimate from the instruction mix; no specialised variant has been built or measured).
   The remaining cost is arithmetic that has to be done for every pixel (9-bit combiner math, 4-tap TMEM gathers).
4. **Conclusion for the checkpoint.** On an A55-class core the JIT's best case is ~4 % (glue) and it cannot touch the vector bodies, while
   static variants for the ~30 most common state shapes would get most of the specialisation gain without runtime code generation, W^X
   handling, or a Lightning dependency (and its LGPL / bundling issues). **Recommendation: drop the Lightning compiler and, in M3, add
   precompiled macro-specialised variants (cycle count x texture format class x TLUT x persp x Z mode class x blend class), selected by a
   small state key with the generic NEON kernels as the fallback.** Larger wins than either are available elsewhere: fusing stages to keep
   the 8-pixel working set in registers, a faster delta-Z path for interpenetrating / anti-aliased states (36 % of Super Mario 64's pixels run
   through the slow path), and skipping texture / combiner work for lanes the Z test already rejected (5-15 % of Z-tested groups are dead).
5. **System level.** With the RDP on worker threads the emulation thread is the bottleneck in all games tested: Super Mario 64 stays at
   about 28 ms per frame whatever the worker count because cxd4 + the CPU interpreter + parsing take that long, so the next frame-rate gains
   are in the RSP rather than in the RDP; the parallel-rsp pairing first gave SM64 56 fps and Snap 31 fps; after vectorising its VU / load-store ops SM64 is at the
   60 fps cap and Snap became RDP-bound (40 fps with the M3 renderer).
6. **Outcome of M3 (measured).** The estimate in point 3 was too pessimistic. The static variants make the replay of the same frames
   1.25-1.65x faster on one thread and 1.2-1.5x faster with three workers than the M2 renderer (table in [Measured results](#measured-results)),
   about twice what point 3 guessed, because a variant removes more than branches: the per-batch table setup (the combiner splatted
   its constants into per-batch arrays and classified its equations on every batch), the loads of operand descriptors, the shade /
   cycle-0 round trips through memory and the format switches of the texture fetch are all gone. The Lightning compiler stays dropped:
   everything it could have done (choose a kernel per state) is done by the selection table, without runtime code generation. Not done,
   after measuring: skipping texture / combiner work for lanes the Z test has already rejected (5-15 % of the Z-tested groups of Super Mario 64,
   Donkey Kong 64 and Pokemon Snap are dead, 5 % in Banjo-Kazooie, 0.3 % in Kirby 64; an early Z pass costs about as much as it saves
   below ~10 % dead groups, so at most 3-4 % in the best scenes) and a faster delta-Z path (the interpenetrating-mode variant is
   ~4 % of Super Mario 64's time).

7. **Outcome of the shape kernels, and whether a copy-and-patch JIT would still pay.** The combiner is 7-8 % of foir's CPU time in game (profile above). Its remaining
   inefficiencies after the shape kernels, measured on the rig: (a) a shape kernel is 1.15-1.65x an exact variant, which costs 0-5 % of the replay time and only where a shape kernel
   runs instead of an exact variant (never, with the dispatch order exact -> shape); (b) the 10 % (leave-one-game-out) of the combiner pixels that no shape kernel covers still run
   in the generic kernel at 2-3x a shape kernel, i.e. up to ~1-2 % of the frame time in the worst unseen games. A JIT that emitted exactly the kernel of the state (copy-and-patch of
   pre-built stencils) would remove both, but its ceiling for the combiner alone is therefore about 2-3 % of the CPU time, against a cost in W^X handling, code-buffer management and a
   second code path to keep bit-exact. It looks worthwhile only as a way to *fuse stages* (texture, combiner, Z compare and blender sharing registers and avoiding the batch arrays),
   which these numbers do not address; as a combiner-only mechanism it does not. Recommendation: keep the exact variants **and** the shape kernels (exact first: they are the faster path for the
   states they cover and cost 275 KB; the shape kernels add 298 KB and the generalisation), regenerate both lists from more games when convenient (`tools/specstats.py`, `tools/shapegen.py`), and do not build the JIT for the combiner.

---

## Roadmap

1. **M1 -- Reference renderer:** command parser, primitive setup and the scalar C
   pipeline. **Done**, bit-exact with angrylion on the synthetic scenes.
2. **M2 -- NEON + threads:** NEON kernels, band-split worker pool and the VI blit.
   **Done** -- first full-speed games on an A55 (see results). Open items: YUV in NEON (COPY-cycle rectangles are NEON since the
   "cheap wins" below), the CPU framebuffer-read hooks (`FBRead` / `FBWrite` only drain the workers).
3. **M3 -- Specialised pixel kernels:** **Done** -- static macro-specialised variants of every pixel stage, selected per render
   state, with the generic NEON kernels as the always-available fallback (the Lightning compiler was dropped after the
   feasibility checkpoint). Shape-specialised combiner kernels (the cheaper fallback for selector sets that no exact variant covers) are in.
   **Cheap wins after M3 -- done:** NEON COPY-cycle rectangles, inlined / hoisted / prefetched colour and Z buffer access, "any format" texture kernels and the complete Z compare list (see
   [results](#cheap-wins-after-m3)). Open items: variants for 32-bit colour buffers, the NEON COPY kernel for 8-bit colour buffers and COPY triangles, contiguous-texel fast paths for the COPY kernel,
   the per-lane path of the groups that straddle two scanlines (a vector form was measured and does not pay on the A55), batch building (the second largest item of the in-game profile, 20 %: first step done, see [the results](#batch-builder-edge-zone-coverage-and-fixed-size-ramps); what is left is ~500 instructions a span, mostly the 45 vector stores of a piece, the per-piece prefetch and the edge-zone pixels).
4. **M4 -- Tuning:** per-game hints set by suxty, A53 profiling, more of the
   speed knobs (texture precision), VI filters if wanted.

---

## Building

foir follows the sibling Mupen64Plus repos and builds with a `projects/unix`
Makefile.

```sh
cd projects/unix
# x86_64 (scalar kernels): plugin, offline replay tool and the angrylion reference runner
make all replay ref NEON=0
# aarch64 cross build (NEON kernels, products in _out_AARCH/): plugin, replay tool, kernel fuzzer
make all replay fuzz CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64
# native aarch64 build: just `make all` (NEON=1 is the default on aarch64)
```

Requirements: a C11 compiler (GCC or Clang), the Mupen64Plus core API headers (found next to the checkout or
in `/usr/local/include/mupen64plus`), `pthread`. The angrylion reference runner (`make ref`) needs the sibling
`angrylion-rdp-plus` checkout. Useful options: `NEON=0|1`, `OPTFLAGS=`, `DEBUG=1`, `POSTFIX=`, and `CFLAGS=-DFOIR_STATS` in the
*environment* (`CFLAGS=-DFOIR_STATS make ... POSTFIX=-stats`; NEON fallback counters and the per-state / per-stage-key pixel
statistics that the variant lists are made from, see [Adding variants](#adding-variants)). There is no `JIT=` make option any more.
aarch64 builds use the compiler's generic armv8-a target. `-mcpu=cortex-a55` was tried on the RK3566 rig and was
performance-neutral (foir-replay 1-2.5% faster single-threaded, 2-5% slower with 3 workers; in-game frame times unchanged).

## Source layout

```
src/plugin.c          Mupen64Plus exports, [Video-Foir] config, frame skipping, UpdateScreen/ReadScreen2, the suxty interface
src/foir_suxty.h      the suxty interface table (ABI 1), shared with mupen64plus-input-suxty
src/parser.c          DP command list reader, command dispatch, state-setting commands, derived pipeline flags
src/setup.c           primitive setup: triangle / texrect / fillrect -> canonical 44-word record -> per-primitive constants + walker record (setup_walk)
src/walk.h            the edge walker for one row from the walker record (walk_row: NEON, walk_row_c: portable)
src/raster.c          row loop (walk_row per owned row) and span -> pixel-batch builder (NEON: fixed-size ramps, edge-zone coverage; portable reference builder), worker row ownership
src/mt.c              worker pool: job ring (walker record + constants), state / TMEM snapshots, hazards and barriers
src/tmem.c            TMEM model, LOAD_TILE/BLOCK/TLUT (word-wise fast path), texel readback
src/kernels_scalar.c  scalar reference kernels (combiner, Z, blender, FB store, FILL, COPY)
src/kernels_tex.c     scalar texture kernel (perspective, LOD, filtering, TLUT, YUV)
src/kernels_neon.c    NEON kernel table, FOIR_NEON_MASK / FOIR_NEON_VERIFY (wrappers for texture, combine, zcmp, blend, store and copy)
src/kneon_tex.c       NEON texture unit (texture_body: generic kernel + the template of the variants), the COPY-cycle span kernel (neon_copy)
src/kneon_comb.c      NEON colour combiner (generic kernel)
src/kneon_blend.c     NEON blender (blend_body: generic kernel + variant template)
src/kneon_mem.c       NEON Z compare, framebuffer / Z access (inline windows, batch-level bounds checks), store (zcmp_body / store_body), FILL
src/spec.h, spec.c    M3: stage keys, variant lookup / selection per render state, key <-> state helpers for the tests
src/kspec_*.inc       M3: the specialised variants (instantiated from the lists in src/spec_*.def, included by the kneon_*.c files)
src/spec_*.def        M3: the variant lists (generated by tools/specstats.py / tools/specmatrix.py)
src/kspec_shape.inc   shape-specialised combiner kernels (shape_body + the kernels listed in spec_shape.def; operand table built in spec.c)
src/spec_shape.def    the shape kernel list (generated by tools/shapegen.py)
src/combreg.h         combiner selector -> register mapping (parser and variants)
src/vi.c              VI: crop the active framebuffer to one RGBA texture (NEON), interlace hack
src/gl_out.c          draws that texture through the core's VidExt (GLES2, no link-time GL)
src/capture.c         optional frame capture for offline replay (FOIR_DUMP=prefix)
src/stats.c           debug statistics (-DFOIR_STATS): pixels per pipeline state and per stage key
tools/                foir-replay (offline renderer), kfuzz, walkfuzz (+ walk_ref.inc, the sequential walker it checks against), ref-angrylion, neoncheck.py, mtfuzz.py, regress.py,
                      compare.py, vitest.py, rdpgen.py, specstats.py / specmatrix.py (variant lists), shapegen.py (shape kernel list)
test-data/dumps/      captured frames of real games (xz-compressed .fdump)
test-output/          reference images (synthetic scenes, rig screenshots, M2 screenshots)
```

## Tests

All of these run on x86_64; the NEON ones also under `qemu-aarch64-static` or natively on a device.

```sh
cd projects/unix
make check CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 \
           RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu"      # walkfuzz + neoncheck.py + mtfuzz.py + kfuzz (+ kfuzz -m 32: COPY kernel, kfuzz -S: every variant, any-format and shape kernel)
python3 ../../tools/neoncheck.py                                       # on a device / x86 host (native)
python3 ../../tools/regress.py ../../test-output/synthetic            # scalar vs angrylion, 15 synthetic scenes
python3 ../../tools/vitest.py ./foir-replay                           # interlace hack
```

* `neoncheck.py` replays every scene (the captured real frames in `test-data/dumps` -- 18 now, six of them with mip-mapped draws -- plus the 25 + 4 synthetic scenes of `regress.py`, `SCENES` and `XSCENES`) with
  `--scalar` as the reference and with the default kernels (specialised variants on) at 0, 1, 2 and 3 worker threads, with the
  combiner path forced by `FOIR_SPEC_COMB=shape|exact|generic` and the texture path by `FOIR_SPEC_TEX=shape|exact|generic` (inline and with 3 workers), with the
  variants off (`--nospec`, the generic NEON kernels) inline and with 3 workers, and with the plugin-default knobs
  (`--fast`: RelaxedPrecision, which the scalar kernels do not have) variants against generic kernels; the final RDRAM
  images must be byte-identical. It also prints the share of the pixels that ran in specialised variants per scene.
* `foir-kfuzz -m 64` checks the batch builder (`raster.c`) against its portable reference, which the replay-based checks cannot do (they replace the kernels, not the builder); see
  [the results](#batch-builder-edge-zone-coverage-and-fixed-size-ramps). The aarch64-vs-x86 cross check is a plain `foir-replay -o` of each dump from the `NEON=0` x86_64 build and the aarch64 build, compared with `cmp`.
* **LOD / mip-map scenes.** `foir-kfuzz -L` makes every iteration a mip-map scene: TEX_LOD_EN (mostly), detail and sharpen now and then, perspective (mostly), one and two cycles, point and bilinear sampling, TLUT, a combiner that reads TEXEL0 / TEXEL1 / the LOD fraction,
  batches whose scanline runs are linear ramps of s, t, w with steps from sub-texel to the largest the 14-bit LOD holds (also around the powers of two where a level changes), a different row-below step, the odd ramp with one lane off by a little, and a tile chain
  (`tilenum .. tilenum + max_level`, one more with detail) that has the texel format of the primitive's tile with arbitrary tiles after it, or, one time in four, a disagreeing tile inside it (the specialised variants must hand that to a generic kernel).
  The default `kfuzz` run draws one iteration in four that way, `kfuzz -S` does it for the LOD variants (with detail / sharpen, which are not part of the variant key), and `make check` runs `-L -m 1` (20,000) and `-L -S -T 0` (8,000) on top. The run prints how many LOD batches it verified by mode
  (`kfuzz LOD: ...`). `mtfuzz.py` scenes now switch TEX_LOD_EN / detail on in a third / a sixth of their triangles, with a four-level mip chain in tiles 0-3 and a random max level. `regress.py` has ten mip-map scenes (two cycles; detail; sharpen; min level; no perspective; IA8 point sampled one cycle;
  CI4 + TLUT; RGBA32 with mirror; IA16 with clamp / mirror / detail; I4 with clamp): scalar vs angrylion, 0 differing pixels. Four more (`XSCENES`: one-cycle perspective LOD with a max level above 0, where the scalar reference and angrylion differ in the far part of the plane --
  angrylion's `tclod_1cycle_current` has span-edge cases the scalar kernel does not model; not investigated) are NEON-vs-scalar scenes only.
* `foir-walkfuzz` (any architecture; `make check` runs 40,000 primitives) checks the per-row edge walker (`walk_row`, `src/walk.h`) against the sequential walker of the
  previous design: `tools/walk_ref.inc` is `walk_edges()` as it was at commit c73eb8e (every subscanline from the top vertex, repeated additions, one `struct span` per
  row), kept verbatim as the oracle. Random primitives -- ordinary triangles and rectangles, vertices anywhere in the 14-bit range (up to 12,000 subscanlines of
  walking before the first visible row), full-range 28-bit edge positions and 30-bit slopes and 32-bit interpolant values, so that the products wrap around many times, fully
  random records, random scissors (also the reset one), interlace fields and cycle types -- go through both; compared are the visibility, the first and last row, the
  per-primitive constants (`r->pc`) and, for every row, the validity and every field of the span; the rows are computed in ascending and in shuffled order, and in the aarch64
  build by both the NEON and the portable walker. Mutations of the row jump and of the middle-vertex switch are caught within the first 60 primitives (see [the results](#edge-walker-on-the-workers-walk_row)).
* `foir-kfuzz` (aarch64 only) draws random render states through the real command parser, random TMEM /
  framebuffer / Z contents and random pixel batches and requires every NEON stage to equal the scalar stage;
  the store stage is checked on the resulting RDRAM including the hidden bits. **`foir-kfuzz -S` fuzzes the specialised
  variants**: iteration *i* takes variant *i* (of all five stages) from the tables, writes its key over a random
  render state (`foir_spec_apply`), checks that the selection picks exactly that variant (for an "any option" variant: after
  drawing the options until no exact variant matches), runs the whole chain through the selected kernel table and compares with
  the scalar chain (the generic NEON chain for a variant with RelaxedPrecision). Every variant must be hit
  (`make check` runs 15,000 iterations over all 492 table entries, 15,000 more over the 147 shape kernels only, `-S -T 5`, and 8,000 over the texture table, `-S -T 0`). An "any format" texture kernel is
  selected with `FOIR_SPEC_TEX=shape` semantics (exact variants skipped) on a state with random tile formats. **`foir-kfuzz -m 32` fuzzes the COPY span kernel** (stage 32; `make check` runs 20,000 iterations). For the shape kernels the iteration draws random
  operands of the kernel's shape instead of writing a key (`foir_shape_apply`); the batch constants the operand table points at are copied into the fuzzer's batch (`foir_shape_export`).
* `mtfuzz.py` random scenes (see above) now take half of their combiner / blender modes from the variant lists, so the
  variants run in the multi-threaded, row-ownership stress scenes too; it prints the share of specialised pixels it reached.
* `regress.py` compares the scalar renderer with angrylion-rdp-plus (needs `make ref`).
* The offline tools replay with the accuracy-preserving knob settings; `foir-replay --fast` uses the plugin
  defaults, `-t N` selects the worker count, `--scalar` the reference kernels, `--setup-only` skips the pixel stage.
* Captured frames: `FOIR_DUMP=/path/prefix FOIR_DUMP_FRAME=n` writes a two-frame dump for `foir-replay` and
  `ref-angrylion`.

```sh
make replay ref NEON=0
FOIR_DUMP=/tmp/smb FOIR_DUMP_FRAME=300 mupen64plus ...        # capture a real frame (2 frames)
./foir-replay /tmp/smb_300.fdump -p out.ppm                   # replay it offline (also on-device)
./ref-angrylion /tmp/smb_300.fdump -o ref.rdram                # replay through angrylion
```

`FOIR_LOGFILE=path` additionally writes the plugin's log lines to an unbuffered file; every 120 frames it logs
wall time per frame, RDP time (parse + setup, of which time waiting at `SYNC_FULL`), VI convert / present time,
pixels, primitives and skipped frames.

## Known gaps

* No VI filters (anti-aliasing, divot, gamma, dither) and no `Filter`-style 480i deinterlacing beyond the
  `InterlaceHack`; `FBRead` / `FBWrite` only drain the workers (no framebuffer emulation for CPU effects).
* NEON falls back to the scalar kernels for YUV tiles, chroma key and 4 / 8-bit framebuffers; COPY-cycle
  rectangles are NEON only for 16-bit colour buffers and texture rectangles (8-bit colour buffers, COPY triangles and YUV texels without TLUT are scalar: none of them occurs in the 62 captured games). The scalar path remains the exact reference.
* **Specialised variants (M3).** The variant lists were built from the first minute of 23 ROMs plus 11 captured frames (intros, menus,
  demos, early levels), not from full playthroughs; a state without a variant simply runs the generic NEON kernel of that stage (at
  the speed of M2). The combiner was the weak spot: its selector sets are game specific, so a game outside the lists gets roughly
  70 % of its combiner pixels through an exact variant (see [Specialisation coverage](#specialisation-coverage)); the shape kernels bring that to
  about 90 % (leave-one-game-out), the rest of the combiner pixels (mostly two-cycle states with a rare combination of equation shapes) still run in the generic kernel; the other
  stages 80-100 %. (After the second tier below: texture and Z compare 99.9-100 % out of sample.) There are no variants yet for 32-bit colour buffers, YUV, chroma key, FILL (those paths are unchanged),
  and the texture variants assume that all tiles a mip-map chain can select share one format (checked per batch, else generic).
  Groups of 8 pixels that straddle two scanlines (10-30 % of the groups, 45 % in Paper Mario, see [Colour / Z buffer access](#colour--z-buffer-access)) still take the scalar
  per-pixel path in the colour / Z buffer access; a vector form of it was measured and does not pay on the Cortex-A55.
* **Shape kernels.** Cover the equation-shape combinations seen in 62 games (147 kernels); the leave-one-game-out estimate for a game outside them is 89.8 % of its combiner
  pixels, 6 of 62 games below 50 %. A combination no kernel has (a rare cycle-1 shape paired with another cycle-0 shape) runs in the generic combiner. A sharpened LOD fraction in a multiply input and wide
  texels (YUV tiles, BI_LERP = 0) need the `ext` form of a kernel, which exists only for the shapes that were seen with a LOD fraction multiply input (no `ext` kernel for wide texels).
  Both options are exercised by the fuzzers; the dumps contain sharpen modes (Pilotwings 64, SpaceStation Silicon Valley) but whether their combiners read the LOD fraction there was not checked. The shape kernels were verified offline (replay) and by fuzzing, and in game on the RK3566 with `FOIR_NEON_VERIFY=1`
  (exact knobs): Super Mario 64 and Pokemon Snap with `FOIR_SPEC_COMB=shape` (100 % of combiner batches in shape kernels, ~2.0 M / 1.1 M
  batches, 0 mismatching; SM64 frames 600 / 1200 byte-identical to the exact-variant run) and SM64 / Banjo-Kazooie / Snap in `auto`. Timings are from the offline replay (inline and 3 workers), not from in-game frame times.
* **Second tiers of the texture unit and the Z compare.** The 38 "any format" texture kernels are the 30 structures seen in 62 games plus 8 structural LOD forms (leave-one-game-out 99.9 % of a game's textured pixels, worst game 98.4 %; the exact tier alone
  92.1 %); a structure nobody has used runs in the generic kernel, and they cost 11-23 % more texture-stage time than an exact variant (stage timer). Exact variants with MID_TEXEL set exist only for the keys that were seen; every other MID_TEXEL state runs in an "any format" kernel.
  The Z compare is complete for 16-bit colour buffers only (32-bit colour buffers: generic kernel, 4 / 8-bit: scalar). The coverage tables are in-sample (the lists were made from the same 62 games); the out-of-sample figures are the leave-one-game-out estimates.
* **What was checked how, for the COPY kernel and the buffer access.** Offline replay (184 dumps with `FOIR_NEON_VERIFY=1`, 27 scenes in `neoncheck`), fuzzers and in game for Paper Mario, Rocket, Mischief Makers, Super Mario 64, Donkey Kong 64 (and Banjo-Kazooie), 60-75 s each. The row-ownership
  rule (an odd-aligned window and a COPY window must not leave its row) is only tested for equal output, not with a data-race detector (ThreadSanitizer cannot run the NEON kernels on x86_64). `flip = 0` COPY spans and 8-bit colour buffers are only exercised by the fuzzers, where they take the scalar kernel on both sides. The in-game
  profile covers two games and is noisy to about 10 %; the three-worker replay timings of the two-frame dumps are noisy to the same order (see the tables).
* **LOD.** One-cycle perspective LOD with a max level above 0 is not angrylion-exact in the scalar reference (`XSCENES` in `tools/regress.py`; the far part of the plane differs), the NEON path follows the scalar reference. Detail and sharpen occur in no captured frame
  (fuzzers and synthetic scenes only). A point-sampled two-cycle LOD state has no variant (generic kernel). The variants require one texel format along `tilenum .. tilenum + max_level (+ 1 with detail)`, as before but without the two spare tiles.
* Some hidden-bit and COPY-mode details are still approximations (inherited from M1).
* The pseudo random noise is not the hardware sequence.
* The parallel-rsp pairing was verified on the RK3566 (aarch64) only; the x86_64 plugin builds but was not run in an emulator.
  The 23-ROM comparison covers the first minute of each game (intros, demos, early gameplay), not full playthroughs.
* `FrameSkip=auto` is a simple window-based controller and does not know about render-to-texture frames.

## License

**GPLv3 or later** (like the sibling plugins; foir no longer needs GNU Lightning, whose LGPLv3+ was the original reason for GPLv3+).

[angrylion-rdp-plus]: https://github.com/ata4/angrylion-rdp-plus
