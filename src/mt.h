/* Band-split worker pool (see mt.c). */
#ifndef FOIR_MT_H
#define FOIR_MT_H

#include "rdp.h"

/* rows are owned by worker (y >> FOIR_STRIPE_SHIFT) % nworkers */
#define FOIR_STRIPE_SHIFT 2

/* Create the pool on the parser context; `threads` is the total number of rendering threads including
 * the parser thread (<= 1 disables the pool). Returns true when a pool is active. */
bool mt_start(struct rdp *r, int threads);
void mt_stop(struct rdp *r);
/* Queue a primitive (r->pc and the walker record; the workers compute the spans of their rows). */
void mt_submit(struct rdp *r, const struct prim_walk *pw, int start, int end);
/* Wait until every queued primitive has been rendered. */
void mt_sync(struct rdp *r);
/* Wait if RDRAM bytes [lo, hi) may still be written by queued primitives. */
uint32_t mt_pixels(struct rdp *r);
/* pixels that ran through a specialised variant per stage, all pixels, and the pixels that needed the texture stage */
void mt_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all);
#ifdef FOIR_FRAMELOG
/* switch on the busy / sync timing; read the cumulative counters */
void mt_prof_enable(struct rdp *r);
int mt_prof_read(struct rdp *r, uint64_t busy[8], uint64_t *jobs, uint64_t *sync_ns);
#endif
void mt_sync_range(struct rdp *r, uint32_t lo, uint32_t hi);

#endif
