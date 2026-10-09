#include "config.h"

void foir_config_defaults(struct foir_config *c)
{
    memset(c, 0, sizeof(*c));
    c->fast_combiner = true;
    c->relaxed_precision = true;
    c->frame_skip = 0;
    c->interlace_hack = true;
    c->threads = 0;
    c->specialize = true;
    c->neon = true;
    c->vsync = true;
    c->filter = false;
    c->widescreen = false;
}
