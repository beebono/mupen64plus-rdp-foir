# test-data

Captured RDP frames of real games for the offline tests. The dumps are derived from commercial ROMs, so they are not
committed; generate them locally. Everything in this directory except this file is ignored by git.

## What uses it

- `tools/neoncheck.py` (and so `make check`) replays every `test-data/dumps/*.fdump` / `*.fdump.xz` with the NEON
  kernels against the scalar reference. With no dumps it runs only the synthetic scenes from `tools/regress.py`, so the
  tests still work on a fresh clone; the dumps add real-game coverage.
- The benchmark tables in `docs/performance.md` ("captured frames") were measured on the set below with `foir-replay`.

## Generating dumps

Any Mupen64Plus front-end works; foir must be the video plugin and an **LLE RSP** must drive it (rsp-hle with
`DisplayListToGraphicsPlugin = False` and `RspFallback` = parallel-rsp, or rsp-cxd4 with
`DisplayListToGraphicsPlugin = False`). Set the capture variables and run the game past the chosen frame:

```sh
FOIR_DUMP=test-data/dumps/Super_Mario_64 FOIR_DUMP_FRAME=650 \
mupen64plus --gfx mupen64plus-rdp-foir.so --rsp mupen64plus-rsp-hle.so \
  --set "rsp-hle[DisplayListToGraphicsPlugin]=False" --set "rsp-hle[AudioListToAudioPlugin]=False" \
  --set "rsp-hle[RspFallback]=/path/to/mupen64plus-rsp-parallel.so" \
  --set "Core[RandomizeInterrupt]=False" "Super Mario 64.z64"
xz -9 test-data/dumps/Super_Mario_64_650.fdump
```

- `FOIR_DUMP=<prefix>` enables capture and writes `<prefix>_<frame>.fdump` (RDRAM snapshot + the RDP commands).
- `FOIR_DUMP_FRAME=<n>`: first VI frame to capture (default 200). `FOIR_DUMP_FRAMES=<k>`: number of frames
  (default 2). Long captures replay against the RDRAM snapshot taken at the first frame, which is fine for state /
  coverage statistics but not for pixel-exact images later in the window.
- `Core[RandomizeInterrupt]=False` makes runs repeatable, so the same frame number captures the same scene.
- Name dumps `<Game>_<frame>.fdump.xz`; neoncheck uses the file name as the scene name.
- On a headless desktop, `SDL_VIDEODRIVER=offscreen` works for the x86_64 plugin (scalar kernels; the dump format is
  the same). Run desktop emulators in a memory-capped scope, e.g.
  `systemd-run --user --scope -p MemoryMax=2G -p MemorySwapMax=0 ...` — the legacy x86_64 dynarec of older cores can
  run away on some ROMs.

## The set used for the published numbers

Two-frame dumps (`FOIR_DUMP_FRAMES` default) at these VI frames, most captured with no controller input (attract /
intro sequences):

| file | game | frame |
|---|---|---|
| `Banjo-Kazooie_450.fdump.xz` | Banjo-Kazooie | 450 |
| `Diddy_Kong_Racing_700.fdump.xz` | Diddy Kong Racing | 700 |
| `Donkey_Kong_64_800.fdump.xz` | Donkey Kong 64 | 800 |
| `F-Zero_X_700.fdump.xz` | F-Zero X | 700 |
| `GoldenEye_007_900.fdump.xz` | GoldenEye 007 | 900 |
| `Hydro_Thunder_1500.fdump.xz` | Hydro Thunder | 1500 |
| `Jet_Force_Gemini_500.fdump.xz` | Jet Force Gemini | 500 |
| `Kirby_64_-_The_Crystal_Shards_600.fdump.xz` | Kirby 64: The Crystal Shards | 600 |
| `Paper_Mario_900.fdump.xz` | Paper Mario | 900 |
| `Pokemon_Snap_300.fdump.xz` | Pokemon Snap | 300 |
| `Star_Fox_64_1000.fdump.xz` | Star Fox 64 | 1000 |
| `Super_Mario_64_650.fdump.xz` | Super Mario 64 | 650 |

A regenerated frame need not be byte-identical to the one the numbers came from (core version, RSP and interrupt timing
all shift the scene a little), but the NEON-vs-scalar checks are exact on whatever is captured.
