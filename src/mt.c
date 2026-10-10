/*
 * mupen64plus-rdp-foir - mt.c
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

/* Stripe class c (stripe % K) is the unit of ordering: its tasks run in job order, one at a time, on whichever thread claims it
 * (home worker c % nw, or the parser as helper while it waits). The state word cls[c].st = (tasks done << 1) | busy is the lock:
 * claim by CAS to set busy, release by storing (n + 1) << 1. Rows are private to their class, so the pixel loop needs no locks. */

#define MT_MAXW      8
#define MT_MAXK      32
#define MT_JOBS      8192           /* power of two */
#define MT_JM        (MT_JOBS - 1)
#define MT_SNAPS     1024
#define MT_TMEMS     256
#define MT_MAXREG    8
#ifndef MT_SPIN_US
#define MT_SPIN_US   400
#endif
#define RS_MAX       2048

struct mt_job {
    int start, end;
    uint32_t state_gen, tmem_gen;
    uint16_t state_slot, tmem_slot;
    uint32_t cmask;
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
#ifdef FOIR_FRAMELOG
    _Atomic uint64_t busy_ns;
#endif
};

struct mt_class {
    _Atomic uint64_t st;              /* (tasks completed << 1) | busy */
    char pad[56];
};

struct mt_stats { unsigned long jobs, region_syncs, range_syncs, full_syncs, alloc_waits, snap_waits, tmem_waits, help_tasks; };

struct rdp_mt {
    struct mt_stats st;
    int nw, K;
    bool helper;
    struct mt_worker w[MT_MAXW];
    struct mt_worker hw;
    struct mt_job *jobs;
    _Atomic uint64_t head;
    struct mt_snap *snaps;
    struct mt_tmem *tmems;
    uint32_t gen_counter, cur_state_gen, cur_tmem_gen;
    int cur_state_slot, cur_tmem_slot;
    uint64_t snap_last_job[MT_SNAPS], tmem_last_job[MT_TMEMS];   /* slot reusable once floor passes it */
    int next_snap, next_tmem;
    struct mt_region reg[MT_MAXREG];
    int nreg;
    uint64_t floor_cache;             /* every job below it is complete in all classes (parser thread only) */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    _Atomic int sleepers;
    _Atomic int stop;
    uint32_t *cq[MT_MAXK];            /* [MT_JOBS] job indices (low 32 bits) */
    uint64_t tail_local[MT_MAXK];
    _Alignas(64) _Atomic uint64_t ctail[MT_MAXK];   /* release-stored after the cq entry */
    _Alignas(64) struct mt_class cls[MT_MAXK];
#ifdef FOIR_FRAMELOG
    _Atomic int prof;
    uint64_t sync_ns;
    uint64_t help_ns;
#endif
};

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

/* monotonic: the result only grows */
static uint64_t mt_floor(struct rdp_mt *mt)
{
    uint64_t h = atomic_load_explicit(&mt->head, memory_order_relaxed), f = h;
    for (int c = 0; c < mt->K; c++) {
        uint64_t n = atomic_load_explicit(&mt->cls[c].st, memory_order_acquire) >> 1;
        if (n < mt->tail_local[c]) {
            uint64_t j = h - (uint32_t)(h - mt->cq[c][n & MT_JM]);
            if (j < f) f = j;
        }
    }
    if (f > mt->floor_cache) mt->floor_cache = f;
    return mt->floor_cache;
}

static bool mt_all_done(struct rdp_mt *mt)
{
    for (int c = 0; c < mt->K; c++)
        if ((atomic_load_explicit(&mt->cls[c].st, memory_order_acquire) >> 1) != mt->tail_local[c]) return false;
    return true;
}

static void run_task(struct mt_worker *wk, const struct mt_job *job, int c)
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
    w->nworkers = mt->K; w->worker_id = c;           /* the raster code sees class c as worker c of K */
    raster_render_walk(w, &job->pw, job->start, job->end);
}

