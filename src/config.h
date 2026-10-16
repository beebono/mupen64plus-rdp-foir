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

    bool vsync;
    bool filter;
    bool widescreen;
    bool vi_filters;         /* VIFilters: the VI filter pass (AA, divot, dither restore, gamma), live */
    bool crop_h;             /* CropOverscanH: VI_CROP_H pixels off the left and right edges (live) */
    bool crop_v;             /* CropOverscanV: VI_CROP_V lines off the top and bottom (live) */
};

void foir_config_defaults(struct foir_config *c);

#endif
