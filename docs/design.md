# Design

How foir is built: where it sits in Mupen64Plus, the render pipeline, the NEON kernels and their per-state specialisation, the worker pool, and the VI output. Measurements are in [performance.md](performance.md), the tests in [testing.md](testing.md), all options in [options.md](options.md).

## Goals and non-goals

- **Full speed on Cortex-A53 / A55 class SoCs** (Allwinner H700 / A133, Rockchip RK3326 / RK3566 and similar 4-core handheld chips).
- **LLE rendering.** foir interprets real RDP commands rather than high-level display lists, so it needs no per-microcode HLE hacks.
- **NEON first.** The hot paths are hand-written NEON kernels; a scalar C11 path exists as the exact reference and for portability.
- **Configurable accuracy.** Every shortcut is a toggle, so a game that breaks can get the exact behaviour back.
- Not a goal: cycle-exact output (angrylion-rdp-plus is the accuracy reference foir is tested against, not a target it tries to match), a GPU rendering path, upscaling or enhancement features.

## How it fits into Mupen64Plus

Mupen64Plus has no separate "RDP plugin" type. foir is a **video plugin** (`M64PLUGIN_GFX`) that does its real work in `ProcessRDPList()` (`m64p_plugin.h` of the core). An **LLE RSP** plugin calls that function whenever the RSP hands the RDP a command list; foir reads the list directly from RDRAM or DMEM, so no conversion layer is needed.

| RSP setup | Notes |
|---|---|
| **rsp-hle + parallel-rsp as `RspFallback`** (recommended) | rsp-hle runs audio lists in HLE (fast); with `DisplayListToGraphicsPlugin = False` it sends graphics tasks to the fallback, which runs them in LLE with parallel-rsp's Lightning JIT. Fastest of the three. |
| **parallel-rsp as the RSP plugin** | Same graphics path; audio runs in LLE too (a little slower than HLE audio, bit exact). |
| **rsp-cxd4** (`DisplayListToGraphicsPlugin = False`) | Works, but it is an interpreter whose vector unit is SIMD only on x86. With the RDP on worker threads the RSP and CPU emulation, not the RDP, set the frame time in heavy scenes. |

The plugin must be the parallel-rsp **Mupen64Plus plugin target** (`mupen64plus-rsp-parallel.so`, built with `PARALLEL_INTEGRATION`); the stock parallel-rsp library lacks the SP DMA and RDP list handling a plugin needs. The aarch64 fork with that target and a NEON-inlined vector unit is at github.com/beebono/parallel-rsp (branch `m64p-aarch64`). In a comparison of screenshots at fixed VI frames (23 ROMs, deterministic interrupts) the three setups render bit-identical frames, and parallel-rsp's audio output is sample-identical to cxd4's; rsp-hle's HLE audio differs from LLE by a few LSB to a few hundred, as expected.

## Pipeline

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
   each renders the rows of the stripe classes it owns (its own FB / Z / hidden-bit rows)
                  |
                  v
   per row: edge walker (4 subscanlines, NEON) -> span -> pixel batches (64 px, struct-of-arrays, several scanlines)
        texture -> combine -> zcmp -> blend -> store      (NEON kernels, 8 px per step; zcmp first in states that allow it, see Early Z)