/* j is the job index (low 32 bits); release the class with (n + 1) << 1 */
static bool claim_task(struct rdp_mt *mt, int c0, int step, int *pc, uint64_t *pn, uint32_t *pj)
{
    for (;;) {
        int best = -1;
        uint64_t bs = 0;
        uint32_t bj = 0;
        for (int c = c0; c < mt->K; c += step) {
            uint64_t s = atomic_load_explicit(&mt->cls[c].st, memory_order_acquire);
            if (s & 1) continue;
            if ((s >> 1) == atomic_load_explicit(&mt->ctail[c], memory_order_acquire)) continue;
            uint32_t j = mt->cq[c][(s >> 1) & MT_JM];
            if (best < 0 || (int32_t)(j - bj) < 0) { best = c; bs = s; bj = j; }
        }
        if (best < 0) return false;
        if (atomic_compare_exchange_strong_explicit(&mt->cls[best].st, &bs, bs | 1, memory_order_acq_rel, memory_order_relaxed)) {
            *pc = best; *pn = bs >> 1; *pj = bj;
            return true;
        }
    }
}

#ifdef FOIR_FRAMELOG
static inline uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#endif

static void *worker_main(void *arg)
{
    struct mt_worker *wk = (struct mt_worker *)arg;
    struct rdp_mt *mt = wk->mt;
    for (;;) {
        int c;
        uint64_t n;
        uint32_t j;
        uint64_t seen = atomic_load_explicit(&mt->head, memory_order_acquire);
        if (!claim_task(mt, wk->id, mt->nw, &c, &n, &j)) {
            struct timespec t0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            unsigned spins = 0;
            for (;;) {
                if (atomic_load_explicit(&mt->stop, memory_order_acquire)) return NULL;
                cpu_relax();
                seen = atomic_load_explicit(&mt->head, memory_order_acquire);
                if (claim_task(mt, wk->id, mt->nw, &c, &n, &j)) break;
                if ((++spins & 127) != 0) continue;
                struct timespec t1;
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
                if (us < MT_SPIN_US) continue;
                pthread_mutex_lock(&mt->lock);
                atomic_fetch_add(&mt->sleepers, 1);
                while (atomic_load(&mt->head) == seen && !atomic_load(&mt->stop))
                    pthread_cond_wait(&mt->cond, &mt->lock);
                atomic_fetch_sub(&mt->sleepers, 1);
                pthread_mutex_unlock(&mt->lock);
                clock_gettime(CLOCK_MONOTONIC, &t0);
            }
        }
#ifdef FOIR_FRAMELOG
        if (atomic_load_explicit(&mt->prof, memory_order_relaxed)) {
            uint64_t t = mono_ns();
            run_task(wk, &mt->jobs[j & MT_JM], c);
            atomic_store_explicit(&wk->busy_ns, atomic_load_explicit(&wk->busy_ns, memory_order_relaxed) + (mono_ns() - t), memory_order_relaxed);
        } else
#endif
            run_task(wk, &mt->jobs[j & MT_JM], c);
        atomic_store_explicit(&mt->cls[c].st, (n + 1) << 1, memory_order_release);
    }
}

static void wake_workers(struct rdp_mt *mt)
{
    if (atomic_load(&mt->sleepers) > 0) {
        pthread_mutex_lock(&mt->lock);
        pthread_cond_broadcast(&mt->cond);
        pthread_mutex_unlock(&mt->lock);
    }
}

static bool help_one(struct rdp_mt *mt)
{
    int c;
    uint64_t n;
    uint32_t j;
    if (!mt->helper || !claim_task(mt, 0, 1, &c, &n, &j)) return false;
#ifdef FOIR_FRAMELOG
    if (atomic_load_explicit(&mt->prof, memory_order_relaxed)) {
        uint64_t t = mono_ns();
        run_task(&mt->hw, &mt->jobs[j & MT_JM], c);
        mt->help_ns += mono_ns() - t;
    } else
#endif
        run_task(&mt->hw, &mt->jobs[j & MT_JM], c);
    atomic_store_explicit(&mt->cls[c].st, (n + 1) << 1, memory_order_release);
    mt->st.help_tasks++;
    return true;
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

static void wait_all(struct rdp_mt *mt)
{
    int spins = 0;
    while (!mt_all_done(mt)) {
        if (help_one(mt)) { spins = 0; continue; }
        wait_progress(mt, &spins);
    }
}

static struct rdp *new_ctx(const struct rdp *r, int nworkers, int id)
{
    struct rdp *c = (struct rdp *)calloc(1, sizeof(struct rdp));
    if (!c) return NULL;
    c->rdram = r->rdram; c->rdram_size = r->rdram_size; c->hidden = r->hidden;
    c->cfg = r->cfg;
    c->kbase = r->kbase; c->ktab = r->kbase; c->k = &c->ktab;
    c->spec_ok_mask = r->spec_ok_mask; c->spec_dirty = true;
    c->nworkers = nworkers; c->worker_id = id;
    return c;
}

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return (e && *e) ? atoi(e) : def;
}

