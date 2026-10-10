/*
 * mupen64plus-rdp-foir - mt.c
 * Band-split worker pool.
 *
 * The parser thread decodes commands, runs primitive setup and publishes each
 * primitive as a job: the per-primitive constants, the edge walker's setup
 * record (struct prim_walk: the edge and interpolant coefficients) and, when
 * changed since the previous job, a snapshot of the RDP render state and of
 * TMEM. Worker threads compute the span of each row they own from the setup
 * record (walk.h) and run the pixel pipeline for it -- row y belongs to worker
 * (y >> FOIR_STRIPE_SHIFT) % nworkers -- so the hot loop shares nothing between
 * threads and needs no locks: framebuffer, Z and "hidden bit" rows are private
 * to their owner, and no span records travel between cores.
 *
 * All workers stop together only when the parser needs a consistent RDRAM:
 * SYNC_FULL, VI scan-out, CPU framebuffer reads, a texture load from memory the
 * queued primitives may still write, or a change of render-target geometry
 * that would break the row ownership of memory still being written.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "rdp.h"
#include "mt.h"

#define MT_MAXW      8
#define MT_JOBS      8192           /* power of two */
#define MT_SNAPS     1024
#define MT_TMEMS     256
#define MT_MAXREG    8
#ifndef MT_SPIN_US
#define MT_SPIN_US   400            /* idle workers spin this long before sleeping */
#endif
#define RS_MAX       2048

struct mt_job {
    int start, end;
    uint32_t state_gen, tmem_gen;
    uint16_t state_slot, tmem_slot;
    struct prim_ctx pc;
    struct prim_walk pw;
};

struct mt_region { uint32_t addr; int width, bpp; uint32_t lo, hi; };

struct mt_snap { uint32_t gen; uint8_t data[RS_MAX]; };
struct mt_tmem { uint32_t gen; uint8_t data[0x1000]; };

struct mt_worker {
    struct rdp_mt *mt;
    struct rdp *ctx;
    int id;
    pthread_t thr;
    uint32_t state_gen, tmem_gen;
    _Atomic uint64_t done;            /* jobs completed */
    char pad[64];
};

struct mt_stats { unsigned long jobs, region_syncs, range_syncs, full_syncs, alloc_waits, snap_waits, tmem_waits; };   /* alloc_waits: job ring full */

struct rdp_mt {
    struct mt_stats st;
    int nw;
    struct mt_worker w[MT_MAXW];
    struct mt_job *jobs;
    _Atomic uint64_t head;            /* jobs published */
    struct mt_snap *snaps;
    struct mt_tmem *tmems;
    uint32_t gen_counter, cur_state_gen, cur_tmem_gen;
    int cur_state_slot, cur_tmem_slot;
    uint64_t snap_last_job[MT_SNAPS], tmem_last_job[MT_TMEMS];   /* last job index using a slot */
    int next_snap, next_tmem;
    struct mt_region reg[MT_MAXREG];
    int nreg;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    _Atomic int sleepers;
    _Atomic int stop;
};

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

static uint64_t min_done(struct rdp_mt *mt)
{
    uint64_t m = ~0ull;
    for (int i = 0; i < mt->nw; i++) {
        uint64_t d = atomic_load_explicit(&mt->w[i].done, memory_order_acquire);
        if (d < m) m = d;
    }
    return m;
}

/* ---- worker side ------------------------------------------------------------------------- */

static void run_job(struct mt_worker *wk, const struct mt_job *job)
{
    struct rdp_mt *mt = wk->mt;
    struct rdp *w = wk->ctx;
    if (job->state_gen != wk->state_gen) {
        memcpy(&w->rs_begin, mt->snaps[job->state_slot].data, RSTATE_BYTES);
        wk->state_gen = job->state_gen;
        w->spec_dirty = true;
    }
    if (job->tmem_gen != wk->tmem_gen) {
        memcpy(w->tmem, mt->tmems[job->tmem_slot].data, sizeof(w->tmem));
        wk->tmem_gen = job->tmem_gen;
    }
    w->pc = job->pc;
    raster_render_walk(w, &job->pw, job->start, job->end);
}