```

The **parser** (`parser.c`) decodes commands, keeps the render state and TMEM (`tmem.c`) and derives the pipeline flags. **Primitive setup** (`setup.c`) only *decodes* a triangle / rectangle: the 44-word edge record becomes the per-primitive constants and a 192-byte **walker record** (`struct prim_walk`: start, step and middle-vertex switch of the three edges, the scissor, the interpolants at the first row with their per-row steps, the interlace field). The record travels in the job through a lock-free ring; render state (about 1.1 KB) and TMEM (4 KB) are snapshotted only when something changed since the previous job.

### Edge walker on the workers

The RDP walks a primitive in four subscanlines per row, and everything it carries from one subscanline to the next is linear in the subscanline number (edge positions, interpolants; the left edge restarts from the lower edge at the middle vertex). The per-row state (min / max edge, the all-over / all-under / all-invalid flags) is reset every row. The span of row `j` is therefore a function of `j` and the walker record: the edge position of subscanline `k` is `start + (k - k0) * step`, the interpolants of row `j` are `a0 + (j - j0) * step`, in wrapping 32-bit arithmetic, which is exactly what `k - k0` repeated additions give (unsigned add and multiply are ring operations mod 2^32).

Each worker calls `walk_row()` (`walk.h`) for the rows it renders: the four subscanlines sit in the four lanes of NEON vectors (a portable `walk_row_c` serves the scalar build), the span is handed to the batch builder and forgotten. No span records are written, queued or read by another core. This replaced a sequential walker on the emulation thread that wrote one 96-byte span record per scanline (60 % of the emulation thread's own RDP work, 2.8-10 % of foir's CPU time in game). `foir-walkfuzz` checks the per-row walker against that sequential walker, kept verbatim in `tools/walk_ref.inc`.

### Batch builder

Pixels of one primitive are collected into **batches** of up to 64 lanes in struct-of-arrays form (`struct px_batch`, `rdp.h`), possibly spanning several consecutive scanlines of the primitive (`raster.c`). Two parts of the NEON builder are worth knowing:

- **Coverage from the edge zones.** Subscanline *i* covers the pixels strictly between its edges completely and the two edge pixels with a partial mask. The span runs from the smallest valid left edge to the largest valid right edge, so every pixel right of all left edges and left of all right edges has one and the same mask; the interior needs no coverage computation (four constant vector stores), and only the pixels of the two edge zones (normally one or two each) are evaluated and patched in, the four subscanlines in the four lanes of a vector.
- **Fixed-size ramps.** The interpolant ramps of all channels advance together in one loop over groups of four lanes with whole-vector stores (the lane arrays carry 16 lanes of slack); the channel set (shade, alpha, Z, texture) is a compile-time constant of each of 12 instances.

The portable builder (`render_span_ref`, with `compute_cvg`) is kept as the reference; `foir-kfuzz -m 64` compares the two.

### Pixel stages

The batch runs through the stage kernels of `struct foir_kernels`: `texture`, `combine`, `zcmp`, `blend`, `store` (plus `fill` and `copy` for the FILL / COPY cycle types). The table is the swap-in point: the scalar set (`kernels_scalar.c`, `kernels_tex.c`) is the reference, the NEON set (`kneon_*.c`) replaces entries one by one and falls back to the scalar kernel *per batch* for configurations it does not handle. Every stage works in 16-bit lanes, 8 pixels per step (32-bit only where a product needs the headroom).

| Stage | NEON handles | Scalar fallback for |
|---|---|---|
| texture | every non-YUV format (RGBA 4/8/16/32, IA, I, CI), TLUT, point / bilinear / mid-texel, perspective divide, shift / clamp / mask / mirror, LOD, detail and sharpen, 1- and 2-cycle, next-pixel TEXEL1, the BI_LERP = 0 colour conversion on point samples | YUV tiles, the conversion arithmetic on filtered or TLUT texels |
| combine | 1- and 2-cycle, all inputs, dither (matrix / random), noise, shade coverage correction, alpha / coverage selects | chroma key |
| zcmp | 16- and 32-bit colour buffers, every Z mode, Z decode / encode and delta-Z comparator computed (no lookup tables), opaque / transparent fast path | 4- / 8-bit colour buffers |
| blend | both cycles, all selectors, the exact division table, alpha compare (also dithered), RGB dither | -- |
| store | 16- and 32-bit colour buffers, Z update, coverage / hidden bits | 4- / 8-bit colour buffers |
| fill | 16- and 32-bit rows of 16+ pixels | other sizes |
| copy | texture rectangles into 16-bit colour buffers | 8- / 4-bit colour buffers, triangles drawn in COPY mode, YUV texels without TLUT, spans that start left of x = 0, are 1024 pixels long or lie within 16 pixels of the end of RDRAM |

**Early Z.** The Z compare reads the batch's coverage, interpolated Z, delta-Z and the Z / colour buffers, and writes `wen`, `blend_en`, `prewrap`, `sz`, `mem`, `memcvg` and the blender shifts. Texture and combine write only texel, colour, alpha and dither arrays and read no buffer. So the two groups of stages are independent, and `zcmp` can run first, unless one of them rewrites the coverage the other reads: the combiner does so with `CVG_TIMES_ALPHA`, and the Z compare does in the interpenetrating mode (`om_early_z_safe()` in `rdp.h`: `z_compare_en && !cvg_times_alpha && z_mode != INTERPENETRATING`; derived into `pf.early_z`). Alpha compare, coverage-to-alpha selects and `force_blend` do not matter: they only clear `wen` in the blender, after the Z compare. In those states `batch_flush` runs `zcmp`, and a batch in which no lane is write-enabled (every lane failed Z) skips texture, combine, blend and store; a batch with a live lane runs the remaining stages exactly as before, on the same inputs. Blend and store already ignored lanes with `wen = 0`, and batches stay in order, so the memory every `zcmp` reads is unchanged. Without a Z compare (`wen` is always 1) the original order stays. In Pokemon Snap's camera shot 58 % of the processed pixels fail Z. The scalar kernels are the reference and keep the original order, so `neoncheck` and `mtfuzz` compare the two orders; `FOIR_EARLYZ` selects it ([options.md](options.md)), and `kfuzz` checks the rule on random states ([testing.md](testing.md)). What it does not do: skip dead 8-lane groups inside texture and combine (the texture kernel reuses the neighbouring lane's perspective divide and LOD, so it would have to recompute that lane's inputs; modelled at 1-2 points more on the heavy frames), or reject before the batch is built (needs a Z shadow that the CPU can invalidate behind foir's back).

Details that keep the vector code exact: combiner inputs A, B, D use the RDP's quirky 9-bit extension (`0x100..0x17f` stay positive), input C is a true signed 9-bit value; equations are classified per batch (`D`, `A*C`, `(A-B)*C`, `A*C+D`, full) so zero operands cost nothing; the 16-bit framebuffer is read and written with halfword-pair reversal (RDRAM is held as host words) and per-pixel "hidden" bytes for coverage / delta-Z. The pseudo-random numbers behind noise, random dither and dithered alpha compare are a hash of the framebuffer index and a per-primitive seed, so the result does not depend on batching or on which thread renders a pixel (the hardware LFSR sequence is not reproduced, and neither is angrylion's, which depends on pixel order).

**Z compare.** `zcmp` reads the colour buffer (memory colour, coverage, hidden bits) and the Z buffer of a group first, then does the arithmetic. The memory Z (14-bit exponent / mantissa code *e*, *m*) decodes to `oz = 0x40000 - ((base - m) << max(6 - e, 0))` with `base` 0x1000 (0x800 for *e* = 7), so with `Q = 0x40000 - oz` computed in 16-bit lanes the depth test `sz < oz` is `sz + Q < 0x40000` and "farthest depth" (`oz == 0x3ffff`) is `zpx >= 0xfffc`: no 32-bit decode, no table. Opaque / transparent compares take this path when every lane's coverage overflows (delta-Z only matters for partially covered memory pixels) or under RelaxedPrecision, which drops the delta-Z terms for the other lanes. The rest (exact mode with a partially covered lane, decal mode) runs `z_compare8`: the delta-Z of the memory pixel and the comparator's highest bit in 16-bit lanes (eight at once), the three depth comparisons on `sz + Q`; only the interpenetrating mode, which also rewrites the coverage, stays on the 4-lane `z_compare4`. The synthesised depth `sz` is a 19-bit field saturated by `min(s, 0x3ffff)` below 0x60000 and 0 above.

**COPY-cycle rectangles.** Backgrounds, sprites and, in some games, whole screens are drawn with COPY-cycle texture rectangles (no combiner, blender or Z). The scalar kernel fetches one pixel at a time through the generic TMEM readback (about 50 ns per pixel on the A55). The NEON kernel works in two phases: it computes the texture-coordinate start of every group of four pixels, then walks the span in windows of eight pixels aligned to an even framebuffer pixel, expands the group values to the lanes with a table lookup, does the address arithmetic in 16-bit lanes (only the low 11-12 bits of a TMEM address matter), gathers texels (CI8 / CI4 / 16-bit indices through the TLUT, RGBA16 directly, other formats through the texture decoder) and stores the window with a lane mask. A window that is not inside its row is written per pixel, because the neighbouring rows may belong to another worker.

**Colour / Z buffer access.** `zcmp` and `store` read and write the colour buffer, the Z buffer and the hidden bytes in groups of eight lanes. A group of consecutive pixel indices is one vector window. In `zcmp` a group that is the end of one scanline and the start of the next (exactly two runs of consecutive pixels; 22 % of the groups of Pokemon Snap's zoom shot, 9-31 % in the gameplay windows) is read as two windows merged by a lane mask (the first lane that breaks the run is found from the index vector, and the group must be two runs, else it takes the per-lane path); every other group, and every group in `store`, takes the per-lane scalar path. In gameplay windows 54-91 % of the groups are contiguous and 9-31 % straddle two scanlines of the batch. The access functions are inlined into the kernels, the bounds check against RDRAM is made once per batch (the batch builder keeps the lowest / highest pixel index) and the colour / Z / hidden lines of every run are prefetched at batch-build time. A first vector form for the straddling groups was slower on the in-order A55 (see [performance.md](performance.md#tried-and-rejected)); the `zcmp` form above is smaller (one index test, two loads per buffer, one merge) and faster ([Z compare](performance.md#z-compare)).

## Texture unit and TMEM

`tmem.c` models TMEM with word-wise fast paths for `LOAD_TILE` / `LOAD_BLOCK` / `LOAD_TLUT`. Mip-mapping (the LOD path) is NEON throughout: `lod_pass()` computes the level of detail of a whole batch in one call (the neighbouring lane's perspective divide is reused when that lane really holds `s + dsinc`, `t + dtinc`, `w + dwinc`; the tail of the computation runs on eight 16-bit lanes), and the texture cycle takes a one-tile fast path when all eight lanes select the same tile (93-99.6 % of the groups in the games measured); mixed groups take an out-of-line multi-tile pass. The specialised LOD variants require one texel format along the tile chain `tilenum .. tilenum + max_level` (one more with detail), checked per batch; otherwise the batch runs in the generic kernel.

## Specialised pixel kernels

The generic NEON kernels read the render state at run time (combiner operand tables, equation shapes, texel-format and blender switches), a handful of branches per 8-pixel group. Games use few distinct states, so every pixel stage also has **precompiled variants** with that state folded into the code at build time. Nothing is generated at run time: a JIT was evaluated and dropped (see [Why no JIT](#why-no-jit)).

- **Stage keys.** Whenever the render state changes (or a primitive samples a different tile) `foir_spec_select()` (`spec.c`) derives a key per stage:

  | stage | key | generic kernel only for |
  |---|---|---|
  | texture | cycles, which texels the combiner reads, format and size of the tile (and tile + 1), TLUT and its type, point / bilinear / mid-texel, perspective, BI_LERP, LOD. `TEX` / `TEXL`: all of that; `TEXA` / `TEXAL` ("any format"): everything but the formats and sizes of the tiles, the TLUT type and MID_TEXEL, which are read from the state | YUV, BI_LERP = 0 on filtered or TLUT texels |
  | combine | cycle count, the 16 `SET_COMBINE` selectors, shade needed, texel range. `COMB`: also dither, alpha-compare and coverage options; `COMBX`: those read from the state | chroma key |
  | zcmp | colour buffer size / format, image read, Z compare and mode, blender reads memory alpha, RelaxedPrecision (anti-aliasing and force blend are read from the state; every combination of the rest exists for a 16-bit colour buffer) | 32-bit and 4 / 8-bit colour buffers |
  | blend | cycle count, the eight blender selectors. `BLEND`: also alpha compare, dither, coverage, force blend; `BLENDX`: those from the state | -- |
  | store | colour buffer size / format, coverage destination, Z update | 4 / 8-bit colour buffers |

  The tile's coordinate parameters (shift, clamp, mask, mirror), colours, constants and all per-pixel data stay run-time values in every variant.
- **Variants.** A variant is the kernel *body* instantiated with a literal state: `texture_body()`, `comb_body()`, `blend_body()`, `zcmp_body()`, `store_body()` are `always_inline` functions that take the state as a struct. The generic kernel is the same body with the struct filled from the render state; a variant (`kspec_*.inc`) passes literals and the compiler folds operand selection, equation classes, format switches and option branches away. The variants are listed in `spec_*.def` (one line each) and compiled in ahead of time: 164 texture, 110 combine, 42 zcmp, 28 blend, 8 store, plus 147 combiner shape kernels, 499 table entries in all (the plugin's code is about 2.1 MB).
- **Dispatch.** The stages are independent. For each, an exact key match wins; then an "any option" variant with the same selectors (combine, blend), the "any format" kernel of the same structure (texture) or a shape kernel (combine); a stage without a variant keeps its generic NEON kernel, which handles every state. `r->ktab` holds the choice (a hash lookup per stage and state change, one flag test per primitive). Each worker has its own table, so selection needs no synchronisation.
- **Exactness** is tested per variant, not only on sample frames: `foir-kfuzz -S` writes each variant's key over a random render state and compares the chain with the scalar one ([testing.md](testing.md)).
- **Coverage** is the share of pixels that ran through a variant, per stage. The plugin logs it every 120 frames (`specialised pixels: ...`), `foir-replay` prints it per tier, and the statistics build lists the keys that miss.

### Combiner shape kernels

Exact combiner variants are keyed on the full `SET_COMBINE` words and generalise poorly: on 39 games outside the 23 the lists were first made from, 71 % of combiner pixels ran in a variant. What does generalise is the *shape* of the equations `(A - B) * C + D`. A **shape kernel** (`kspec_shape.inc`, list in `spec_shape.def`) compiles in only that structure and takes its operands from a per-draw table.

- **Key** (`struct shape_key`): cycle count and, per cycle, the shape of the RGB and the alpha equation. Every input is one of four letters: `0`, `1` (constant 1.0), `C` (COMBINED, cycle 1 only) or `V` (a variable operand: texel, shade, LOD fraction, noise, PRIM / ENV / K4 / K5, combined alpha). Equations that do not depend on A, B and C are the canonical "D only". One more bit, `ext`, says a variable operand can leave 0..255 (wide texels, a sharpened LOD fraction in a multiply input) and needs sign extension on load. Dither selects, alpha compare, coverage options and shade / noise needs are read from the state at run time.
- **Operand table** (`struct sk_tab`): for every `V` input and channel, `foir_spec_select()` resolves a 16-bit byte offset into the `px_batch`. Per-pixel sources and the kernel's scratch arrays are fields of the batch; the constants (PRIM, ENV, KEY, K4, K5, PRIM LOD fraction) are splatted once per state into `b->kc[]`, already in the 9-bit extended form of their position. Reading a variable operand is the same instruction sequence whatever it is.
- **Kernel set:** the 100 shape combinations seen in the combiner statistics of 62 games, 8 `ext` forms, and the 39 one-cycle (RGB, alpha) pairs that follow from the one-cycle halves seen: 147 kernels, 298 KB (the 110 exact variants are 275 KB). Compiling every valid shape is impossible with fused kernels: a half equation has 84 (RGB) / 56 (alpha) valid shapes in cycle 1 and a kernel is the product of four halves (about two million two-cycle combinations).
- **Dispatch:** exact variant, then shape kernel, then generic kernel. A shape kernel costs 1.15-1.65x an exact variant (median 1.3x), the generic kernel 2.2-3.1x a shape kernel, so the exact variants stay first.
- `FOIR_SPEC_COMB=auto|exact|shape|generic` restricts the choice; `FOIR_SK_SPLIT` (a build option) keys the kernels on which operands are constants and loads those as registers: it needs 236 kernels for the same coverage and was only 1-6 % faster in the combiner stage, so the uniform path ships.

### Texture and Z compare: the second tier

The weak spots were games whose texture or Z-compare *key* was not in the lists, usually a combination of independent bits that none of the first 23 ROMs used (MID_TEXEL on, two tiles with different formats, AA + force blend with an opaque Z compare, interpenetrating mode without image read). Two measures:

- **"Any format" texture kernels** (`TEXA` / `TEXAL`, 38 of them: 30 structures seen in 62 games plus 8 structural LOD forms): the tile formats and sizes, TLUT type and MID_TEXEL are read from the tiles and the state; the rest is compiled in. Dispatch is exact variant, then "any format" kernel, then generic (`FOIR_SPEC_TEX` restricts it). They cost 11-23 % more texture-stage time than an exact variant, against 39-73 % for the generic kernel. Leave-one-game-out coverage of textured pixels: 99.9 % (worst game 98.4 %), against 91.7 % for exact keys only. MID_TEXEL stays a literal in the exact keys; making it a run-time value cost 6-11 % of the texture stage.
- **Complete Z-compare list:** ANTIALIAS_EN and FORCE_BLEND only choose the blend-enable lane mask at the end of the kernel, so they left the key. What remains (image read x colour buffer type x {no compare, four Z modes} x memory-alpha shifts x RelaxedPrecision) is small enough to compile in completely: 42 variants for 16-bit colour buffers (`tools/specmatrix.py zcmp`).

### Adding variants

```sh
# 1. a statistics build; run games with FOIR_STATE_STATS=1 FOIR_STATE_STATS_ALL=1 FOIR_STATE_STATS_FILE=log.txt
#    (the plugin rewrites the file every 120 frames), or foir-replay on dumps with 2> log.txt
CFLAGS=-DFOIR_STATS make all replay CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 POSTFIX=-stats
# 2. rank the keys over any number of logs (every log counts equally) and print the lines to add
python3 tools/specstats.py log1.txt log2.txt ...                    # per stage: ranked keys, '*' = covered
python3 tools/specstats.py --def --stage comb log*.txt >> src/spec_comb.def
python3 tools/specstats.py --def --stage tex --shapes log*.txt      # exact TEX / TEXL lines and TEXA / TEXAL kernels
python3 tools/specmatrix.py zcmp > src/spec_zcmp.def                # the complete Z-compare list
python3 tools/shapegen.py --one-cycle-product logs... > src/spec_shape.def
python3 tools/specstats.py --loo 40 log*.txt                        # leave-one-log-out coverage estimate
# 3. rebuild; `make check` fuzzes every variant (kfuzz -S) against the scalar reference
```

### Why no JIT

An earlier plan was to compile pipeline variants at run time with GNU Lightning. Measured on the RK3566 before building it: only 2-7 distinct pipeline states occur per frame (the 24 most common cover 92 % of all pixels); the generic NEON kernels already classify the fast shapes; and rebuilding with batch sizes of 32 / 64 / 128 / 256 pixels showed all per-batch overhead together is about 4 % of the run time, the upper bound for what scalar glue code from a JIT could remove. The NEON bodies a JIT would chain are the same ones that run today. Static macro-specialised variants got much more than the estimated 10-25 % (1.25-1.65x on one thread), because a variant also removes the per-batch table set-up, the operand-descriptor loads, the shade / cycle-0 round trips through memory and the format switches. After the shape kernels a copy-and-patch JIT for the combiner alone would be worth about 2-3 % of CPU time at the cost of W^X handling and a second path to keep bit exact; it could only pay as a way to *fuse stages*, which nothing here addresses. foir no longer depends on Lightning.

## Threading

- **Stripe classes.** Rows are cut into 4-row stripes and stripe `s` belongs to class `s % K` (`K` = 2 x workers with the emulation-thread helper, else `K` = workers). A class is the unit of ordering: for each job the parser appends the job index to the queue of every class the job has rows in (a 32-bit mask in the job), and the tasks of a class, one per job, are applied strictly in job order, one at a time. A class has one state word (tasks completed << 1 | busy) that is its lock: a thread claims the next task of an idle class with a compare-and-swap, renders the rows of that class, and publishes the next position with a release store, which also hands the pixels to whoever claims the class next. Home workers take the oldest pending task of their own classes (`c % workers == id`, the same rows as the plain `(y >> 2) % N == id` partition). **Order is exact by construction**: every pixel sees its primitives in primitive order whichever thread renders them, and threads never wait for each other.
- **No sharing in the hot loop.** A worker is a private `struct rdp` context whose render-state block is refreshed from the snapshot of whatever job it renders. Framebuffer, Z and hidden-bit rows are private to the class; no lock is taken per pixel or per primitive. Pixels past the end of a row are dropped instead of wrapping into the next row, and vector stores that would rewrite pixels outside their row fall back to per-lane stores.
- **Barriers.** Workers are drained only when the parser needs a consistent RDRAM: `SYNC_FULL`, VI scan-out, `ReadScreen2` / `FBRead` / `FBWrite`, a texture load whose source overlaps memory queued primitives still write, a change of render-target geometry that would break row ownership of memory still being written, and shutdown. A job-ring slot, a state snapshot or a TMEM snapshot is overwritten only when every job below the floor (`mt_floor()`, the oldest job still pending or running in any class) is complete.
- **The emulation thread as a helper (`SyncHelper`, default on).** At every barrier, and when a ring is full, the emulation thread claims idle classes of any worker and renders them (`help_one()`) instead of idling; it backs off only when every pending class is busy on a worker. It never renders while it parses, and no worker ever waits for the helper (a class it holds just has no claimable task). This is motivated by the in-game hitch analysis on the H700: 260 of 433 frames longer than 36 ms were the emulation thread waiting at `SYNC_FULL` with its core idle while three workers were saturated. Two classes per worker beat one, three or four. Idle workers spin for 400 us, then sleep on a condition variable.
- `Threads = 0` picks `cores - 1` workers (3 on a 4-core chip, at most 7); `-1` renders on the emulation thread (the single-threaded behaviour, useful for debugging).
- **Not waiting at all** (`DeferSync`, tried and removed): raising the DP interrupt at `SYNC_FULL` without waiting for the workers, as paraLLEl-RDP does for its GPU work. The frame rate did not move (the wait reappeared in the VI drain, because a game swaps the buffer in the DP interrupt handler and the VI shows it at the next retrace), and it was not exact: nothing tells the plugin that the CPU or RSP does not read the frame or Z buffer behind the interrupt (the core's frame-buffer hooks are off under the dynarec), and Ocarina of Time frames differed from the default build.

## VI output

- foir emulates no VI filters. It crops the active framebuffer region and converts it to RGBA8888 with NEON (about 0.2 ms for 320x237), uploads it as **one texture** through the core's video extension (GLES2; about 0.7 ms including draw and swap on the RK3566), and scales it to the window (`Filter` selects bilinear, `Widescreen` stretches instead of keeping 4:3).
- `InterlaceHack`: for interlaced output of frames the game rendered one field at a time (`SET_SCISSOR` field bits), only the freshest field is scanned out, line doubled, instead of weaving it with the previous field.
- A front-end or plugin that overrides VidExt, such as mupen64plus-input-suxty, can draw its overlay on top of that texture.

## Source layout

```
src/plugin.c          Mupen64Plus exports, [Video-Foir] config, frame skipping, UpdateScreen / ReadScreen2, the suxty interface
src/foir_suxty.h      the suxty interface table (ABI 1), shared with mupen64plus-input-suxty
src/parser.c          DP command list reader, command dispatch, state-setting commands, derived pipeline flags
src/setup.c           primitive setup: triangle / texrect / fillrect -> 44-word record -> per-primitive constants + walker record
src/walk.h            the edge walker for one row from the walker record (walk_row: NEON, walk_row_c: portable)
src/raster.c          row loop, span -> pixel-batch builder (NEON and portable reference), worker row ownership
src/mt.c              worker pool: job ring, per-class queues and claiming, the helper, snapshots, hazards and barriers
src/tmem.c            TMEM model, LOAD_TILE / BLOCK / TLUT, texel readback
src/kernels_scalar.c  scalar reference kernels (combiner, Z, blender, FB store, FILL, COPY)
src/kernels_tex.c     scalar texture kernel (perspective, LOD, filtering, TLUT, YUV)
src/kernels_neon.c    NEON kernel table, FOIR_NEON_MASK / FOIR_NEON_VERIFY wrappers
src/kneon_*.c         NEON texture (and COPY), combiner, blender, Z compare / buffer access / store / FILL
src/spec.h, spec.c    stage keys, variant lookup / selection per render state
src/kspec_*.inc       the specialised variants, instantiated from the lists in src/spec_*.def
src/spec_*.def        the variant lists (generated by tools/specstats.py, specmatrix.py, shapegen.py)
src/vi.c, gl_out.c    VI crop + conversion to one RGBA texture; drawing it through the core's VidExt
src/capture.c         frame capture for offline replay (FOIR_DUMP) and screenshots (FOIR_SHOT_*)
src/stats.c           debug statistics (-DFOIR_STATS); src/framelog.c per-frame record (-DFOIR_FRAMELOG)
tools/                foir-replay, kfuzz, walkfuzz, ref-angrylion, neoncheck.py, mtfuzz.py, regress.py, rdpgen.py, ...
test-data/            captured frames for the tests (not in the repository, see test-data/README.md)
```

## Known gaps

- **VI.** No VI filters (anti-aliasing, divot, gamma, dither) and no 480i deinterlacing beyond `InterlaceHack`. `FBRead` / `FBWrite` only drain the workers: no framebuffer emulation for CPU effects (the core's frame-buffer hooks are off under the dynarec).
- **Exactness.**
  - `RelaxedPrecision` (default off since 2026-10-16) is not exact when turned on. `FastCombiner` and everything else are; with exact settings the output is identical to the scalar reference.
  - One-cycle perspective LOD with a max level above 0 is not angrylion-exact in the far part of the plane (angrylion's `tclod_1cycle_current` has span-edge cases the scalar kernel does not model; the NEON path follows the scalar reference). Detail and sharpen occur in no captured frame; they are covered by the fuzzers and synthetic scenes only.
  - In the captured Kirby 64 frame compared with angrylion, 25 pixels differ (inherited from the scalar reference). Some hidden-bit and COPY-mode details are still approximations.
  - The pseudo-random noise is not the hardware sequence.
- **Scalar fallbacks.** NEON falls back to the scalar kernels for YUV tiles, chroma key and 4 / 8-bit colour buffers. COPY-cycle rectangles are NEON only for 16-bit colour buffers and texture rectangles; 8-bit colour buffers, COPY triangles and YUV texels without TLUT are scalar (none of them occurs in the 62 captured games).
- **Specialisation.** The variant lists come from the first minute or so of the 23 test ROMs plus the statistics of 62 games' captured windows (intros, menus, demos, early levels), not from full playthroughs; a state without a variant runs the generic NEON kernel of that stage. Coverage tables in [performance.md](performance.md) are in-sample; the out-of-sample figures are the leave-one-game-out estimates there (texture 99.9 %, Z compare 100 %, combiner 89.8 %).
  - The combiner shape kernels leave about 10 % of an unseen game's combiner pixels in the generic kernel (6 of 62 games below 50 %), mostly two-cycle states with a rare combination of equation shapes.
  - The `ext` form of a shape kernel exists only for the shapes seen with a LOD-fraction multiply input (none for wide texels such as YUV, BI_LERP = 0). The sharpen modes in the dumps (Pilotwings 64, SpaceStation Silicon Valley) were not checked for whether their combiners read the LOD fraction.
  - No variants for 32-bit colour buffers (the Z compare list is complete for 16-bit colour buffers only), YUV, chroma key and FILL (those paths are unchanged). A point-sampled two-cycle LOD state has no variant. The texture variants assume that all tiles a mip chain can select share one texel format (checked per batch, else generic). The "any format" kernels cost 11-23 % more texture-stage time than an exact variant.
  - Groups of 8 pixels that straddle two scanlines (10-30 % of the groups, 45 % in Paper Mario) take the scalar per-pixel path in the colour / Z buffer access; a vector form was measured and does not pay on the Cortex-A55.
- **Verification scope.** Frames were compared for the first minute of each ROM, not full playthroughs. The row-ownership rule of the NEON windows (an odd-aligned or COPY window must not leave its row) is tested for equal output only, because ThreadSanitizer cannot run the NEON kernels on x86_64. Flip-0 COPY spans and 8-bit colour buffers are exercised only by the fuzzers, where both sides take the scalar kernel.
- **Platforms.** The parallel-rsp pairing was verified on aarch64 devices (RK3566, H700). The x86_64 build (scalar kernels) is for development and testing and is slow.
- `FrameSkip=auto` is a simple window-based controller and does not know about render-to-texture frames.

## Ideas not done

- Variants for 32-bit colour buffers, YUV, chroma key; NEON COPY for 8-bit colour buffers and COPY triangles; contiguous-texel fast paths for the COPY kernel.
- The batch builder is still about 500 instructions per span, mostly the 45 vector stores of a piece, the per-piece prefetch and the edge-zone pixels.
- Skipping texture / combiner work for lanes the Z test already rejected: 5-15 % of the Z-tested groups in the 3D games are dead, and an early Z pass costs about as much as it saves below roughly 10 %.
- Fusing stages to keep the 8-pixel working set in registers.
- VI filters; a faster delta-Z path for interpenetrating states (about 4 % of Super Mario 64's time).