bool mt_start(struct rdp *r, int nworkers)
{
    if (nworkers < 1) return false;
    if (nworkers > MT_MAXW) nworkers = MT_MAXW;
    struct rdp_mt *mt = (struct rdp_mt *)aligned_alloc(64, (sizeof(*mt) + 63) & ~(size_t)63);
    if (!mt) return false;
    memset(mt, 0, sizeof(*mt));
    if (RSTATE_BYTES > RS_MAX) { free(mt); return false; }
    mt->helper = env_int("FOIR_HELPER", r->cfg.mt_helper ? 1 : 0) != 0;
    int g = env_int("FOIR_MT_CLASSES", mt->helper ? 2 : 1);
    if (g < 1) g = 1;
    if (g * nworkers > MT_MAXK) g = MT_MAXK / nworkers;
    mt->K = g * nworkers;
    mt->jobs = (struct mt_job *)calloc(MT_JOBS, sizeof(struct mt_job));
    mt->snaps = (struct mt_snap *)calloc(MT_SNAPS, sizeof(struct mt_snap));
    mt->tmems = (struct mt_tmem *)calloc(MT_TMEMS, sizeof(struct mt_tmem));
    if (!mt->jobs || !mt->snaps || !mt->tmems) goto fail;
    for (int c = 0; c < mt->K; c++) {
        mt->cq[c] = (uint32_t *)calloc(MT_JOBS, sizeof(uint32_t));
        if (!mt->cq[c]) goto fail;
    }
    memset(mt->jobs, 0, MT_JOBS * sizeof(struct mt_job));
    pthread_mutex_init(&mt->lock, NULL);
    pthread_cond_init(&mt->cond, NULL);
    mt->nw = nworkers;
    mt->gen_counter = 1;
    for (int i = 0; i < nworkers; i++) {
        struct mt_worker *wk = &mt->w[i];
        wk->mt = mt; wk->id = i;
        wk->ctx = new_ctx(r, nworkers, i);
        if (!wk->ctx) { mt->nw = i; goto fail; }
    }
    if (mt->helper) {
        mt->hw.mt = mt; mt->hw.id = nworkers;
        mt->hw.ctx = new_ctx(r, nworkers, 0);
        if (!mt->hw.ctx) goto fail;
    }
    for (int i = 0; i < nworkers; i++)
        if (pthread_create(&mt->w[i].thr, NULL, worker_main, &mt->w[i]) != 0) {
            atomic_store(&mt->stop, 1);
            for (int j = 0; j < i; j++) pthread_join(mt->w[j].thr, NULL);
            goto fail;
        }
    r->mt = mt;
    r->rs_dirty = true;
    r->tmem_dirty = true;
    return true;
fail:
    for (int i = 0; i < mt->nw; i++) free(mt->w[i].ctx);
    free(mt->hw.ctx);
    for (int c = 0; c < MT_MAXK; c++) free(mt->cq[c]);
    free(mt->jobs); free(mt->snaps); free(mt->tmems); free(mt);
    return false;
}

void mt_sync(struct rdp *r)
{
    struct rdp_mt *mt = r->mt;
    if (!mt) return;
    mt->st.full_syncs++;
#ifdef FOIR_FRAMELOG
    if (atomic_load_explicit(&mt->prof, memory_order_relaxed)) {
        uint64_t t0 = mono_ns();
        wait_all(mt);
        mt->sync_ns += mono_ns() - t0;
    } else
#endif
        wait_all(mt);
    mt->floor_cache = atomic_load_explicit(&mt->head, memory_order_relaxed);
    mt->nreg = 0;
}