static void *worker_main(void *arg)
{
    struct mt_worker *wk = (struct mt_worker *)arg;
    struct rdp_mt *mt = wk->mt;
    uint64_t next = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&mt->head, memory_order_acquire);
        if (next == h) {
            /* idle: spin for a while (a new primitive usually follows within microseconds; sleeping and being
             * woken costs a futex round trip on both threads), then block */
            struct timespec t0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            unsigned spins = 0;
            while ((h = atomic_load_explicit(&mt->head, memory_order_acquire)) == next) {
                if (atomic_load_explicit(&mt->stop, memory_order_acquire)) return NULL;
                cpu_relax();
                if ((++spins & 127) != 0) continue;
                struct timespec t1;
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
                if (us < MT_SPIN_US) continue;
                pthread_mutex_lock(&mt->lock);
                atomic_fetch_add(&mt->sleepers, 1);
                while (atomic_load(&mt->head) == next && !atomic_load(&mt->stop))
                    pthread_cond_wait(&mt->cond, &mt->lock);
                atomic_fetch_sub(&mt->sleepers, 1);
                pthread_mutex_unlock(&mt->lock);
                clock_gettime(CLOCK_MONOTONIC, &t0);
            }
        }
        while (next < h) {
            run_job(wk, &mt->jobs[next & (MT_JOBS - 1)]);
            next++;
            atomic_store_explicit(&wk->done, next, memory_order_release);
        }
    }
}

/* ---- parser side ---------------------------------------------------------------------------- */

static void wake_workers(struct rdp_mt *mt)
{
    if (atomic_load(&mt->sleepers) > 0) {
        pthread_mutex_lock(&mt->lock);
        pthread_cond_broadcast(&mt->cond);
        pthread_mutex_unlock(&mt->lock);
    }
}

static void wait_progress(struct rdp_mt *mt, int *spins)
{
    if (++*spins < 2000) cpu_relax();
    else if (*spins < 20000) sched_yield();
    else {
        struct timespec ts = { 0, 50000 };
        nanosleep(&ts, NULL);
    }
    (void)mt;
}

bool mt_start(struct rdp *r, int nworkers)
{
    if (nworkers < 1) return false;
    if (nworkers > MT_MAXW) nworkers = MT_MAXW;
    struct rdp_mt *mt = (struct rdp_mt *)calloc(1, sizeof(*mt));
    if (!mt) return false;
    if (RSTATE_BYTES > RS_MAX) { free(mt); return false; }
    mt->jobs = (struct mt_job *)calloc(MT_JOBS, sizeof(struct mt_job));
    mt->snaps = (struct mt_snap *)calloc(MT_SNAPS, sizeof(struct mt_snap));
    mt->tmems = (struct mt_tmem *)calloc(MT_TMEMS, sizeof(struct mt_tmem));
    if (!mt->jobs || !mt->snaps || !mt->tmems) goto fail;
    memset(mt->jobs, 0, MT_JOBS * sizeof(struct mt_job));   /* fault the pages in now, not during the first frame */
    pthread_mutex_init(&mt->lock, NULL);
    pthread_cond_init(&mt->cond, NULL);
    mt->nw = nworkers;
    mt->gen_counter = 1;
    for (int i = 0; i < nworkers; i++) {
        struct mt_worker *wk = &mt->w[i];
        wk->mt = mt; wk->id = i;
        struct rdp *c = (struct rdp *)calloc(1, sizeof(struct rdp));
        if (!c) { mt->nw = i; goto fail; }
        c->rdram = r->rdram; c->rdram_size = r->rdram_size; c->hidden = r->hidden;
        c->cfg = r->cfg;
        c->kbase = r->kbase; c->ktab = r->kbase; c->k = &c->ktab;       /* each worker selects its own variants */
        c->spec_ok_mask = r->spec_ok_mask; c->spec_dirty = true;
        c->nworkers = nworkers; c->worker_id = i;
        wk->ctx = c;
        atomic_store(&wk->done, 0);
    }
    for (int i = 0; i < nworkers; i++)
        if (pthread_create(&mt->w[i].thr, NULL, worker_main, &mt->w[i]) != 0) {
            atomic_store(&mt->stop, 1);
            for (int j = 0; j < i; j++) pthread_join(mt->w[j].thr, NULL);
            for (int j = 0; j < nworkers; j++) free(mt->w[j].ctx);
            goto fail;
        }
    r->mt = mt;
    r->rs_dirty = true;
    r->tmem_dirty = true;
    return true;
fail:
    free(mt->jobs); free(mt->snaps); free(mt->tmems); free(mt);
    return false;
}

