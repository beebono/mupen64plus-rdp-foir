# Performance

Measured results, condensed from the development log. Everything here is output-neutral: each step is bit-exact with the scalar reference at exact settings ([testing.md](testing.md)). How the pieces work is in [design.md](design.md).

## Method

- **Rigs.** RK3566 (ROCKNIX): 4x Cortex-A55, `performance` governor (the real clock is about 1.6 GHz). H700 RG35XX SP (spruce): 4x Cortex-A53, Performance mode at 1512 MHz. Clocks were pinned for timing runs and restored afterwards.
- **Builds.** `-O3`, generic armv8-a, GCC 13. `-mcpu=cortex-a55` was measured performance-neutral (replay 1-2.5 % faster on one thread, 2-5 % slower with three workers, in-game frame times unchanged) and is not used.
- **Offline replay** (`foir-replay`, [testing.md](testing.md#test-data-capture-and-replay)): the RDP command stream of a captured frame (a two-frame dump, or a 60-frame window) is rendered without the emulator. Builds being compared are run alternately in one session (order rotated each round), medians of 3-15 rounds. Differences below about 2 % are noise at one thread, 5-10 % with three workers. With workers, the replay is mostly the wait at `SYNC_FULL`, so it is the upper bound of what a change brings in game.
- **In game**: rsp-hle with parallel-rsp as `RspFallback` unless noted, `Threads = 3`, default knobs, scripted input, 75 s runs, windows after VI frame 1440. "RDP" is the emulation thread's time in the RDP per frame (parse + setup + the wait for the workers at `SYNC_FULL`); "CPU" is user + system time of the whole process per frame including the workers' idle spin.
- The captured frames are of commercial ROMs and are not distributed; `test-data/README.md` explains how to regenerate them.

## Headline

- Offline replay on the RK3566, five games, against the scalar reference renderer: NEON alone **2.6-4.3x**, NEON with three workers **6.1-12.6x**. The specialised kernels added **1.25-1.65x** on one thread and **1.2-1.5x** with three workers on top of that, and later work (COPY kernel, buffer access, second tiers, batch builder, edge walker, LOD path, helper) more (tables below).
- In game, 60 fps cap (59-60 fps) on both devices in Super Mario 64, Donkey Kong 64, Banjo-Kazooie, Paper Mario and Ocarina of Time; Pokemon Snap, the heaviest of the measured games, runs 44.2 fps (H700) and 46.9 fps (RK3566), up from 38.5 and 43.5 before the emulation thread helped render at barriers.

## Offline replay

### Scalar, NEON and threads

RDP time per frame in ms (RK3566, mean of 15 runs of a two-frame dump, including the wait for the workers); "scalar" is the scalar reference renderer, the NEON columns are the NEON kernels before specialisation.

| game (frame) | pixels/frame | scalar | NEON | NEON + 1 worker | + 2 | + 3 | speed-up NEON | speed-up 3 workers |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Super Mario 64 | 59k | 39.0 | 15.0 | 13.8 | 7.7 | **5.8** | 2.6x | **6.7x** |
| Star Fox 64 | 110k | 54.8 | 15.2 | 16.5 | 8.1 | **6.8** | 3.6x | **8.1x** |
| Donkey Kong 64 | 39k | 31.5 | 11.8 | 10.9 | 6.2 | **5.1** | 2.7x | **6.1x** |
| Banjo-Kazooie | 170k | 138.6 | 32.1 | 34.2 | 17.1 | **11.0** | 4.3x | **12.6x** |
| Pokemon Snap | 230k | 251.3 | 59.7 | 60.0 | 30.6 | **21.5** | 4.2x | **11.7x** |

These columns use the accuracy-preserving settings and are bit-identical to each other and to the scalar reference. A single worker is about as fast as the inline NEON path (parsing and setup stay on the emulation thread); going from 1 to 3 workers is 2.1-3.1x on the four-core part, the rest being the serial parse / setup share, the barriers and band imbalance.

### Specialised kernels

Replay, ms per frame, `foir-replay --fast` (the plugin's default knobs), RK3566. "Before" is the renderer without specialised variants, "generic only" the new code with `Specialize = False`.

| game (frame) | before, 1 thread | **specialised, 1 thread** | speed-up | generic only, 1 thread | before, 3 workers | **specialised, 3 workers** | speed-up |
|---|---:|---:|---:|---:|---:|---:|---:|
| Super Mario 64 | 14.9 | **10.9** | **1.37x** | 15.0 | 5.4 | **4.2** | **1.29x** |
| Star Fox 64 | 15.3 | **9.9** | **1.56x** | 15.2 | 6.3 | **4.6** | **1.37x** |
| Donkey Kong 64 (36 % of pixels use LOD) | 11.2 | **8.9** | **1.26x** | 11.0 | 5.0 | **4.2** | **1.19x** |
| Banjo-Kazooie | 31.9 | **19.3** | **1.65x** | 32.4 | 10.8 | **7.3** | **1.49x** |
| Pokemon Snap | 57.7 | **39.8** | **1.45x** | 57.7 | 21.6 | **14.8** | **1.46x** |
| GoldenEye 007 (LOD) | 4.1 | **3.3** | **1.25x** | 4.0 | 1.9 | **1.4** | **1.40x** |
| Paper Mario | 11.9 | **7.9** | **1.50x** | 12.0 | 4.4 | **3.1** | **1.41x** |

The generic kernels are as fast as before, so the refactoring into one body per stage cost nothing. With three workers the gain is a little smaller because the serial part does not shrink. A variant removes more than branches: the per-batch table set-up, operand-descriptor loads, shade / cycle-0 round trips through memory and the texture format switches.

### Combiner shape kernels

Cost of the operand table (the same states run through an exact variant, a shape kernel and the generic kernel, 12 captured frames, combiner-stage timer): a shape kernel costs **1.15-1.65x an exact variant** (median 1.3x; +1.5 to +3.6 ns per pixel), which is **0-5 % of the replay time**, and the generic kernel costs 2.2-3.1x a shape kernel and 2.5-4.2x an exact variant. The dispatch order is therefore exact, shape, generic. Benefit on games the exact variants do not cover (60-frame windows; "before" = exact variant or generic kernel, "auto" = exact, shape, generic). The shipped list was made from the statistics of 62 games, so this is in-sample, and no pixel is left to the generic kernel:

| window | combiner pixels: exact / shape-only / generic | combiner ms: before -> auto | replay ms: before -> auto | change |
|---|---|---:|---:|---:|
| Toy Story 2 (4200) | 0 % / 100 % / 0 % | 150.4 -> 63.7 | 544 -> 449 | -17.5 % |
| Quest 64 (3000) | 4 % / 96 % / 0 % | 172.9 -> 70.5 | 765 -> 653 | -14.6 % |
| Mario Golf (1800) | 11 % / 89 % / 0 % | 184.9 -> 78.8 | 1093 -> 979 | -10.4 % |
| Conker (3000) | 0 % / 100 % / 0 % | 133.2 -> 60.0 | 642 -> 556 | -13.5 % |
| Pokemon Stadium 2 (1800) | 26 % / 74 % / 0 % | 413.1 -> 163.6 | 1228 -> 975 | -20.6 % |
| Gauntlet Legends (1800) | 36 % / 64 % / 0 % | 269.8 -> 124.8 | 1696 -> 1547 | -8.8 % |
| Tetrisphere (1800) | 53 % / 47 % / 0 % | 171.1 -> 96.7 | 1438 -> 1351 | -6.1 % |
| Super Mario 64 (4200) | 100 % / 0 % / 0 % | 88.5 -> 87.6 | 941 -> 936 | -0.5 % |

On states only a shape kernel covers, the combiner stage is 2.2-2.5x faster than with the generic kernel and the whole replay 7-21 % faster; games fully covered by exact variants do not change. With a kernel list made only from the 23 test ROMs (out of sample for the 39 other games), games that use shape combinations the 23 lack stay generic (Toy Story 2 and Quest 64 completely), the others gain 2-13 %.

### Second tiers, COPY kernel and buffer access

Geometric mean of the per-dump speed-ups over the previous renderer (medians of 3-4 alternating rounds, `--fast`):

| set | + COPY kernel | + buffer access | + tex / Z tiers | all three | speed-up |
|---|---:|---:|---:|---:|---:|
| 12 captured frames, 1 thread | -11.0 % | -4.0 % | +0.9 % | -13.7 % | 1.16x |
| 12 captured frames, 3 workers | -6.3 % | -2.8 % | +1.3 % | -11.1 % | 1.12x |
| 15 gameplay windows, 1 thread | -13.5 % | -3.3 % | -3.2 % | -19.4 % | 1.24x |
| 15 gameplay windows, 3 workers | -11.0 % | -4.1 % | -1.2 % | -17.1 % | 1.21x |

- **COPY (NEON `neon_copy`)** is a large win where a game uses COPY rectangles at all (Hydro Thunder 2.67x on the captured frame and 3.37x on a gameplay window, Mischief Makers 2.25x, Kirby 64 1.29x, Paper Mario 1.12-1.26x) and costs nothing elsewhere. In the in-game profile it took COPY from 20 % to 6 % of foir's CPU time in Paper Mario and from 13 % to 3 % in Rocket Robot on Wheels.
- **Colour / Z buffer access** (inlined, bounds check per batch, prefetch) is 3-7 % of every 3D frame.
- **The texture "any format" tier and the complete Z-compare list** are the big win for games the lists did not know (Pilotwings 64 1.33x, Earthworm Jim 3D 1.17x, Mario Golf 1.31x together with the COPY kernel, Turok and Toy Story 2 about 1.06x) and a small cost (+0.9 % geometric mean) on frames that were covered already.

### Z compare

Pokemon Snap's attract-demo zoom shot (VI frames 1100-1340: 11.4 colour pixels processed per pixel, 58 % of them failing the Z compare) spent 19 % of the renderer in `zcmp`; frame 1101 on one thread: texture 38.5, combine 10.3, **zcmp 20.8**, blend 4.7, store 7.5 of 112.3 ms. Profile of that frame (RK3566, `perf`, `--fast`): 276 instructions and 336 cycles per 8-pixel group (IPC 0.82), 1.2 L1D refills and 0.8 branch mispredictions per group, 7 k L2 refills per frame: instruction bound, not miss bound. By code: 47 % arithmetic and stores, 28 % in the colour / Z read functions (they hold the per-lane loops for the 26 % of groups that are not one run: 22 % straddle two scanlines, 3 % have more runs, 1 % end a batch), 13 % in the vector windows, 5 % the contiguity test, 4 % the hidden-bit resolve. With exact settings the opaque compare takes the fast path only when all eight lanes overflow coverage, which 46 % of the opaque groups of this frame do; the rest run the 4-lane delta-Z comparator (11 % of the cycles; 410 cycles per group). All `zcmp` pixels were already in specialised variants (100 % in every dump, no entries added to `spec_zcmp.def`). Dead groups (no lane passes): 35 % of the opaque and 78 % of the transparent groups of the frame.

What changed (`kneon_mem.c`, bit-exact; [design.md](design.md#pixel-stages)): a group that is two runs of consecutive pixels (end of a scanline, start of the next) is read as two windows merged by a lane mask instead of per lane; the buffer reads are issued before the arithmetic; the Z decode is `Q = 0x40000 - oz` in 16-bit lanes (`sz + Q < 0x40000`, farthest depth is `zpx >= 0xfffc`); `sz` saturation is `min` / `compare` / `and`; the hidden bits and memory coverage are resolved in 8-bit lanes; the blender shifts are saturating byte subtractions; masks are reduced with a 64-bit move instead of `uminv`; the exact path for partially covered pixels and the decal mode run on eight lanes (`z_compare8`). Instructions per group 276 -> 212 (`--fast`), 331 -> 247 (exact); cycles 336 -> 279 and 410 -> 313.

One-thread replay, medians of 5 alternating rounds, ms per run (A55: RK3566, `performance` governor; A53: H700, 1512 MHz):

| dump | A55 `--fast`: zcmp | A55 exact: zcmp | A53 `--fast` | A53 exact |
|---|---|---|---|---|
| Snap 1101 (zoom) | 20.78 -> 17.22 (-17 %), total -1.7 % | 24.60 -> 19.60 (-20 %), total -3.6 % | 20.79 -> 16.59 (-20 %), total -4.0 % | 25.43 -> 18.84 (-26 %), total -5.9 % |
| Snap 1301 (zoom) | 17.91 -> 14.80 (-17 %), total -2.9 % | 21.53 -> 16.83 (-22 %), total -3.7 % | 18.04 -> 14.28 (-21 %), total -3.9 % | 21.91 -> 16.28 (-26 %), total -5.7 % |
| Snap 2101 (foliage) | 15.98 -> 13.24 (-17 %), total -3.8 % | 19.29 -> 14.96 (-22 %), total -5.3 % | 16.18 -> 12.82 (-21 %), total -4.4 % | 19.58 -> 14.65 (-25 %), total -5.3 % |
| Snap 701 | 11.42 -> 9.24 (-19 %), total -3.6 % | 13.37 -> 10.54 (-21 %), total -3.6 % | 11.48 -> 9.05 (-21 %), total -4.1 % | 13.77 -> 10.18 (-26 %), total -5.0 % |
| Banjo-Kazooie 450 | 5.38 -> 4.50 (-16 %), total -4.3 % | 6.12 -> 4.86 (-21 %), total -4.0 % | 5.30 -> 4.34 (-18 %), total -4.7 % | 6.27 -> 4.83 (-23 %), total -5.9 % |
| Donkey Kong 64 2400 | 4.57 -> 4.04 (-12 %), total -1.9 % | 5.08 -> 4.34 (-15 %), total -2.2 % | | |
| Super Mario 64 650 | 3.19 -> 2.84 (-11 %), total -2.4 % | 3.20 -> 2.87 (-11 %), total -1.6 % | 3.29 -> 2.99 (-9 %), total -1.8 % | 3.28 -> 2.99 (-9 %), total -2.1 % |
| Paper Mario 900 | 1.02 -> 0.85 (-17 %), total -2.2 % | 1.05 -> 0.84 (-20 %), total -2.1 % | 1.03 -> 0.83 (-20 %), total -2.0 % | 1.04 -> 0.83 (-20 %), total -1.9 % |
| Kirby 64 600 | 5.48 -> 4.59 (-16 %), total -3.0 % | 5.90 -> 4.95 (-16 %), total -3.6 % | | |
| Star Fox 64 1000 | 2.81 -> 2.44 (-13 %), total -2.8 % | 2.82 -> 2.49 (-11 %), total -2.0 % | | |
| **geometric mean** | zcmp **-14.6 %**, total -2.5 % (25 dumps) | zcmp **-16.5 %**, total -2.6 % | zcmp **-19.3 %**, total -3.8 % (12 dumps) | zcmp **-22.8 %**, total -4.7 % |

The other stages are unchanged (frame 1101, `--fast`: texture 38.5 -> 39.0, combine 10.3 -> 10.5, blend 4.7 -> 4.8, store 7.5 -> 7.5 ms). GoldenEye 007 (zcmp 0.5 ms) and Hydro Thunder (COPY cycles, no Z) do not move.

In game, Pokemon Snap attract demo without input, `Core[RandomizeInterrupt] = False`, three workers, `FOIR_FRAMELOG`, 240 s (RK3566, `performance`) / 270 s (H700, 1512 MHz, started from the PyUI hand-over, cooled to 52 C between runs), old / new alternated, three runs each; median of the three (range in brackets), frame time = swap to swap in ms:

| | window | p50 | p95 | p99 | max | > 33.3 ms | mean busy (sum of workers) |
|---|---|---|---|---|---|---|---|
| RK3566 old | zoom [1100, 1340) | 35.9 (35.7-36.1) | 39.6 (39.3-40.2) | 43.7 (43.3-46.5) | 48.9 (48.6-51.0) | 69.6 % (68.3-70.0) | 92.1 |
| RK3566 new | zoom | **34.9** (34.8-34.9) | **38.4** (38.1-39.3) | **41.7** (41.5-41.9) | 44.8 (43.2-49.6) | 65.4 % (60.8-71.7) | 89.4 |
| RK3566 old | whole run (frames > 100) | 17.0 | 32.1 (31.9-32.2) | 36.8 (36.8-36.9) | 206.9 | 3.7 % (3.6-3.8) | 26.1 |
| RK3566 new | whole run | 17.0 | **31.1** (31.0-31.4) | **35.8** (35.6-35.8) | 204.7 | 3.3 % (3.2-3.4) | 25.7 |
| H700 old | zoom | 37.3 (37.2-37.3) | 42.2 (41.4-42.6) | 45.5 (45.0-46.5) | 49.8 (46.9-51.4) | 97.1 % (95.8-97.9) | 93.7 |
| H700 new | zoom | **36.2** (36.1-36.4) | **41.2** (40.8-41.2) | **44.5** (44.1-46.0) | 48.0 (46.8-49.2) | 82.9 % (82.1-89.6) | 90.5 |
| H700 old | whole run (frames 100-12000, the frame log's cap) | 16.8 | 32.8 | 38.2 | 203.8 | 4.4 % (4.3-4.5) | 24.6 |
| H700 new | whole run | 16.8 | **31.9** | **37.2** | 203.6 | 4.0 % (4.0-4.1) | 23.9 |

The shot stays over 33.3 ms in both builds: 2.7 ms off the 92 ms the workers spend (0.9 ms off the busiest worker's 31) buys about 1 ms of frame time. The spikes are the same frames before and after (boot frame 137 at 204 ms, scene changes 2714 / 5865-5868 / 6169 with the emulation thread busy and no RDP work; the zoom shot's worst frames are the 1.18 Mpix frames with 28 ms of `SYNC_FULL` wait). The share over 16.7 ms is not meaningful here (vsync-locked frames sit at 16.8-17.0 ms).

### Batch builder

The batch builder (`raster_render_job`) was 17-23 % of foir's CPU time in game and instruction bound (about 800 cycles per span plus 17 per pixel). The edge-zone coverage and the fixed-size ramps ([design.md](design.md#batch-builder)) took the instructions per span in `raster_render_job` from 708-1414 to 498-996 and its branch mispredictions per span from 6.3-18 to 2.4-5.6.

| dump | before (ms/run, 1 thread) | after | change |
|---|---:|---:|---:|
| Paper Mario (900) | 12.52 | 11.95 | -4.6 % |
| Banjo-Kazooie (450) | 36.86 | 35.27 | -4.3 % |
| Donkey Kong 64 (800) | 16.68 | 15.68 | -6.0 % |
| Pokemon Snap (300) | 75.91 | 72.16 | -4.9 % |
| Rocket Robot on Wheels (1500) | 28.49 | 27.01 | -5.2 % |
| Super Mario 64 (650) | 20.98 | 19.21 | -8.4 % |
| Star Fox 64 (1000) | 18.70 | 18.15 | -2.9 % |
| GoldenEye 007 (900) | 6.43 | 6.03 | -6.2 % |
| **geometric mean** | | | **-5.3 % (1.06x)** |

With three workers the geometric mean is -6.7 % (13 rounds pooled), noisy; Paper Mario and Star Fox 64 are limited by the parser thread and do not move. Whole-replay counters: instructions -6.5 to -8 %, branch mispredictions -33 to -43 %, cycles -1.7 to -5.7 %.

### Edge walker on the workers

Moving the edge walker from the emulation thread to the workers ([design.md](design.md#edge-walker-on-the-workers)) removed one 96-byte span record per scanline that the parser wrote and the workers, on other cores, read (Pokemon Snap: 2.5 MB written per frame and 2.5 MB read, against 228 KB of walker records). The sequential walk cost 173-175 ns a row; `walk_row` costs 41-113 ns (median 80 ns).

- Replay, one thread: geometric mean **0.976** (-2.4 %; Donkey Kong 64 -5.4 %, GoldenEye -7.4 %, Banjo-Kazooie +1.5 %).
- Replay, three workers: 0.971; the emulation thread's own work (parse, setup, queueing) falls by 61 %, and the wait at `SYNC_FULL` absorbs it, since the workers set the frame time.
- In game (RK3566, nine games): the emulation thread's RDP time falls by 18-27 % in seven of nine games (geometric mean -18 %), its own work by 44 %, process CPU per frame by 6-11 % in the same seven (-7 % over all nine):

| game | emulation thread RDP ms/frame | process CPU ms/frame |
|---|---|---|
| Banjo-Kazooie | 3.36 -> 2.64 | 34.37 -> 31.34 |
| Donkey Kong 64 | 5.28 -> 4.30 | 40.60 -> 36.00 |
| Mario Kart 64 | 2.23 -> 1.63 | 26.01 -> 23.70 |
| Paper Mario | 3.06 -> 2.42 | 25.17 -> 22.58 |
| Star Fox 64 | 1.60 -> 1.22 | 20.46 -> 19.22 |
| Super Mario 64 | 2.11 -> 1.62 | 23.94 -> 21.86 |
| Ocarina of Time | 1.84 -> 1.36 | 17.89 -> 16.01 |
| Pokemon Snap (RDP-bound) | 2.17 -> 2.19 | 11.80 -> 11.97 |
| Toy Story 2 (RDP-bound) | 1.96 -> 1.98 | 12.86 -> 12.82 |

All nine stay at the frame-rate cap; what changes is headroom. In the RDP-bound games the frame ends when the workers have rendered it, and the workers now also walk.

### LOD / mip-map texture path

The profile listed "texture helpers" as 31.4 % of foir's CPU time in Banjo-Kazooie and 23.8 % in Donkey Kong 64. They were not scalar code: they were the out-of-line NEON functions that the *generic* texture kernel calls, and 45-62 % of the LOD batches in those games were handed to the generic kernel because (a) the LOD variants checked two tiles too many past the end of the mip chain, and (b) point-sampled LOD (Donkey Kong 64) had no variant. The fix: the chain bound, `lod_pass()` computing the LOD of a whole batch in one call with the neighbouring pixel's divide reused, a one-tile fast path, a cheaper perspective divide (`tc_divide4`), and variants for point-sampled LOD. The share of textured pixels in the generic kernel is 0 % in every captured LOD dump (it was up to 39.5 %).

| dump | before (ms/run, 1 thread) | after | change |
|---|---:|---:|---:|
| Banjo-Kazooie (1800) | 40.66 | 34.43 | -15.3 % |
| Banjo-Kazooie (2400) | 45.56 | 39.02 | -14.3 % |
| Banjo-Kazooie (3000) | 38.42 | 33.20 | -13.6 % |
| Donkey Kong 64 (2400) | 44.63 | 40.33 | -9.6 % |
| Donkey Kong 64 (3000) | 37.97 | 33.20 | -12.6 % |
| Donkey Kong 64 (800) | 14.91 | 13.91 | -6.7 % |
| Rocket Robot on Wheels (1500) | 26.57 | 25.56 | -3.8 % |
| GoldenEye 007 (900) | 5.57 | 5.31 | -4.6 % |

With three workers the LOD dumps are 6-18 % faster. Dumps without LOD stay within +-1.5 %. In game the emulation thread's RDP time falls 16 % in Donkey Kong 64 (4.24 -> 3.55 ms), 13 % in Banjo-Kazooie and 12 % in Paper Mario, and process CPU 5.9 %, 2.2 % and 3.8 %; the profile share of the texture helpers falls from 32.5 to 14.8 % (Banjo-Kazooie) and from 25.4 to 12.4 % (Donkey Kong 64).

### The emulation thread as helper

Replay, three workers, ms per run (pinned clocks, medians of 3 rounds); "before" has no helper:

| dump | H700 (A53) before -> after | RK3566 (A55) before -> after |
|---|---|---|
| Banjo-Kazooie (450) | 13.34 -> 9.95 | 12.90 -> 9.44 |
| Donkey Kong 64 (800) | 6.66 -> 5.08 | 5.72 -> 4.83 |
| GoldenEye 007 (900) | 2.60 -> 1.84 | 2.75 -> 1.86 |
| Paper Mario (900) | 5.02 -> 3.74 | 4.77 -> 3.80 |
| Pokemon Snap (300) | 27.61 -> 21.36 | 25.39 -> 21.59 |
| Rocket Robot on Wheels (1500) | 10.90 -> 8.69 | 10.08 -> 7.83 |
| Star Fox 64 (1000) | 8.76 -> 6.17 | 7.09 -> 5.71 |
| Super Mario 64 (650) | 8.11 -> 6.39 | 7.07 -> 5.95 |
| **geometric mean** | **0.753** | **0.788** |

On one thread there is nothing to help (0.998 / 0.994). The new claiming protocol with the helper switched off is 2-5 % faster than the old fixed partition, since rows of other classes are no longer visited.

## In game

### RSP pairing

RK3566, `Threads = 3`, default knobs, mean of 2 runs, frame time (fps) and the emulation thread's time in the RSP plugin per frame:

| game (window end) | rsp-cxd4 | parallel-rsp standalone | rsp-hle + parallel-rsp fallback |
|---|---|---|---|
| Super Mario 64 (600) | 28.0 ms / **36 fps**; RSP 24.6 ms | 19.7 ms / **51 fps**; RSP 16.4 ms | 17.8 ms / **56 fps**; RSP 14.4 ms |
| Star Fox 64 (1080) | 60 fps; RSP 11.5 ms | 60 fps; RSP 9.7 ms | 60 fps; RSP 7.9 ms |
| Donkey Kong 64 (1080) | 60 fps; RSP 7.8 ms | 60 fps; RSP 7.1 ms | 60 fps; RSP 6.2 ms |
| Banjo-Kazooie (600) | 60 fps; RSP 11.6 ms | 60 fps; RSP 11.4 ms | 60 fps; RSP 10.9 ms |
| Pokemon Snap (600) | 43.8 ms / **23 fps**; RSP 40.8 ms | 34.0 ms / **29 fps**; RSP 30.8 ms | 32.5 ms / **31 fps**; RSP 29.3 ms |

The first parallel-rsp JIT gave 1.3-1.7x less RSP time than cxd4; the version with the vector unit and vector load / stores emitted inline (output bit-identical) takes Super Mario 64 to the 60 fps cap (RSP time -42 %, 14.2 -> 8.3 ms with the rsp-hle fallback). Pokemon Snap does not get faster from the RSP side alone: its RSP "net" time halves, but the emulation thread then waits about 11 ms per frame at `SYNC_FULL` for the workers, so Snap is RDP-bound and its frame time has to come from the RDP.

### Frame rate by worker count

RK3566, rsp-cxd4 as the RSP, the NEON kernels before specialisation; RDP time per frame and total frame rate (60 = capped at the VI rate):

| game (VI frames ending at) | scalar | NEON | NEON + 1 worker | + 2 | + 3 |
|---|---:|---:|---:|---:|---:|
| Super Mario 64 (600) | 35.6 ms / **16 fps** | 13.9 ms / **25 fps** | 2.1 ms / **35 fps** | 2.1 ms / **35 fps** | 2.1 ms / **36 fps** |
| Star Fox 64 (1080) | 55.2 ms / **16 fps** | 14.6 ms / **43 fps** | 13.5 ms / **45 fps** | 7.1 ms / **60 fps** | 5.0 ms / **60 fps** |
| Donkey Kong 64 (1080) | 48.5 ms / **17 fps** | 22.1 ms / **30 fps** | 17.5 ms / **34 fps** | 7.8 ms / **50 fps** | 4.9 ms / **58 fps** |
| Banjo-Kazooie (600) | 135.7 ms / **7 fps** | 29.4 ms / **26 fps** | 23.4 ms / **31 fps** | 10.0 ms / **53 fps** | 5.8 ms / **59 fps** |
| Pokemon Snap (600) | 345.0 ms / **3 fps** | 75.3 ms / **9 fps** | 39.2 ms / **13 fps** | 9.1 ms / **22 fps** | 7.8 ms / **23 fps** |

With cxd4 the emulation thread (cxd4 + CPU interpreter + command parsing), not the RDP pixel work, limits the frame rate at three workers. The windows are real-time play, so scenes drift a few frames between runs.

### Specialised kernels, in game

rsp-hle + vectorised parallel-rsp fallback, `Threads = 3`, mean of the 120-frame window in two runs. Frame time ms (fps); CPU = process CPU ms per frame.

| game (window end) | renderer | frame time (fps) | RDP | CPU |
|---|---|---:|---:|---:|
| Super Mario 64 (600) | before | 16.6 (60) | 2.30 | 33.7 |
| | **specialised** | 16.7 (60) | 2.25 | 33.3 |
| Star Fox 64 (1080) | before | 16.6 (60) | 6.24 | 29.0 |
| | **specialised** | 16.7 (60) | 4.82 | **24.0** |
| Donkey Kong 64 (1080) | before | 16.7 (60) | 4.92 | 27.0 |
| | **specialised** | 16.7 (60) | 3.70 | **22.7** |
| Banjo-Kazooie (600) | before | 16.8 (60) | 9.25 | 43.9 |
| | **specialised** | 16.6 (60) | 5.48 | **29.9** |
| Pokemon Snap (600) | before | 33.2 (30) | 19.55 | 114.1 |
| | **specialised** | **24.9 (40)** | 11.04 | **82.8** |

Pokemon Snap goes from 30 to 40 fps (RDP-bound once the RSP is fast) and uses 27 % less CPU per frame; in the games already at the cap the CPU per frame falls 12-32 %. Super Mario 64 is RSP- or cap-bound and shows no change. The 120-frame windows are demos and menus; there are only 2 runs per cell, with a spread of 1-3 % in frame time.

### The helper, in game

Both devices, rsp-hle + parallel-rsp fallback, `Threads = 3`, scripted input (Pokemon Snap: none), 75 s runs, 3-10 runs per cell, builds alternated; frame time is the time between buffer swaps.

| game | device | fps before -> after | frames > 36 ms before -> after | p99 frame ms before -> after |
|---|---|---|---|---|
| Pokemon Snap | H700 (A53) | 38.5 -> **44.2** | 121 -> 23 | 45.2 -> 36.9 |
| Pokemon Snap | RK3566 (A55) | 43.5 -> **46.9** | 24 -> 7 | 37.6 -> 33.3 |
| Donkey Kong 64 | H700 | 59.2 -> 59.4 | 43 -> 10 | 37.3 -> 32.8 |
| Donkey Kong 64 | RK3566 | 60.0 -> 60.0 | 16 -> 10 | 34.2 -> 33.4 |
| Banjo-Kazooie | H700 | 59.0 -> 59.1 | 22 -> 13 | 35.2 -> 33.1 |
| Banjo-Kazooie | RK3566 | 59.6 -> 59.6 | 19 -> 16 | 34.2 -> 33.5 |
| Ocarina of Time | H700 | 59.4 -> 59.4 | 17 -> 3 | 31.0 -> 29.2 |
| Super Mario 64 | H700 / RK3566 | 59.4 -> 59.4 / 59.9 -> 59.9 | 4 -> 5 / 10 -> 5 | 29.8 -> 25.9 / 31.9 -> 31.5 |
| Paper Mario | H700 / RK3566 | 59.4 -> 59.4 / 59.9 -> 59.9 | 8 -> 7 / 19 -> 9 | 31.2 -> 31.6 / 34.2 -> 32.4 |

On Pokemon Snap the emulation thread renders about 5.2 ms of every frame on the A53 (4.0 ms on the A55) that it used to spend asleep, and process CPU falls by 15 % because the workers idle less in their spin. The SYNC_FULL wait falls by 29-38 % in the other games. One open observation: Ocarina of Time on the RK3566 shows 32 -> 43 frames above 36 ms with identical fps and p50 / p95, a SYNC_FULL wait of only 0.5 ms (nothing for the helper to win in that scripted run), and overlapping run distributions; it was not explained. The remaining spikes are the same saturated frames, shorter: frames in which the busy times of the three workers add up to 50 ms or more still take 40+ ms even with the helper.

## Early Z

Batch-skip early Z ([design.md](design.md#pixel-stages)): the Z compare runs first where it cannot depend on the combiner, and a batch in which every lane failed Z skips texture, combine, blend and store. Output-neutral: RDRAM identical to the previous build on the 8 Pokemon Snap dumps below and on the 18 game dumps, NEON and scalar, 0 to 3 workers. Pokemon Snap's attract demo has a camera shot (VI frames about 1100-1340) with 11 layers per pixel on average, of which 58 % of the processed pixels fail Z.

Replay, `--fast`, ms per run (pinned clocks, medians of 3 rounds of `-n 5`, old and new alternated, spread under 2 % on one thread). Both builds from `toolchains/rg35xxsp`, the same binaries on both devices:

| Snap dump | RK3566 1 thread | | RK3566 3 workers | | H700 1 thread | | H700 3 workers | |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| | old -> new | change | old -> new | change | old -> new | change | old -> new | change |
| 1101 (camera) | 110.3 -> 94.2 | -14.6 % | 30.9 -> 26.0 | -15.8 % | 115.8 -> 98.0 | -15.4 % | 31.7 -> 27.4 | -13.7 % |
| 1201 (camera) | 107.9 -> 92.7 | -14.1 % | 29.2 -> 25.3 | -13.3 % | 114.2 -> 96.4 | -15.6 % | 31.9 -> 26.7 | -16.0 % |
| 1301 (camera) | 94.5 -> 80.7 | -14.6 % | 25.8 -> 22.4 | -13.4 % | 99.9 -> 84.4 | -15.5 % | 27.4 -> 23.7 | -13.3 % |
| 2101 (foliage) | 88.5 -> 84.6 | -4.4 % | 24.1 -> 23.3 | -3.3 % | 93.3 -> 88.5 | -5.1 % | 26.0 -> 24.7 | -5.0 % |
| 701 | 66.6 -> 61.6 | -7.5 % | 18.4 -> 17.5 | -4.8 % | 68.5 -> 64.3 | -6.0 % | 20.3 -> 18.3 | -9.5 % |
| 1701 (foliage) | 65.2 -> 61.1 | -6.4 % | 18.2 -> 17.3 | -5.3 % | 68.9 -> 63.9 | -7.2 % | 19.4 -> 18.8 | -2.9 % |
| 3702 | 53.1 -> 50.2 | -5.3 % | 15.5 -> 15.1 | -2.6 % | 55.4 -> 52.5 | -5.4 % | 17.5 -> 16.2 | -7.3 % |
| 4100 (no Z compare) | 9.31 -> 9.29 | -0.2 % | 3.19 -> 3.17 | -0.5 % | 9.90 -> 9.88 | -0.2 % | 3.13 -> 3.09 | -1.2 % |

The 18 game dumps of `test-data` (Banjo-Kazooie, Donkey Kong 64, Super Mario 64, Paper Mario, Star Fox 64, GoldenEye, Rocket, Hydro Thunder, ...; one thread): geometric mean new / old 0.982 (RK3566) and 0.984 (H700); Rocket Robot on Wheels -19.6 % / -18.3 %, Donkey Kong 64 (2400) -5.2 % / -3.3 %, Pokemon Snap (300) -3.1 % / -3.2 %, GoldenEye (3 workers) -9 % on the RK3566; the others between 1.6 % faster and 1.3 % slower, which is about the noise (the slower ones are scenes with little Z fail: Super Mario 64, Star Fox 64, Paper Mario, F-Zero X, Banjo-Kazooie 3000). With three workers the geometric mean is 0.985 / 0.992. The prototype of this change that was measured first (same order, test build) took 92.7 ms on frame 1101 against 94.2 ms here; not explained (the stage order and the live-lane test are the same; code layout is the guess). Switching the batch-build prefetch off gives the same gain (109.3 -> 93.0 ms), so the earlier `zcmp` does not lose the prefetch's benefit.

In game, Pokemon Snap attract demo with no input, `Core[RandomizeInterrupt]=False`, rsp-hle + parallel-rsp, 3 workers, FOIR_FRAMELOG on in both builds, pinned clocks (RK3566 performance governor, H700 Performance mode 1512 MHz), old and new alternated in sessions of three 75 s runs, 3 runs of each. Frame time is swap to swap; zoom window = VI frames [1100, 1340); median over the 3 runs (min-max):

| | p50 | p95 | p99 | max | > 16.7 ms | > 33.3 ms | fps | worker busy / SYNC_FULL wait |
|---|---|---|---|---|---|---|---|---|
| RK3566 zoom, old | 35.4 (35.3-35.5) | 39.7 (37.4-40.3) | 42.5 (41.8-46.6) | 46.5 (45.8-47.8) | 100 % | 65 % (62-71) | 28.7 | 90.8 / 15.6 ms |
| RK3566 zoom, new | 32.0 (31.8-32.0) | 36.0 (35.7-36.3) | 38.7 (38.6-38.9) | 42.1 (40.7-42.3) | 100 % | 10 % (10-15) | 31.7 | 80.9 / 12.1 ms |
| RK3566 whole run, old | 19.3 (19.0-19.4) | 35.1 (35.1-35.2) | 36.5 (36.4-37.1) | 204.5 (boot) | 74 % | 6 % | 46.0 | 44.5 / 5.1 ms |
| RK3566 whole run, new | 19.2 (19.1-19.3) | 32.0 (31.8-32.0) | 34.5 (34.3-34.6) | 204.9 (boot) | 79 % | 1 % | 47.4 | 42.8 / 4.4 ms |
| H700 zoom, old | 37.3 (37.1-37.3) | 42.9 (42.5-42.9) | 47.8 (46.2-48.8) | 50.7 (49.7-51.9) | 100 % | 99 % (97-100) | 27.1 | 93.5 / 17.6 ms |
| H700 zoom, new | 33.7 (33.6-34.0) | 38.2 (38.0-38.4) | 42.3 (41.2-43.6) | 48.8 (44.7-50.1) | 100 % | 56 % (55-60) | 29.9 | 83.4 / 14.2 ms |
| H700 whole run, old | 21.3 (21.3-21.4) | 37.3 (37.3-37.4) | 39.6 (39.5-40.4) | 204.9 (boot) | 87 % | 10 % | 43.6 | 46.1 / 6.2 ms |
| H700 whole run, new | 20.0 (19.9-20.2) | 33.9 (33.8-34.1) | 36.3 (36.1-36.3) | 204.0 (boot) | 88 % | 6 % | 45.2 | 43.8 / 5.3 ms |

The whole run is the first 75 s (VI frames 100 to 3440 old / 3540 new on the RK3566, 3260 / 3380 on the H700: the new build gets further in the same time, so the two windows differ by about 3 % at the end); its maximum is the boot frame 137, then frame 2714 (a scene change with no RDP work, 70 ms in both builds). The ten slowest frames of the zoom window are all RDP-bound in both builds (`SYNC_FULL` wait 8-32 ms, workers busy 90-125 ms summed); the slowest new frame is 42.3 ms (RK3566) / 50.1 ms (H700) against 47.8 / 51.9 ms. The camera shot moves from "mostly above 30 fps pacing" to "mostly just below the 33.3 ms limit" on the RK3566 (65 % -> 10 % of the frames above it), and from 99 % to 56 % on the H700; it does not reach 60 fps.

Other games (Super Mario 64, Banjo-Kazooie, Donkey Kong 64; scripted input, 50 s, frames from 1440; 2 runs each on the RK3566, 1 on the H700) are at their 60 fps cap in both builds (59.3-60.0 fps, p50 16.6 ms); the summed worker busy time is lower in the new build (SM64 9.9 -> 9.8 ms, Banjo-Kazooie 16.2 -> 15.8, Donkey Kong 64 19.1 -> 18.3 on the RK3566; 10.0 -> 9.8, 16.8 -> 16.3, 19.7 -> 18.7 on the H700). The upper percentiles of frame time at the cap shift in Donkey Kong 64 and Banjo-Kazooie on the RK3566 (p95 26.2 -> 30.4 and 30.4 -> 31.3 ms in both repeats, mean frame time unchanged at 16.65-16.88 ms); with vsync-locked pacing they follow the phase between the emulation thread and the swap more than the work, and the work (busy time, emulation-thread CPU 12.3 / 12.2 ms) did not rise. Not understood beyond that.

## Specialisation coverage

Share of pixels per stage that ran in a specialised variant (statistics build, 184 captured 60-frame windows of 62 games, three windows of a game summed, mean over the games; the variant lists were made from the same statistics, so "after" is in-sample):

| games | stage | exact keys only | with second tiers and complete lists | games below 90 % |
|---|---|---:|---:|---:|
| 62 | texture | 91.5 % | **100.0 %** | 13 -> 0 |
| 62 | Z compare | 93.5 % | **100.0 %** | 9 -> 0 |
| 62 | combine (exact + shape) | -- | **100.0 %** | 0 |
| 62 | blend / store | 99.8 % / 100.0 % | 99.8 % / 100.0 % | 0 |
| 39 unseen | texture | 89.5 % | **100.0 %** | 10 -> 0 |
| 39 unseen | Z compare | 89.8 % | **99.9 %** | 9 -> 0 |

The worst individual games before the tiers were Earthworm Jim 3D (texture 0.1 %), Hydro Thunder (2.9 %), Toy Story 2 (Z compare 18.5 %), Turok (26.3 %), GoldenEye 007 (texture 27.1 %), Quake (Z 44.5 %), Pilotwings 64 (texture 42.5 %).

**Out of sample** (leave-one-game-out: the lists are rebuilt from the other games and applied to the left-out one):

| stage | estimate |
|---|---|
| texture, exact keys from 23 ROMs | 90 % (min 47 %) |
| texture, exact keys from 62 games | 92.1 % (min 0.1 %) |
| texture, with the "any format" tier | **99.9 %** (min 98.4 %) |
| Z compare, with the complete list | **100 %** for 16-bit colour buffers |
| combine, exact variants only (23 ROMs) | 59 % (min 8 %); for 39 unseen games 71 % |
| combine, with shape kernels | **89.8 %** (median 100 %; 6 of 62 games below 50 %) |

Combiner pixels of the 39 unseen games, by kind of kernel:

| kernel list | exact variant | shape kernel only | generic kernel | games with > 50 % generic |
|---|---:|---:|---:|---:|
| exact only | 71.0 % | -- | 29.0 % | 10 |
| + shape list made from the 23 test ROMs (out of sample) | 71.0 % | 13.5 % | **15.5 %** (median 2.5 %) | 4 |
| + shape list from 62 games (shipped; in-sample) | 71.0 % | 29.0 % | 0.0 % | 0 |

## What `RelaxedPrecision` saves

Measured 2026-10-16 on rgdsplus (4x A55, `performance` governor, effective ~1.58 GHz), foir devel working tree built with `-DFOIR_FRAMELOG` (generic armv8-a -O3), rsp-hle + parallel-rsp as `RspFallback`, rsp-timing shim, `FOIR_THREADS=3`, scripted input (Pokemon Snap with no input), 75 s per run, frames after VI 1440 (about 2000-3000 frames per run). `RelaxedPrecision` is read at launch, so each setting is a separate launch; 10 games x 2 settings x 3 repeats = 60 runs in 20 sessions of 3 runs, the order alternating `True False True` / `False True False`, the lead flipped every other game. The frame-time source is the swap-to-swap time of `FOIR_FRAMELOG` (`tools/framelog.py`); "worker ms/frame" is the summed busy time of the pixel workers per frame, "RDP" is the emulation thread's time in `ProcessRDPList` (parse + setup + waits). Raw files and scripts: the session scratchpad (not in the repo). The in-game option was verified per run from the written config (`RelaxedPrecision = True / False`).

Worker busy ms/frame, mean of 3 runs (half-range of the three run means), and the change of `False` against `True`:

| game | worker True | worker False | delta | run-to-run noise | RDP (emu thread) delta | process CPU delta |
|---|---|---|---|---|---|---|
| Banjo-Kazooie | 15.12 +-0.01 | 15.47 +-0.03 | **+2.3 %** | 0.2 % | +0.5 % | +0.4 % |
| Donkey Kong 64 | 17.92 +-0.05 | 18.53 +-0.08 | **+3.4 %** | 0.4 % | +1.9 % | +0.6 % |
| Ocarina of Time | 7.07 +-0.77 (6.04 7.59 7.58) | 7.89 +-0.01 | +11.6 % (median-based **+4.0 %**) | 10.9 % (one True run was a different scene) | +5.2 % | +0.3 % |
| Mario Kart 64 | 11.29 +-0.06 | 11.78 +-0.04 | **+4.3 %** | 0.6 % | +3.0 % | +0.1 % |
| Paper Mario | 11.91 +-0.21 | 12.31 +-0.07 | **+3.3 %** | 1.7 % | +6.5 % | +2.3 % |
| Pokemon Snap (no input) | 40.02 +-0.10 | 41.16 +-0.12 | **+2.8 %** | 0.3 % | +6.2 % | +2.6 % |
| Star Fox 64 | 7.46 +-0.04 | 7.65 +-0.00 | **+2.5 %** | 0.5 % | +2.2 % | +0.9 % |
| Super Mario 64 | 12.12 +-0.05 | 12.56 +-0.04 | **+3.6 %** | 0.4 % | +6.8 % | +0.9 % |
| Toy Story 2 | 5.49 +-0.02 | 5.56 +-0.00 | +1.1 % | 0.4 % | +1.0 % | +0.3 % |
| Pilotwings 64 | 4.62 +-0.06 (4.70 4.58 4.59) | 5.44 +-1.08 (6.87 4.73 4.72) | median-based **+3.0 %** (means +17.6 %, one False run was an outlier) | 19.8 % (outlier) | +2.5 % | -0.4 % |

Median of the three runs per game and setting: `False` is slower in **10 of 10 games**, by +0.8 % (Toy Story 2) to +4.6 % (Mario Kart 64), mean +3.1 %, median +3.3 %. Excluding the two outlier runs the run-to-run noise is 0.2-1.7 % and every game except Toy Story 2 is clearly outside it (+2.2 to +4.6 %), and the sign agrees in each individual run pair (the slowest True run is faster than the fastest False run in all 10 games, in Toy Story 2 by 0.04 ms and in Pilotwings by 0.02 ms, i.e. barely). In absolute terms that is 0.1-0.6 ms of summed worker time per frame (about 0.04-0.2 ms of wall time per frame at three workers); the emulation thread's RDP time moves by +0.5 to +7 % of a 1-5 ms stage, and the whole process CPU time by 0-2.6 %.

**No effect on frame rate, frame-time percentiles or spikes.** Every game is vsync-locked (59.2-59.9 fps; Pokemon Snap's attract demo 47.9 vs 47.2 fps and Toy Story 2 58.6 are limited by the emulation, not by `RelaxedPrecision`), p50 is identical (16.6 ms), p95 / p99 differ by -0.5 to +1.9 % apart from the noisy Ocarina (+5 / +8 %) and Pilotwings (outlier) runs, and the top-10 spike frames are the same frames at the same VI numbers in both settings (Banjo 1497 / 1526 / 3655, DK64 2137 / 3771, Mario Kart 1892, Toy Story 2 3224, ...); they are scene changes, boot / `SYNC_FULL` waits with no workers busy, and CPU / JIT stalls, dominated by emulation-thread work, not pixel work. The spikes that do carry worker load (DK64, Ocarina, Snap, 40-50 ms) are 40-50 ms with either setting.

**Share of work on the relaxed path** (aarch64 `FOIR_STATS` build under qemu, `foir-replay --fast`, existing dumps; counters added in a scratch copy of `kneon_mem.c`, not in the tree). Pixels of Z-compare batches in the opaque / transparent Z modes ("fast-Z"), and of those the pixels that are only partially covered in memory after the new coverage is added (the only pixels where the delta-Z terms matter) and the 8-pixel groups with at least one such pixel (the groups that need the exact `z_compare4` path when `RelaxedPrecision = False`):

| dump | Z-compare pixels | fast-Z share | partial pixels (of Z) | groups with a partial pixel (of fast-Z groups) |
|---|---|---|---|---|
| Banjo-Kazooie 450 / 1800 / 2400 / 3000 | 236k / 102k / 134k / 104k | 100 % | 1.1 / 3.4 / 3.5 / 5.3 % | 6.6 / 12.9 / 16.7 / 20.0 % |
| Donkey Kong 64 800 / 2400 / 3000 | 50k / 128k / 117k | 100 / 99.5 / 95 % | 17.0 / 7.0 / 7.3 % | 48.8 / 25.9 / 25.2 % |
| Pokemon Snap 300 | 457k | 100 % | 15.3 % | 38.2 % |
| Kirby 64 600 | 117k | 100 % | 5.7 % | 23.6 % |
| Rocket 1500 | 136k | 99.8 % | 9.9 % | 28.4 % |
| Diddy Kong Racing 700 | 66k | 87 % | 3.4 % | 13.8 % |
| Super Mario 64 650 | 42k | 0 % (interpenetrating / decal: not affected) | - | - |
| Paper Mario 900, Star Fox 64 1000, F-Zero X, GoldenEye, Hydro Thunder, Jet Force Gemini | no Z-compare pixels in these dumps | | | |

So 1-17 % of the Z-compare pixels (typically 3-10 %) are partially covered, which is where the exact path differs, and 7-49 % of the 8-pixel groups of the fast-Z modes contain at least one such pixel and fall to the exact path without the relaxation. That fits the measured gain: the relaxation is the difference between a vector compare and the slow path for those groups. (The dumps are one frame each; Mario Kart 64, Ocarina of Time, Toy Story 2 and Pilotwings 64 have no dump. SM64 uses the unaffected modes in this dump yet shows +3.6 % in game, so part of the gain comes from other scenes of the game.)

**Verdict.** The gain is real but small: 2-5 % of the pixel worker time in 9 of 10 games (+0.8 % in Toy Story 2), 0.1-0.6 ms per frame, invisible in fps, frame-time percentiles and the spike frames of these vsync-locked sessions; it would only show up in a scene that is worker-bound beyond the frame budget (Pokemon Snap's heaviest windows, 40 ms of worker time per frame, +1.2 ms). The cost is that the default output is not bit-exact with angrylion for partially covered coplanar-edge pixels. Recommendation: the benefit is outside the noise but too small to matter on the target handhelds in the 10 games measured, so either choice is defensible; the exactness-first reading of the decision rule in FOIR-TODO item 3 ("within noise: make `False` the default") does not apply because the gain is not within noise. If the default is to stay `True`, document it as a ~3 % pixel-time optimisation; if exact-everywhere matters more than ~3 % of worker time, switching the default to `False` costs nothing visible in frame rate. The measurement alone supports keeping `True` for the speed-first project goal (speed over invisible accuracy gaps), with `FOIR_KNOBS=exact` / `RelaxedPrecision = False` as the exact setting.

**Decision (2026-10-16).** The option stays, but its default is now `False` (exact everywhere): ~3 % of pixel-worker time is invisible in fps and tails on both handhelds, and exact-by-default needs no caveat when comparing with angrylion. `True` remains available per game (suxty menu) for worker-bound scenes.

Limits: the generic build carried `-DFOIR_FRAMELOG` in both settings (the overhead is the same in both); the 3-worker pool hides part of the saving in wall time; runs of the same game differ in the scene reached by the scripted input (Ocarina True run 1, Pilotwings False run 1), which is why the medians are quoted; audio was the dummy plugin.

## VI filter pass

`foir-replay --vi-bench 100`, rgdsplus (4x A55, `performance` governor, 1.58 GHz real) and rg35xxsp (4x A53, `cpu_perf.sh on`, 1512 MHz), the glibc 2.31 build of foir-todo, 100 frames per mode, ms per VI frame (rg35xxsp: three runs per dump, which agree within 1 %; rgdsplus: one run of this build, three runs of the first build agreed within 1 %) of the already rendered dump frame (VI conversion only; modes alternate inside each run). The "aa0 + divot + dither restore" row is the usual 0x13016 set of status bits.

| mode (RK3566, A55) | Banjo 292x213 | DKR | Paper Mario | Snap |
|---|---|---|---|---|
| unfiltered conversion | 0.17 | 0.20 | 0.20 | 0.20 |
| filter pass, nothing enabled | 0.41 | 0.47 | 0.47 | 0.47 |
| dither restore | 0.92 | 1.06 | 1.06 | 1.06 |
| divot | 0.61 | 0.69 | 0.69 | 0.69 |
| gamma | 0.60 | 0.68 | 0.68 | 0.68 |
| aa0 + divot + dither restore | 1.20 | 1.60 | 1.39 | 4.11 |
| scalar reference, same | 7.9 | 9.8 | 9.6 | 12.4 |

The H700 (A53) numbers are within 5 % of these in every row except Snap's last (4.6 ms). Partially covered pixels: Banjo 0 %, Paper Mario 0 %, DKR 1 %, Snap 21 % (44 % of the 16-pixel groups). Cost of the first version (3-line reload per output line, 16-bit planes, scalar AA from RDRAM): 1.9-2.8 ms typical and 7.2 ms for Snap; the line cache, 8-bit planes and AA from the converted lines halved it. What it would still take: the restore filter is operation bound (24 compares and 24 adds per 16 pixels and channel; computing each neighbour pair once would save about a third), the scalar AA filter costs about 165 ns per partially covered pixel, and the pass could run on the idle workers (they are drained at the VI scan-out). **Decision (2026-10-16):** `VIDivot` and `VIGamma` default on (0.2 ms each), `VIAntiAliasing` and `VIDedither` default off (`VIFilters` was the first version's single switch): all four cost 1.4-1.9 ms, 7-10 % of a 60 fps frame on the emulation thread, and 4.4-4.8 ms (25-30 %) in an edge-heavy 3D scene, for a quality gain most people notice only on the dither pattern and on Banjo-Kazooie-style jagged logos. Each can be set per game from suxty. Costs per option are in the next table.

**Per option (2026-10-16).** `foir-replay --vi-bench 200`, status with every filter enabled by the game (`aa_mode` 0, divot, dither restore, gamma) and the options set as listed; the glibc 2.17 build of foir-todo on both devices (rgdsplus `performance` 1.58 GHz real, rg35xxsp `cpu_perf.sh on` 1512 MHz), three rounds over the four dumps, medians in ms per VI frame (all spreads within 6 %, mostly 1 %). With no option on the plain conversion is taken (the first row), whatever the game enables; any one filter pays the pass's fixed ~0.35 ms plus its own work.

| options | Banjo A55 | DKR A55 | Paper A55 | Snap A55 | Banjo A53 | DKR A53 | Paper A53 | Snap A53 |
|---|---|---|---|---|---|---|---|---|
| none (plain conversion) | 0.17 | 0.20 | 0.20 | 0.20 | 0.19 | 0.22 | 0.22 | 0.22 |
| AA | 0.51 | 0.79 | 0.58 | 3.33 | 0.53 | 0.84 | 0.59 | 3.82 |
| divot | 0.61 | 0.70 | 0.71 | 0.70 | 0.62 | 0.71 | 0.71 | 0.71 |
| dither restore | 0.92 | 1.07 | 1.08 | 1.07 | 0.90 | 1.04 | 1.04 | 1.04 |
| gamma | 0.60 | 0.69 | 0.69 | 0.69 | 0.62 | 0.71 | 0.71 | 0.71 |
| AA + divot | 0.71 | 1.02 | 0.81 | 3.55 | 0.72 | 1.07 | 0.81 | 4.06 |
| AA + dither restore | 1.02 | 1.38 | 1.18 | 3.94 | 1.00 | 1.40 | 1.14 | 4.37 |
| divot + dither restore | 1.12 | 1.30 | 1.30 | 1.30 | 1.09 | 1.27 | 1.26 | 1.27 |
| AA + divot + dither restore | 1.22 | 1.61 | 1.40 | 4.14 | 1.19 | 1.63 | 1.37 | 4.61 |
| all four | 1.41 | 1.83 | 1.62 | 4.36 | 1.39 | 1.86 | 1.60 | 4.85 |
| scalar reference, all four | 8.07 | 10.03 | 9.83 | 12.57 | 9.42 | 11.64 | 11.50 | 14.28 |

The pass itself costs 0.41-0.47 ms with nothing enabled in the buffer (the earlier measurement above); on top of that AA adds about 0.1 ms in a scene with few partially covered pixels and 2.7-3.4 ms in Snap, divot and gamma about 0.2 ms each, dither restore about 0.5 ms. So in terms of the plain conversion each option alone costs 0.3-0.6 ms (AA; 3.1-3.6 in Snap), 0.5 (divot), 0.5 (gamma) and 0.7-0.85 (dither restore).

## Tried and rejected

Each of these was built and measured with the same method; differences below about 1 % are noise.

- **Run-time code generation (Lightning)**: bounded at about 4 % by the per-batch overhead measurement; see [design.md](design.md#why-no-jit).
- **A `SkipCoverage` knob** (treat every span pixel as covered): saved 0.7-1.5 ms per frame at three workers, bought no frame rate, and produced one-pixel seams along polygon edges (SM64's cap, Pokemon Snap's terrain). Removed.
- **Vector form for colour / Z groups that straddle two scanlines:** +3 % slower than the scalar per-lane path (about 40 more instructions per access; a restricted version had -1.8 % instructions and no change in cycles on the in-order A55). Upper bound if every straddling group were contiguous: -3.3 % (SM64), -6 % (DK64). Padding every run of a batch to a multiple of eight lanes was not tried (dead lanes cost about 400 instructions per pixel against 70 saved per straddling access).
- **MID_TEXEL as a run-time value of the exact texture variants**: costs 6-11 % of the texture stage. **Operand constants as registers** in the shape kernels (`FOIR_SK_SPLIT`): 236 kernels for the same coverage, 1-6 % faster combiner stage. Neither kept.
- **Batch builder:** 16-lane blocks for the ramps (0.7-0.9 % faster, not kept), per-channel run-time branches instead of the 12 instances (+0.7 %), splatting zero-step channels (no gain), eight-lane half blocks, `render_span_cycle` out of line (+0.6-1.3 %), cheaper prefetch variants (equal within noise; switching the prefetch off costs Donkey Kong 64 +3.8 %), walking only the worker's own stripes in the row loop (no difference).
- **Edge walker:** the portable C walker on aarch64 (+1 % on one thread, +2 % with three workers: jumping by multiplication in scalar code costs 190-320 ns a row on the A55), gathering rows into a worker-local span array first (+3.8 %), decoding the 44-word record on the workers (upper bound 2-8 % of the emulation thread's remaining RDP time, in exchange for decoding every primitive three times).
- **LOD:** table lookups of the divide as lane loads (0 to +1 %), keeping the old divide for non-LOD kernels (+1.5 to +5 %), a vector reciprocal in place of the 128 KB divide table (needs 12 table registers), a NEON merge of mixed-tile groups (0.4-7 % of the groups).
- **Helper and pool:** the emulation thread as a fourth static owner (its share would start at the barrier), the helper rendering while the parser parses (delays the critical path), a worker claiming all its classes at once (no stealing window), scanning the job ring instead of per-class queues (stale positions alias after wrap), three or four classes per worker (0.752 / 0.753 against 0.753 on the A53), and `DeferSync` ([design.md](design.md#threading)).
- **A faster delta-Z path:** roughly 4 % of SM64's time. (Early Z, rejected here at first at 3-4 % in the best scenes, came back as the batch skip above, once Pokemon Snap's camera shot showed 58 % of its pixels failing Z.)
- **Z compare:** an extra software prefetch of the next group's colour / Z windows inside `zcmp` (+6 % `zcmp` time; the stage is instruction bound, the batch builder's prefetch already covers the lines); the two-window merge generalised to any run structure and to the partial group at the end of a batch (a window per run, merged lane by lane: +1.4 % `zcmp` time against the per-lane path for the 3 % of groups it serves, dropped); `z_compare8` inlined into the loop (+1.7 % for the RelaxedPrecision loop through register allocation, so it is out of line); skipping the colour-buffer read and the memory-colour / coverage / shift / blend-enable stores of groups with no live lane (-8 % `zcmp`, -1.2 % total on frame 1101 with RelaxedPrecision, RDRAM identical on four dumps, not kept: blend and store never read those fields of a dead group, but `zcmp` would no longer write them, which `foir-kfuzz` and `FOIR_NEON_VERIFY` compare lane by lane).
