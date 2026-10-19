# mupen64plus-rdp-foir

A low-level (LLE) **software RDP video plugin** for Mupen64Plus, built for aarch64 retro handhelds (Cortex-A53 / A55 class SoCs such as the
Allwinner H700 and the Rockchip RK3566). foir renders the N64's RDP command stream on the CPU with hand-written NEON kernels and a pool of
worker threads, with no GPU rendering path, no per-game graphics hacks and no run-time code generation. It is a video plugin
(`mupen64plus-rdp-foir.so`) that is driven by an LLE RSP.

**Accuracy.** [angrylion-rdp-plus] is foir's accuracy oracle. foir's scalar C reference renderer follows the hardware arithmetic that
angrylion documents (bit widths, rounding, lookup tables, the subscanline edge walker and coverage rules), reorganised as batch kernels, and
is compared with angrylion pixel by pixel on synthetic scenes and captured game frames. The NEON kernels, the per-state specialised variants and
the threaded paths are compared bit for bit with that scalar reference. At exact settings ([docs/options.md](docs/options.md#video-foir)) all
three agree on everything tested, except the cases listed under Status. foir does not reproduce angrylion's VI stage.

## Highlights

- **NEON kernels** for every pixel stage (texture, colour combiner, Z compare, blender, store, fill, COPY), eight pixels per step, with a scalar
  fallback per batch for what they do not handle.
- **Per-state specialised variants**: 499 precompiled kernel variants (exact states, combiner "shape" kernels, "any format" texture kernels, a complete
  Z-compare set) selected whenever the render state changes; the generic NEON kernels remain the fallback for any state.
- **Multithreaded, order-exact worker pool.** Workers render 4-row stripes; the emulation thread renders too while it waits at barriers
  (`SyncHelper`), and the edge walker runs on the workers.
- **Speed/accuracy toggles** per game; widescreen, smoothing, VSync and frame skip.

In offline replay on the RK3566, NEON alone is 2.6-4.3x and NEON with three workers 6.1-12.6x faster than the scalar reference renderer, and the
specialised kernels add another 1.2-1.65x on top. In game (rsp-hle + parallel-rsp) Super Mario 64, Donkey Kong 64, Banjo-Kazooie,
Paper Mario and Ocarina of Time hold 59-60 fps on both the RK3566 and the H700 RG35XX SP, and Pokemon Snap, the heaviest game measured, runs at 44-47 fps.
Numbers, method and caveats are in [docs/performance.md](docs/performance.md).

## Using it

foir needs an **LLE RSP** that hands graphics tasks to the video plugin, so that the plugin sees RDP command lists. The recommended pairing is
`mupen64plus-rsp-hle` with the **parallel-rsp** Mupen64Plus plugin (`mupen64plus-rsp-parallel.so`, from github.com/beebono/parallel-rsp, branch
`m64p-aarch64`) as its `RspFallback`: audio lists run in fast HLE, graphics lists in LLE through parallel-rsp's JIT. Set
`DisplayListToGraphicsPlugin = False` so that rsp-hle hands graphics to the fallback:

```sh
mupen64plus --gfx mupen64plus-rdp-foir.so --rsp mupen64plus-rsp-hle.so --audio mupen64plus-audio-sdl.so \
  --set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False \
  --set rsp-hle[RspFallback]=/path/to/mupen64plus-rsp-parallel.so  game.z64
# or parallel-rsp alone as the RSP:  --rsp /path/to/mupen64plus-rsp-parallel.so
# or rsp-cxd4 with rsp-cxd4[DisplayListToGraphicsPlugin]=False (works, but slower)
```

The same settings in `mupen64plus.cfg`, together with foir's own section:

```ini
[Rsp-HLE]
DisplayListToGraphicsPlugin = False
AudioListToAudioPlugin = False
RspFallback = "/usr/lib/mupen64plus/mupen64plus-rsp-parallel.so"

[Video-Foir]
Threads = 0          ; 0 = auto (cores - 1), -1 = render on the emulation thread
```

Main `[Video-Foir]` options (all of them, the environment variables and the suxty interface are in [docs/options.md](docs/options.md)):

| Option | Default | Effect |
|---|---|---|
| `Widescreen` | `False` | Stretch to the whole window instead of 4:3 |
| `Filter` | `False` | Bilinear filtering when scaling the image to the window |
| `VSync` | `True` | Wait for vertical sync when presenting |
| `Threads` | `0` | Worker threads: `0` = auto (cores - 1), `-1` = none (render on the emulation thread), `1`..`7` |
| `FrameSkip` | `0` | `0` = off, `N` = render one frame in `N+1`, `auto` = skip to hold full speed |
| `VIDivot`, `VIGamma` | `True` | Apply that VI filter when the game enables it (anti-aliasing from the coverage bits, divot, dither restore, gamma); 0.2-0.5 ms per frame each; filtered 16-bit images use the same full-range colour as unfiltered ones (white 255, unlike angrylion's 248) |
| `VIAntiAliasing`, `VIDedither` | `False` | Same for the VI's anti-aliasing (0.3-0.6 ms, about 3.5 ms in an edge-heavy scene) and dither restore (0.7-0.85 ms); all four together 1.4-1.9 ms |
| `CropOverscanH`, `CropOverscanV` | `False` | Crop 8 columns (left and right) / 12 lines (top and bottom) of VI pixels off the image, to hide edge artefacts; display only |
| `InterlaceHack` | `True` | Show only the freshest field of field-rendered interlaced frames |
| `RelaxedPrecision` | `False` | `True` = faster Z compare for opaque/transparent modes (about 3 % of pixel-worker time); not exact (see Status) |
| `SyncHelper`, `FastCombiner`, `NEON`, `Specialize` | `True` | Speed paths; each is exact, `False` falls back to slower code |

**In-game menu.** With mupen64plus-input-suxty as the input plugin, its menu gets a *FOIR Options* page: Widescreen, Smoothing (`Filter`), Interlace
Hack, Frame Skip and the two overscan crops apply at once; VSync, Worker Threads, NEON, Specialised Kernels, Fast Combiner and Relaxed Precision apply at the next launch, and
can be stored per game.

## Status, accuracy and known gaps

Pre-alpha. Verified on the RK3566 (ROCKNIX) and the H700 RG35XX SP (spruce) with 23 ROMs that ran for about a minute each without a crash or hang, and
offline on captured frames of 62 games. Known gaps (full list in [docs/design.md](docs/design.md#known-gaps)):

- **`RelaxedPrecision` (off by default) is not exact when turned on**: it drops the delta-Z terms of the opaque/transparent depth compare for partially covered
  pixels, which affects only coplanar edge cases, and saves about 3 % of pixel-worker time (0.1-0.6 ms/frame). The default is exact.
- **VI filters**: `VIDivot` and `VIGamma` are on by default, `VIAntiAliasing` and `VIDedither` opt-in (all four: 1.4-1.9 ms per frame on the handhelds, about 4.4-4.8 ms in a scene full of edges): anti-aliasing, divot, dither restore and gamma
  as the game enables them and the option allows, bit-exact with angrylion at the source pixels (not the VI's own resampling, not gamma-dither noise); otherwise the VI stage crops, converts and
  scales the framebuffer, with `InterlaceHack` for field-rendered interlaced frames. `FBRead` / `FBWrite` only drain the workers, so there is no framebuffer emulation for CPU effects.
- **One-cycle perspective LOD with a max level above 0** is not angrylion-exact in the far part of a plane (the scalar reference differs, and the NEON path follows it).
- **Kirby 64**: in the captured frame compared with angrylion, 25 pixels differ (inherited from the scalar reference).
- Noise and random dither do not follow the hardware sequence. YUV tiles, chroma key and 4/8-bit colour buffers take the (exact, slow) scalar kernels.
- The specialised variants were chosen from games' first minutes; a rare render state runs in a generic NEON kernel at lower speed.

## Documentation

- [docs/design.md](docs/design.md): architecture, parser and edge walker, batch builder, NEON kernels and specialisation, worker pool, VI output, known gaps.
- [docs/performance.md](docs/performance.md): measured results on the RK3566 and the H700 and what was tried and rejected.
- [docs/testing.md](docs/testing.md): the bit-exactness tests and tools, capture and replay, the in-plugin verifier, debug builds.
- [docs/options.md](docs/options.md): every option, environment variable and the suxty interface.

## Building

```sh
cd projects/unix
make all replay ref NEON=0     # x86_64: scalar plugin, offline replay tool, angrylion reference runner (ref needs an angrylion-rdp-plus checkout: `upstream/angrylion-rdp-plus` of the workspace by default, or `ANGRYDIR=<its src/core>`)
make all replay fuzz CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64      # aarch64 cross build with NEON, products in _out_AARCH/
make check CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu"   # bit-exactness suite
```

A native aarch64 build is just `make all`. It needs a C11 compiler, `pthread` and the Mupen64Plus core API headers (found next to the checkout,
in `/usr/local/include/mupen64plus`, or via `APIDIR=`). Builds use the compiler's generic armv8-a target. Debug builds add statistics and a
per-frame log: `CFLAGS=-DFOIR_STATS make ... POSTFIX=-stats` or `CFLAGS=-DFOIR_FRAMELOG make ...` (`CFLAGS` as an environment variable;
see [docs/testing.md](docs/testing.md#debug-builds-and-logs)).

## License

**GPLv3 or later** (like the sibling plugins; foir no longer needs GNU Lightning, whose LGPLv3+ was the original reason for GPLv3+); full text in `LICENSE`. Exception: `src/foir_suxty.h`, the interface header shared with the GPLv2+ suxty plugin, is GPL-2.0-or-later.

[angrylion-rdp-plus]: https://github.com/ata4/angrylion-rdp-plus