void mt_stop(struct rdp *r)
{
    struct rdp_mt *mt = r->mt;
    if (!mt) return;
    mt_sync(r);
    if (getenv("FOIR_MT_STATS"))
        fprintf(stderr, "mt: %lu jobs, %lu syncs (%lu from render-target hazards, %lu from texture loads), waits for ring space: %lu jobs, %lu state snapshots, %lu tmem snapshots; %d classes, helper %s: %lu tasks\n",
                (unsigned long)atomic_load(&mt->head), mt->st.full_syncs, mt->st.region_syncs, mt->st.range_syncs,
                mt->st.alloc_waits, mt->st.snap_waits, mt->st.tmem_waits, mt->K, mt->helper ? "on" : "off", mt->st.help_tasks);
    atomic_store(&mt->stop, 1);
    pthread_mutex_lock(&mt->lock);
    pthread_cond_broadcast(&mt->cond);
    pthread_mutex_unlock(&mt->lock);
    uint32_t pix = 0;
    for (int i = 0; i <= mt->nw; i++) {
        struct mt_worker *wk = i < mt->nw ? &mt->w[i] : &mt->hw;
        if (i < mt->nw) pthread_join(wk->thr, NULL);
        if (!wk->ctx) continue;
        pix += wk->ctx->stat_pixels;
        for (int k = 0; k < COV_N; k++) r->cov_pix[k] += wk->ctx->cov_pix[k];
        r->cov_tex_all += wk->ctx->cov_tex_all;
        free(wk->ctx);
    }
    r->stat_pixels += pix;
    for (int c = 0; c < MT_MAXK; c++) free(mt->cq[c]);
    free(mt->jobs); free(mt->snaps); free(mt->tmems);
    pthread_mutex_destroy(&mt->lock);
    pthread_cond_destroy(&mt->cond);
    free(mt);
    r->mt = NULL;
}

#ifdef FOIR_FRAMELOG
void mt_prof_enable(struct rdp *r)
{
    if (r->mt) atomic_store(&r->mt->prof, 1);
}

int mt_prof_read(struct rdp *r, uint64_t busy[8], uint64_t *jobs, uint64_t *sync_ns, uint64_t *help_ns)
{
    struct rdp_mt *mt = r->mt;
    if (!mt) return 0;
    for (int i = 0; i < mt->nw && i < 8; i++) busy[i] = atomic_load_explicit(&mt->w[i].busy_ns, memory_order_relaxed);
    *jobs = atomic_load_explicit(&mt->head, memory_order_relaxed);
    *sync_ns = mt->sync_ns;
    *help_ns = mt->help_ns;
    return mt->nw < 8 ? mt->nw : 8;
}
#endif

uint32_t mt_pixels(struct rdp *r)
{
    struct rdp_mt *mt = r->mt;
    uint32_t pix = r->stat_pixels;
    if (mt) {
        for (int i = 0; i < mt->nw; i++) pix += mt->w[i].ctx->stat_pixels;
        if (mt->hw.ctx) pix += mt->hw.ctx->stat_pixels;
    }
    return pix;
}