void mt_sync(struct rdp *r)
{
    struct rdp_mt *mt = r->mt;
    if (!mt) return;
    uint64_t h = atomic_load_explicit(&mt->head, memory_order_relaxed);
    int spins = 0;
    mt->st.full_syncs++;
    while (min_done(mt) < h) wait_progress(mt, &spins);
    mt->nreg = 0;
}

void mt_stop(struct rdp *r)
{
    struct rdp_mt *mt = r->mt;
    if (!mt) return;
    mt_sync(r);
    if (getenv("FOIR_MT_STATS"))
        fprintf(stderr, "mt: %lu jobs, %lu syncs (%lu from render-target hazards, %lu from texture loads), waits for ring space: %lu jobs, %lu state snapshots, %lu tmem snapshots\n",
                (unsigned long)atomic_load(&mt->head), mt->st.full_syncs, mt->st.region_syncs, mt->st.range_syncs,
                mt->st.alloc_waits, mt->st.snap_waits, mt->st.tmem_waits);
    atomic_store(&mt->stop, 1);
    pthread_mutex_lock(&mt->lock);
    pthread_cond_broadcast(&mt->cond);
    pthread_mutex_unlock(&mt->lock);
    uint32_t pix = 0;
    for (int i = 0; i < mt->nw; i++) {
        pthread_join(mt->w[i].thr, NULL);
        pix += mt->w[i].ctx->stat_pixels;
        for (int k = 0; k < COV_N; k++) r->cov_pix[k] += mt->w[i].ctx->cov_pix[k];
        r->cov_tex_all += mt->w[i].ctx->cov_tex_all;
        free(mt->w[i].ctx);
    }
    r->stat_pixels += pix;
    free(mt->jobs); free(mt->snaps); free(mt->tmems);
    pthread_mutex_destroy(&mt->lock);
    pthread_cond_destroy(&mt->cond);
    free(mt);
    r->mt = NULL;
}

uint32_t mt_pixels(struct rdp *r)
{
    struct rdp_mt *mt = r->mt;
    uint32_t pix = r->stat_pixels;
    if (mt)
        for (int i = 0; i < mt->nw; i++) pix += mt->w[i].ctx->stat_pixels;
    return pix;
}

void mt_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all)
{
    struct rdp_mt *mt = r->mt;
    for (int k = 0; k < COV_N; k++) pix[k] = r->cov_pix[k];
    *tex_all = r->cov_tex_all;
    if (mt)
        for (int i = 0; i < mt->nw; i++) {
            for (int k = 0; k < COV_N; k++) pix[k] += mt->w[i].ctx->cov_pix[k];
            *tex_all += mt->w[i].ctx->cov_tex_all;
        }
}

static int bytes_per_px(int size) { return size == PIXEL_SIZE_4BIT ? 1 : (1 << size) >> 1; }

struct mt_job_region { uint32_t addr; int width, bpp; uint32_t lo, hi; };

static void make_region(struct mt_job_region *g, uint32_t addr, int width, int bpp, int y0, int y1)
{
    g->addr = addr; g->width = width; g->bpp = bpp;
    g->lo = addr + (uint32_t)(y0 * width * bpp);
    g->hi = addr + (uint32_t)((y1 + 1) * width * bpp);
}

/* true when the region cannot join the queued work without a barrier: it overlaps memory a queued primitive
 * writes with a different row mapping, or the region list is full */
static bool region_conflict(const struct rdp_mt *mt, const struct mt_job_region *g)
{
    bool have_key = false;
    for (int i = 0; i < mt->nreg; i++) {
        const struct mt_region *p = &mt->reg[i];
        if (p->addr == g->addr && p->width == g->width && p->bpp == g->bpp) { have_key = true; continue; }
        if (g->lo < p->hi && p->lo < g->hi) return true;     /* overlaps work with a different row mapping */
    }
    return !have_key && mt->nreg >= MT_MAXREG;
}

