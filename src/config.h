#ifndef FOIR_CONFIG_H
#define FOIR_CONFIG_H

#include "foir.h"

enum { FOIR_FRAMESKIP_AUTO = -1 };

struct foir_config {
    bool fast_combiner;
    bool relaxed_precision;
    int  frame_skip;
    bool interlace_hack;
    int  threads;            /* 0 = auto */
    bool mt_helper;
    bool specialize;
    bool neon;
    bool early_z;            /* batch-skip early Z (NEON kernels); FOIR_EARLYZ overrides */
    bool debug_nopixels;
    int  skip_min_prims;     /* FOIR_SKIP_MINPRIMS: a VI period with fewer primitives into scanned-out buffers is not worth skipping (the next one is rendered) */
    int  skip_opts;          /* FOIR_SKIP_OPTS bits, to bisect FrameSkip artefacts: 1 no refill, 2 no heavy-frame rule, 4 skip off-screen targets too, 8 skip fills and copies too */
    bool skip_log;           /* FOIR_SKIPLOG: log each frame start (FrameSkip) and each VI scan-out */
    bool skip_legacy;        /* FOIR_SKIP_LEGACY: FrameSkip drops every primitive, as before the off-screen / fill / copy rules (comparisons) */

    bool vsync;
    bool filter;
    bool widescreen;
    bool vi_aa;              /* VIAntiAliasing, VIDivot, VIDedither, VIGamma: allow that VI filter when the game enables it in VI_STATUS (live) */
    bool vi_divot;
    bool vi_dedither;
    bool vi_gamma;
    bool crop_h;             /* CropOverscanH: VI_CROP_H pixels off the left and right edges (live) */
    bool crop_v;             /* CropOverscanV: VI_CROP_V lines off the top and bottom (live) */
};

void foir_config_defaults(struct foir_config *c);

#endif
