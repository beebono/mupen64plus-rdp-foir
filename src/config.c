#include "config.h"

void foir_config_defaults(struct foir_config *c)
{
    memset(c, 0, sizeof(*c));
    c->fast_combiner = true;
    c->relaxed_precision = false;
    c->frame_skip = 0;
    c->skip_min_prims = 100;
    c->interlace_hack = true;
    c->threads = 0;
    c->mt_helper = true;
    c->specialize = true;
    c->neon = true;
    c->early_z = true;
    c->vsync = true;
    c->filter = false;
    c->widescreen = false;
    c->vi_filters = false;
    c->crop_h = false;
    c->crop_v = false;
}
