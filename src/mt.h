#ifndef FOIR_MT_H
#define FOIR_MT_H

#include "rdp.h"

#define FOIR_STRIPE_SHIFT 2

bool mt_start(struct rdp *r, int threads);
void mt_stop(struct rdp *r);
void mt_submit(struct rdp *r, const struct prim_walk *pw, int start, int end);
void mt_sync(struct rdp *r);
void mt_sync_frame(struct rdp *r);
void mt_wait_range(struct rdp *r, uint32_t lo, uint32_t hi);   /* wait for the pending jobs that write [lo, hi) only */     /* the drain at SYNC_FULL and at the VI scan-out (skipped by FOIR_SKIP_BARRIERS bit 4, a measurement) */
uint32_t mt_pixels(struct rdp *r);
void mt_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all);
#ifdef FOIR_FRAMELOG
void mt_prof_enable(struct rdp *r);
int mt_prof_read(struct rdp *r, uint64_t busy[8], uint64_t *jobs, uint64_t *sync_ns, uint64_t *help_ns);
#endif
void mt_sync_range(struct rdp *r, uint32_t lo, uint32_t hi);
/* the pool counters since the previous call, formatted (empty string without a pool) */
const char *mt_stats_delta(struct rdp *r);

#endif
