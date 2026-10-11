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
};

void foir_config_defaults(struct foir_config *c);

#endif