void mt_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all)
{
    struct rdp_mt *mt = r->mt;
    for (int k = 0; k < COV_N; k++) pix[k] = r->cov_pix[k];
    *tex_all = r->cov_tex_all;
    if (mt)
        for (int i = 0; i <= mt->nw; i++) {
            const struct rdp *c = i < mt->nw ? mt->w[i].ctx : mt->hw.ctx;
            if (!c) continue;
            for (int k = 0; k < COV_N; k++) pix[k] += c->cov_pix[k];
            *tex_all += c->cov_tex_all;
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

static bool region_conflict(const struct rdp_mt *mt, const struct mt_job_region *g)
{
    for (int i = 0; i < mt->nreg; i++) {
        const struct mt_region *p = &mt->reg[i];
        if (p->addr == g->addr && p->width == g->width && p->bpp == g->bpp) continue;
        if (g->lo < p->hi && p->lo < g->hi) return true;
    }
    return false;
}

static struct mt_region *region_find(struct rdp_mt *mt, const struct mt_job_region *g)
{
    for (int i = 0; i < mt->nreg; i++) {
        struct mt_region *p = &mt->reg[i];
        if (p->addr == g->addr && p->width == g->width && p->bpp == g->bpp) return p;
    }
    return NULL;
}

static void region_add(struct rdp_mt *mt, const struct mt_job_region *g)
{
    struct mt_region *p = region_find(mt, g);
    if (p) {
        if (g->lo < p->lo) p->lo = g->lo;
        if (g->hi > p->hi) p->hi = g->hi;
        return;
    }
    p = &mt->reg[mt->nreg++];
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

    /* check every region before registering any: a barrier clears the list */
    int ct = r->om.cycle_type;
    int fbbpp = bytes_per_px(r->fb_size);
    struct mt_job_region regs[2];
    int nregs = 0;
    make_region(&regs[nregs++], r->fb_address, r->fb_width, fbbpp, start, end);
    if ((ct == CYCLE_TYPE_1 || ct == CYCLE_TYPE_2) && r->pf.need_z)
        make_region(&regs[nregs++], r->zb_address, r->fb_width, 2, start, end);
    bool barrier = false;
    for (int i = 0; i < nregs; i++) barrier |= region_conflict(mt, &regs[i]);
    if (!barrier) {
        int need = 0;
        for (int i = 0; i < nregs; i++) need += region_find(mt, &regs[i]) == NULL;
        barrier = mt->nreg + need > MT_MAXREG;
    }
    if (barrier) { mt->st.region_syncs++; mt_sync(r); }
    for (int i = 0; i < nregs; i++) region_add(mt, &regs[i]);

    uint64_t head = atomic_load_explicit(&mt->head, memory_order_relaxed);
    struct mt_job *job = &mt->jobs[head & MT_JM];
    int spins = 0;

    while (head - mt->floor_cache >= MT_JOBS - 1) {
        if (head - mt_floor(mt) < MT_JOBS - 1) break;
        if (spins == 0) mt->st.alloc_waits++;
        if (help_one(mt)) { spins = 1; continue; }
        wait_progress(mt, &spins);
    }

    if (r->rs_dirty) {
        int slot = mt->next_snap;
        spins = 0;
        while (mt->snap_last_job[slot] > mt->floor_cache) {
            if (mt->snap_last_job[slot] <= mt_floor(mt)) break;
            if (spins == 0) mt->st.snap_waits++;
            if (help_one(mt)) { spins = 1; continue; }
            wait_progress(mt, &spins);
        }
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
        while (mt->tmem_last_job[slot] > mt->floor_cache) {
            if (mt->tmem_last_job[slot] <= mt_floor(mt)) break;
            if (spins == 0) mt->st.tmem_waits++;
            if (help_one(mt)) { spins = 1; continue; }
            wait_progress(mt, &spins);
        }
        memcpy(mt->tmems[slot].data, r->tmem, sizeof(r->tmem));
        mt->cur_tmem_gen = ++mt->gen_counter;
        mt->tmems[slot].gen = mt->cur_tmem_gen;
        mt->cur_tmem_slot = slot;
        mt->next_tmem = (slot + 1) % MT_TMEMS;
        r->tmem_dirty = false;
    }
    mt->snap_last_job[mt->cur_state_slot] = head + 1;
    mt->tmem_last_job[mt->cur_tmem_slot] = head + 1;

    uint32_t cmask;
    {
        int s0 = start >> FOIR_STRIPE_SHIFT, s1 = end >> FOIR_STRIPE_SHIFT, K = mt->K;
        if (s1 - s0 + 1 >= K) cmask = K >= 32 ? 0xffffffffu : ((1u << K) - 1);
        else {
            cmask = 0;
            for (int s = s0; s <= s1; s++) cmask |= 1u << (s % K);
        }
    }
    job->start = start; job->end = end;
    job->pw = *pw;
    job->pc = r->pc;
    job->cmask = cmask;
    job->state_gen = mt->cur_state_gen; job->state_slot = (uint16_t)mt->cur_state_slot;
    job->tmem_gen = mt->cur_tmem_gen; job->tmem_slot = (uint16_t)mt->cur_tmem_slot;
    for (uint32_t m = cmask; m; m &= m - 1) {
        int c = __builtin_ctz(m);
        mt->cq[c][mt->tail_local[c] & MT_JM] = (uint32_t)head;
        atomic_store_explicit(&mt->ctail[c], ++mt->tail_local[c], memory_order_release);
    }
    atomic_store_explicit(&mt->head, head + 1, memory_order_seq_cst);
    if (atomic_load(&mt->sleepers) > 0) wake_workers(mt);
}
