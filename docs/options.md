# Options

foir reads its settings from the `[Video-Foir]` section of `mupen64plus.cfg` (written with the defaults below the first time the plugin starts), plus the window size from `[Video-General]`. A front-end can set any of them per game with `--set Video-Foir[Key]=value`. The `FOIR_*` environment variables override or extend them for testing and benchmarking.

## `[Video-Foir]`

| Key | Default | Effect |
|---|---|---|
| `Widescreen` | `False` | Stretch the image to the whole window instead of keeping 4:3. |
| `Filter` | `False` | Bilinear filtering when scaling the VI image to the window (off = nearest neighbour). |
| `VSync` | `True` | Wait for vertical sync when presenting. |
| `Threads` | `0` | Rendering worker threads. `0` = auto (cores - 1: 3 on a 4-core chip; at most 7; one on a 2-core chip); `-1` = render on the emulation thread; `1`..`7` = that many. |
| `FrameSkip` | `0` | `0` = off; `N` = render one frame in `N+1`; `auto` = skip when the achieved frame interval exceeds the VI period (see below). Skipped frames are still parsed (state, TMEM loads, syncs) but not rendered. |
| `InterlaceHack` | `True` | Show only the freshest field of field-rendered interlaced frames, line doubled, instead of weaving it with the previous field. |
| `SyncHelper` | `True` | The emulation thread renders pending stripe classes while it waits for the workers (`SYNC_FULL`, VI scan-out, hazard barriers, full job ring). Exact; `False` = it idles. |
| `RelaxedPrecision` | `True` | Depth compare of the opaque / transparent Z modes without the delta-Z terms for partially covered pixels; affects only coplanar edge cases. **Not exact.** |
| `FastCombiner` | `True` | Use the NEON combiner and blender (exact). Off: the scalar reference combiner and blender. |
| `NEON` | `True` | Use the NEON kernels when built in. `False` = scalar reference kernels (slow). |
| `Specialize` | `True` | Use the precompiled per-state pixel kernel variants; `False` = the generic NEON kernels only (bit-identical output, slower). Needs `NEON`. |

`[Video-General]` holds the usual `Fullscreen` (default `False`), `ScreenWidth` (640) and `ScreenHeight` (480); foir registers them if they are not there yet.

**Exact settings** (the output matches the scalar reference, and through it angrylion-rdp-plus): `RelaxedPrecision = False`, `FrameSkip = 0`, `InterlaceHack = False`; all other options are exact in either position. `FOIR_KNOBS=exact` forces these three.

**FrameSkip.** A fixed `N` renders the frames whose count is a multiple of `N+1`. In `auto` mode the achieved frame interval is measured over windows of 60 presented frames: above 112 % of the VI period (16.67 ms) the skip level goes up, below 92 % it goes down; level 1 renders every second frame, level 2 one frame in three. It is a simple controller and does not know about render-to-texture frames. The suxty menu offers Off / Every 2nd / 2 of 3 / 3 of 4 / Auto (levels 0-3 and `auto`; the names count skipped frames).

Removed options: a `JIT` entry that an older config file may still contain is ignored (the compile-at-run-time pipeline it was reserved for was dropped in favour of the static variants); a `SkipCoverage` knob existed for a while and was removed because it produced seams along polygon edges.

## Environment variables

Always available:

| Variable | Effect |
|---|---|
| `FOIR_THREADS=n` | Overrides `Threads`. |
| `FOIR_NEON=0\|1` | Overrides `NEON`. |
| `FOIR_HELPER=0\|1` | Overrides `SyncHelper`. |
| `FOIR_MT_CLASSES=n` | Stripe classes per worker (default 2 with the helper, 1 without). |
| `FOIR_SPEC=0` | Same as `Specialize = False`. |
| `FOIR_SPEC_COMB=auto\|exact\|shape\|generic` | Combiner dispatch: exact variant, then shape kernel, then generic kernel (`auto`), or only some of them. |
| `FOIR_SPEC_TEX=auto\|exact\|shape\|generic` | Texture dispatch: exact variant, then "any format" kernel, then generic kernel (`shape` = "any format" kernels only). |
| `FOIR_KNOBS=exact` | `RelaxedPrecision`, `FrameSkip` and `InterlaceHack` at their accuracy-preserving setting. |
| `FOIR_NEON_MASK=bits` | Which stages use NEON, one bit each: texture 0x01, combine 0x02, zcmp 0x04, blend 0x08, store 0x10, fill 0x20, copy 0x40 (default `0x7f`). A stage that is not NEON is not specialised either; `0x3f` = everything but the NEON COPY kernel. |
| `FOIR_NEON_VERIFY=1` | Run every NEON stage next to the scalar kernel and count mismatches; use with `FOIR_KNOBS=exact` ([testing.md](testing.md#in-plugin-verifier)). |
| `FOIR_MT_STATS=1` | Print worker-pool counters (jobs, syncs, ring waits, helper tasks) when the plugin shuts down. |
| `FOIR_LOGFILE=path` | Also append the plugin's log lines to this file (unbuffered). |
| `FOIR_DUMP=prefix`, `FOIR_DUMP_FRAME=n`, `FOIR_DUMP_FRAMES=k` | Capture RDP frames for `foir-replay` (first frame, default 200; number of frames, default 2). |
| `FOIR_SHOT_DIR=dir`, `FOIR_SHOT_FRAMES=a,b,...` | Write `frame_NNNNN.ppm` screenshots at these plugin frame numbers (up to 64). |

Only in debug builds (`CFLAGS=-DFOIR_STATS` or `-DFOIR_FRAMELOG`, see [testing.md](testing.md#debug-builds-and-logs)):

| Variable | Effect |
|---|---|
| `FOIR_STATE_STATS=1`, `FOIR_STATE_STATS_ALL=1`, `FOIR_STATE_STATS_FILE=path` | Per-state and per-stage-key pixel statistics (all keys; to a file rewritten every 120 frames). |
| `FOIR_NEON_STATS` | NEON kernel and fallback counters at exit. |
| `FOIR_FRAMELOG=path`, `FOIR_FRAMELOG_MAX=n` | Per-frame binary record (default cap 36,000 frames). |

Compile-time options that change the build rather than a run: `-DFOIR_BATCH=n` (pixels per batch, default 64), `-DFOIR_SHAPE_DEF=\"file\"` (an alternative shape-kernel list), `-DFOIR_SK_SPLIT=1` (shape kernels keyed on which operands are constants), `-DFOIR_STAGETIME` (per-stage timers).

## Log output

Every 120 frames the plugin logs the wall time per frame, the RDP time (parse + setup, of which the wait at `SYNC_FULL`), VI convert and present time, pixels, primitives, skipped frames, process and emulation-thread CPU per frame, and the share of pixels that ran in specialised variants per stage (`specialised pixels: ...`). At start it logs the kernel set, the worker count and the number of variants per stage.

## suxty interface

foir exports one extra function, `FoirSuxtyGetInterface()` (declared in `src/foir_suxty.h`, listed in `src/foir_api_export.ver`), so that the in-game menu of mupen64plus-input-suxty can list and change foir's options. It returns a pointer to a static, `const`, versioned table (`abi`, `size`, option count, option descriptors, `get`, `set`, `save_default`). It is one-way: foir never calls suxty, nothing in the plugin API changes, and a foir that nobody asks for behaves exactly as before. suxty finds the symbol by walking the loaded objects (front-ends load plugins `RTLD_LOCAL`, under whatever file name the user chose) and checks `abi` (currently 1).

| id | key (`[Video-Foir]`) | type | applies |
|---|---|---|---|
| 0 | `Widescreen` | bool | at once |
| 1 | `Filter` | bool | at once |
| 2 | `InterlaceHack` | bool | at once |
| 3 | `FrameSkip` | enum: Off / Every 2nd / 2 of 3 / 3 of 4 / Auto | at once |
| 4 | `VSync` | bool | next launch |
| 5 | `Threads` | enum: None, Auto, 1..7 (index = `Threads` + 1) | next launch |
| 6 | `NEON` | bool | next launch |
| 7 | `Specialize` | bool | next launch |
| 8 | `FastCombiner` | bool | next launch |
| 9 | `RelaxedPrecision` | bool | next launch |

*Why these are live.* `Widescreen` and `Filter` are read when the frame is drawn, `InterlaceHack` by the VI conversion and `FrameSkip` by the `SYNC_FULL` hook, all on the emulation thread and none in the workers' private copy of the config. *Why the others are not.* `Threads`, `NEON`, `Specialize` and `FastCombiner` choose the kernel tables and the worker pool at start; `RelaxedPrecision` is part of the specialisation key and is copied into every worker; `VSync` is applied when the window is created. Changing them mid-game would mean rebuilding the pipeline.

*Thread safety.* `get()` / `set()` must be called on the emulation thread, not from inside a foir call. suxty's menu runs inside the buffer swap, which `UpdateScreen()` issues last, after the workers have been drained and the frame uploaded: nothing else touches the live options while the menu is open, and the next `ProcessRDPList()` starts after the menu returns. Once the ROM is open a call from another thread is refused.

*What `set()` does.* It never writes `mupen64plus.cfg`. Before the ROM is opened it overrides what was read from `[Video-Foir]` for the coming launch (suxty hands over its stored per-game values from `InitiateControllers`, which runs before foir opens the ROM; this is how the "next launch" options get applied without a restart). After the ROM is open a live option takes effect and a next-launch option is only remembered. Environment overrides (`FOIR_NEON`, `FOIR_KNOBS=exact`) still win. `save_default()` writes one value into `[Video-Foir]` and saves that section; suxty only calls it when the user picks "Save as foir default".