static void region_add(struct rdp_mt *mt, const struct mt_job_region *g)
{
    for (int i = 0; i < mt->nreg; i++) {
        struct mt_region *p = &mt->reg[i];
        if (p->addr == g->addr && p->width == g->width && p->bpp == g->bpp) {
            if (g->lo < p->lo) p->lo = g->lo;
            if (g->hi > p->hi) p->hi = g->hi;
            return;
        }
    }
    struct mt_region *p = &mt->reg[mt->nreg++];
    p->addr = g->addr; p->width = g->width; p->bpp = g->bpp; p->lo = g->lo; p->hi = g->hi;
}

void mt_sync_range(struct rdp *r, uint32_t lo, uint32_t hi)
{
    struct rdp_mt *mt = r->mt;
    if (!mt || mt->nreg == 0) return;
    for (int i = 0; i < mt->nreg; i++)
        if (lo < mt->reg[i].hi && mt->reg[i].lo < hi) { mt->st.range_syncs++; mt_sync(r); return; }
}

void mt_submit(struct rdp *r, const struct prim_walk *pw, int start, int end)
{
    struct rdp_mt *mt = r->mt;
    if (end < start) return;

    /* row-ownership hazards on the memory this primitive renders to. All the regions of the job are checked
     * first: a barrier clears the list, so the regions are registered only after it. */
    int ct = r->om.cycle_type;
    int fbbpp = bytes_per_px(r->fb_size);
    struct mt_job_region regs[2];
    int nregs = 0;
    make_region(&regs[nregs++], r->fb_address, r->fb_width, fbbpp, start, end);
    if ((ct == CYCLE_TYPE_1 || ct == CYCLE_TYPE_2) && r->pf.need_z)
        make_region(&regs[nregs++], r->zb_address, r->fb_width, 2, start, end);
    bool barrier = false;
    for (int i = 0; i < nregs; i++) barrier |= region_conflict(mt, &regs[i]);
    if (barrier) { mt->st.region_syncs++; mt_sync(r); }
    for (int i = 0; i < nregs; i++) region_add(mt, &regs[i]);

    uint64_t head = atomic_load_explicit(&mt->head, memory_order_relaxed);
    struct mt_job *job = &mt->jobs[head & (MT_JOBS - 1)];
    int spins = 0;

    /* a free slot in the job ring */
    while (head - min_done(mt) >= MT_JOBS - 1) { if (spins == 0) mt->st.alloc_waits++; wait_progress(mt, &spins); }

    if (r->rs_dirty) {
        int slot = mt->next_snap;
        spins = 0;
        while (mt->snap_last_job[slot] > min_done(mt)) { if (spins == 0) mt->st.snap_waits++; wait_progress(mt, &spins); }
        memcpy(mt->snaps[slot].data, &r->rs_begin, RSTATE_BYTES);
        mt->cur_state_gen = ++mt->gen_counter;
        mt->snaps[slot].gen = mt->cur_state_gen;
        mt->cur_state_slot = slot;
        mt->next_snap = (slot + 1) % MT_SNAPS;
        r->rs_dirty = false;
    }
    if (r->tmem_dirty) {
        int slot = mt->next_tmem;
        spins = 0;
        while (mt->tmem_last_job[slot] > min_done(mt)) { if (spins == 0) mt->st.tmem_waits++; wait_progress(mt, &spins); }
        memcpy(mt->tmems[slot].data, r->tmem, sizeof(r->tmem));
        mt->cur_tmem_gen = ++mt->gen_counter;
        mt->tmems[slot].gen = mt->cur_tmem_gen;
        mt->cur_tmem_slot = slot;
        mt->next_tmem = (slot + 1) % MT_TMEMS;
        r->tmem_dirty = false;
    }
    mt->snap_last_job[mt->cur_state_slot] = head + 1;
    mt->tmem_last_job[mt->cur_tmem_slot] = head + 1;

    job->start = start; job->end = end;
    job->pw = *pw;
    job->pc = r->pc;
    job->state_gen = mt->cur_state_gen; job->state_slot = (uint16_t)mt->cur_state_slot;
    job->tmem_gen = mt->cur_tmem_gen; job->tmem_slot = (uint16_t)mt->cur_tmem_slot;
    atomic_store_explicit(&mt->head, head + 1, memory_order_seq_cst);
    if (atomic_load(&mt->sleepers) > 0) wake_workers(mt);
}
