# Options

foir reads its settings from the `[Video-Foir]` section of `mupen64plus.cfg` (written with the defaults below the first time the plugin starts), plus the window size from `[Video-General]`. A front-end can set any of them per game with `--set Video-Foir[Key]=value`. The `FOIR_*` environment variables override or extend them for testing and benchmarking.

## `[Video-Foir]`

| Key | Default | Effect |
|---|---|---|
| `Widescreen` | `False` | Stretch the image to the whole window instead of keeping 4:3. |
| `Filter` | `False` | Bilinear filtering when scaling the VI image to the window (off = nearest neighbour). |
| `VIFilters` | `False` | Apply the VI filters the game enables in `VI_STATUS`: anti-aliasing from the coverage bits, divot, dither restore and gamma (details and exactness in [design.md](design.md#vi-filter-pass)). Costs 1.2-1.6 ms per frame on the handhelds (about 4 ms in a scene with many partially covered pixels), so it is off by default. Applies at once. |
| `CropOverscanH` | `False` | Crop 8 VI pixels off both the left and the right edge of the image (16 each for a hi-res image, 512 or more pixels wide) before it is scaled. Hides the edge garbage some games leave in the outermost columns, which a TV's overscan would hide. Display only. Applies at once. |
| `CropOverscanV` | `False` | Crop 12 lines off both the top and the bottom edge (24 each for a frame of 360 or more lines). Same purpose; display only. Applies at once. |
| `VSync` | `True` | Wait for vertical sync when presenting. |
| `Threads` | `0` | Rendering worker threads. `0` = auto (cores - 1: 3 on a 4-core chip; at most 7; one on a 2-core chip); `-1` = render on the emulation thread; `1`..`7` = that many. |
| `FrameSkip` | `0` | `0` = off; `N` = render one frame in `N+1`; `auto` = skip when the achieved frame interval exceeds the VI period (see below). Skipped frames are still parsed (state, TMEM loads, syncs) but not rendered. |
| `InterlaceHack` | `True` | Show only the freshest field of field-rendered interlaced frames, line doubled, instead of weaving it with the previous field. |
| `SyncHelper` | `True` | The emulation thread renders pending stripe classes while it waits for the workers (`SYNC_FULL`, VI scan-out, hazard barriers, full job ring). Exact; `False` = it idles. |
| `RelaxedPrecision` | `False` | `True` = depth compare of the opaque / transparent Z modes without the delta-Z terms for partially covered pixels; affects only coplanar edge cases and saves about 3 % of pixel-worker time ([measurement](performance.md#what-relaxedprecision-saves)). **Not exact when on.** |
| `FastCombiner` | `True` | Use the NEON combiner and blender (exact). Off: the scalar reference combiner and blender. |
| `NEON` | `True` | Use the NEON kernels when built in. `False` = scalar reference kernels (slow). |
| `Specialize` | `True` | Use the precompiled per-state pixel kernel variants; `False` = the generic NEON kernels only (bit-identical output, slower). Needs `NEON`. |

`[Video-General]` holds the usual `Fullscreen` (default `False`), `ScreenWidth` (640) and `ScreenHeight` (480); foir registers them if they are not there yet.

**Exact settings** (the output matches the scalar reference, and through it angrylion-rdp-plus): `RelaxedPrecision = False` (the default), `FrameSkip = 0` (the default), `InterlaceHack = False`; all other options are exact in either position. `FOIR_KNOBS=exact` forces these three (so only `InterlaceHack` differs from the defaults).

**Overscan crop.** Both bools take a fixed number of *VI pixels* (pixels of the framebuffer image, not screen pixels) off each edge: `CropOverscanH` 8 columns left and right, `CropOverscanV` 12 lines top and bottom (a 320x237 frame becomes 304x213). The cropped image fills the same 4:3 box (or the window with `Widescreen`), so it is scaled up by 5 % / 11 %, in whole VI pixels: nearest-neighbour stays even. A hi-res image (width of 512 or more) crops 16 columns and a frame of 360 or more lines 24, so the same part of the picture goes; an axis whose image would fall below 32 pixels is left alone. Only the conversion of the VI image changes: the RDRAM, framebuffer and `foir-replay` checksums do not, and the screenshot (`ReadScreen2`) shows the cropped image. Under `InterlaceHack` the crop is even, so line pairs and fields stay together. Why these values: in the playtest captures the edge artefacts are in the framebuffer itself, not in the VI registers (`H_START`/`V_START` are the standard 108-748 / 37-511 in the games checked): Jet Force Gemini and Pilotwings 64 carry a garbage last column (1-2 px), Pilotwings and Mario Golf draw black borders of 7-10 px of their own, and hardware hides the outer edge behind the TV's overscan. 8 columns clear the garbage and most of the games' own borders, 12 lines match the 5 % of a typical NTSC overscan; both stay off by default because a game that fills its frame loses real picture. Per game, suxty's `[foir]` section can turn them on.

**FrameSkip.** `N` renders one frame in `N+1`; in `auto` mode the achieved frame interval is measured over windows of 60 presented frames: above 112 % of the VI period (16.67 ms) the skip level goes up, below 92 % it goes down; level 1 renders every second frame, level 2 one frame in three. The rules that decide what a skipped frame drops (the playtest artefacts of Mario Golf, Paper Mario and Star Fox 64 came from breaking them; [design.md](design.md#frame-skip)):

- A *frame* is a VI period: everything drawn into scanned-out buffers between two scan-outs. (Counting `SYNC_FULL`s let a game that syncs twice per frame, or renders in two passes, lock the skip pattern onto one of them: Paper Mario's screen frames were skipped every time.)
- Only triangles and texture rectangles into a **scanned-out** colour image are dropped. Off-screen targets (a colour image the VI never scans out) are always drawn, because a later frame may texture from them; fill and copy rectangles are always drawn (cheap, and the only thing that repaints a border or a backdrop).
- A period is skipped only if the previous one drew at least 100 primitives into scanned-out buffers (`FOIR_SKIP_MINPRIMS`; a light frame saves nothing, and 2D / menu frames are the ones that draw on top of what is already in the buffer) and was not rendered in fields (interlaced `SET_SCISSOR` field bit).
- A buffer whose last frame was skipped is refilled from the last frame that was rendered before the first drawing into it in a rendered frame, so a game that draws on top of the buffer's old content (a pause menu over a frozen scene) does not keep the hole.
- The buffer list is 16 colour images (it was 4: a game with three scan buffers and three off-screen targets thrashed it and showed buffers that still held a pre-skip frame). When the VI scans out a colour image whose last drawing was skipped, foir neither converts nor presents it, so the screen keeps the last rendered frame (`held` in the log).

Known limit: a game that reads its own picture with the CPU or RSP (a snapshot of the screen for a transition effect; Mario Golf's course intro keeps a 160 KB copy at 0x2e8000) sees a buffer that lacks the skipped frames, and foir cannot see CPU reads (the core's frame-buffer hooks are off under the dynarec). Those scenes show wrong frames while skipping lasts, as before. The suxty menu offers Off / Every 2nd / 2 of 3 / 3 of 4 / Auto (levels 0-3 and `auto`; the names count skipped frames).

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
| `FOIR_FRAMESKIP=N\|auto` | Overrides `FrameSkip` (tests). |
| `FOIR_SKIPLOG=1` | Log one line per VI scan-out (shown or held, the colour images drawn since the last one with their primitives, drawn and skipped) and one per frame start (heavy, skip or render). |
| `FOIR_FRAMEHASH=1` | Log a hash of every presented frame and whether it was held (for comparing a FrameSkip run with an unskipped one: `tools/skiptest.py` does it offline, the in-game method is in [testing.md](testing.md)). |
| `FOIR_SKIP_MINPRIMS=n` | The primitive count a period must have drawn before the next one may be skipped (default 100). |
| `FOIR_SKIP_LEGACY=1` | FrameSkip as it was before the rules above (every primitive dropped, decided at every `SYNC_FULL`, four-entry history), to compare. |
| `FOIR_SKIP_OPTS=bits` | Bisecting FrameSkip artefacts: 1 no refill, 2 no heavy-frame rule, 4 also skip off-screen targets, 8 also skip fills and copies, 32 do all the bookkeeping but skip nothing. With `FOIR_SKIPLOG`, `FOIR_RDRAM_BLOCKS=1` adds a hash of every 32 KB of RDRAM to each scan-out line. |
| `FOIR_KNOBS=exact` | `RelaxedPrecision`, `FrameSkip` and `InterlaceHack` at their accuracy-preserving setting. |
| `FOIR_EARLYZ=0\|1\|2` | Early Z ([design.md](design.md#pixel-stages)): `0` = off (original stage order), `1` = on for the NEON kernels (default; the scalar reference always keeps the original order), `2` = on for the scalar kernels too (testing). Exact in every position; for A/B timing and bisecting. |
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

Every 120 frames the plugin logs the wall time per frame, the RDP time (parse + setup, of which the wait at `SYNC_FULL`), VI convert and present time, pixels, primitives, skipped frames, frames held back because they showed a skipped buffer, process and emulation-thread CPU per frame, and the share of pixels that ran in specialised variants per stage (`specialised pixels: ...`). At start it logs the kernel set, the worker count and the number of variants per stage.

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
| 10 | `CropOverscanH` | bool | at once |
| 11 | `CropOverscanV` | bool | at once |
| 12 | `VIFilters` | bool | at once |

*Why these are live.* `Widescreen` and `Filter` are read when the frame is drawn, `CropOverscanH`, `CropOverscanV` and `VIFilters` by the VI conversion (they are copied into the RDP's config at once, like `InterlaceHack`), `InterlaceHack` by the VI conversion and `FrameSkip` by the `SYNC_FULL` hook, all on the emulation thread and none in the workers' private copy of the config. *Why the others are not.* `Threads`, `NEON`, `Specialize` and `FastCombiner` choose the kernel tables and the worker pool at start; `RelaxedPrecision` is part of the specialisation key and is copied into every worker; `VSync` is applied when the window is created. Changing them mid-game would mean rebuilding the pipeline.

*Thread safety.* `get()` / `set()` must be called on the emulation thread, not from inside a foir call. suxty's menu runs inside the buffer swap, which `UpdateScreen()` issues last, after the workers have been drained and the frame uploaded: nothing else touches the live options while the menu is open, and the next `ProcessRDPList()` starts after the menu returns. Once the ROM is open a call from another thread is refused.

*What `set()` does.* It never writes `mupen64plus.cfg`. Before the ROM is opened it overrides what was read from `[Video-Foir]` for the coming launch (suxty hands over its stored per-game values from `InitiateControllers`, which runs before foir opens the ROM; this is how the "next launch" options get applied without a restart). After the ROM is open a live option takes effect and a next-launch option is only remembered. Environment overrides (`FOIR_NEON`, `FOIR_KNOBS=exact`) still win. `save_default()` writes one value into `[Video-Foir]` and saves that section; suxty only calls it when the user picks "Save as foir default".
