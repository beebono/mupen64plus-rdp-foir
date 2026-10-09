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
/* Queue a primitive. `rows` was obtained from mt_alloc_rows(). */
void mt_submit(struct rdp *r, struct span *rows, int jlo, int start, int end);
/* Wait until every queued primitive has been rendered. */
void mt_sync(struct rdp *r);
/* Wait if RDRAM bytes [lo, hi) may still be written by queued primitives. */
uint32_t mt_pixels(struct rdp *r);
/* pixels that ran through a specialised variant per stage, all pixels, and the pixels that needed the texture stage */
void mt_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all);
void mt_sync_range(struct rdp *r, uint32_t lo, uint32_t hi);
/* Row storage for the primitive being set up (nrows rows, indexed 0..nrows-1). */
struct span *mt_alloc_rows(struct rdp *r, int nrows);

#endif
