/* foir configuration (the [Video-Foir] section). See README "Speed / accuracy knobs". */
#ifndef FOIR_CONFIG_H
#define FOIR_CONFIG_H

#include "foir.h"

enum { FOIR_FRAMESKIP_AUTO = -1 };

struct foir_config {
    /* speed/accuracy knobs; all default to "enabled" per the README. M1 stores
     * them but only the reference (accurate) paths exist, so most are no-ops. */
    bool fast_combiner;      /* FastCombiner */
    bool relaxed_precision;  /* RelaxedPrecision */
    int  frame_skip;         /* FrameSkip: 0 = off, N = fixed, -1 = auto */
    bool interlace_hack;     /* InterlaceHack */
    int  threads;            /* Threads (0 = auto) */
    bool mt_helper;          /* SyncHelper: the emulation thread renders for the workers while it waits at a barrier */
    bool specialize;         /* Specialize: use the precompiled per-state kernel variants (M3) */
    bool neon;               /* kernel selection: use NEON kernels when compiled in */
    bool debug_nopixels;     /* parse and set up primitives but skip the pixel pipeline (profiling) */

    /* output */
    bool vsync;              /* VSync */
    bool filter;             /* Filter: bilinear upscaling of the VI texture */
    bool widescreen;         /* Widescreen: stretch to the whole window instead of 4:3 */
};

void foir_config_defaults(struct foir_config *c);

#endif
