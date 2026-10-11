/*
 * mupen64plus-rdp-foir - overdraw.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"

#ifdef FOIR_OVERDRAW
/* Overdraw / early-Z measurement build (-DFOIR_OVERDRAW; single-threaded foir-replay only).
 *
 * od_stages() replaces the five pixel-stage calls of batch_flush(): it times every stage per batch (tick counter), snapshots the write-enable lanes after
 * zcmp and after blend, and classifies every lane as
 *   zfail   write enable cleared by the Z compare            (needs only Z memory, coverage and the interpolated Z: no texel, no combiner)
 *   late    passed Z, cleared by blend (alpha compare / coverage 0)
 *   write   written to the colour image
 * A shadow array per colour-image pixel then tells which writes were REPLACED later by a write that did not read the old colour (the write is dead
 * work: a visibility sort could have skipped it), and how many times each pixel was processed (depth complexity). The cost of every lane is the
 * measured stage time of its batch (texture / combine / zcmp evenly over the batch lanes, blend over the lanes that passed Z, store over the written lanes),
 * so the split of the time into survivors / replaced / zfail / late is a per-lane attribution, not a per-lane measurement.
 * Nothing in the rendering changes. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

static inline uint64_t od_tick(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
#elif defined(__x86_64__)
    return __rdtsc();
#else
    return 0;
#endif
}

struct od_ent {                /* one per colour-image pixel (key = byte address / 2) */
    uint16_t np;               /* times processed (lanes in a batch) */
    uint16_t nw;               /* times written */
    uint8_t ng[3];             /* times processed by state group: [0] force blend (XLU), [1] alpha compare, [2] other */
    uint8_t nzf;               /* times Z-failed */
    uint8_t cnt[4];            /* live writes by bucket: bit1 = z update, bit0 = alpha compare (needs the texel to decide) */
    float cost[4];
};

enum { OC_ZFAIL, OC_LATE, OC_DEAD, OC_SURV, OC_N };

struct od_img {
    uint32_t addr; int size, width;
    uint64_t proc, zfail, late, written, replacing, dead, fill, copy;
    double cost[OC_N];          /* ticks */
    uint64_t dead_ac, dead_pre; double cost_dead_ac, cost_dead_pre;   /* dead_pre: victim and killer both without alpha compare / cvg*alpha (their Z outcome would be known to a Z pre-pass) */   /* dead writes whose own state had an alpha compare */
    uint32_t kmin, kmax;
};

#define OD_MAXIMG 32
#define OD_MAXCLS 256

struct od_cls {
    uint64_t key; char desc[200];
    int ez_ok;                  /* the Z compare could run before texture / combine without changing the result (z_compare_en && !cvg_times_alpha) */
    uint64_t batches, lanes, zfail, late, written, dead, groups, groups_alive, groups_compact, dead_batches, prov;
    uint64_t ez_checked, ez_mismatch, prov_checked, prov_violation;   /* exactness checks: zcmp run before texture / combine == zcmp run after (ez states); provably dead => really dead (cvg*alpha states) */
    double t_zc_dead;           /* zcmp ticks of batches that are completely dead (a tile / batch Z test could skip them whole) */
    uint32_t last_prim; uint64_t nprim;   /* distinct primitives that flushed a batch in this class */
    double t_early;             /* ticks of the extra zcmp calls (cvg candidates 1..8) a cvg_times_alpha state needs to prove a lane dead before the combiner */
    double t[5];                /* tex, comb, zcmp, blend, store ticks */
    double sv1, sv2, sv3;       /* ticks of tex+comb an early Z compare would have skipped: whole dead batches / dead 8-lane groups / compaction of the live lanes */
};

static int od_mutate = -1;   /* FOIR_OD_MUTATE=1: deliberate errors in the early results, the exactness counters must then report mismatches */

static struct {
    struct od_ent *sh; size_t nsh;
    struct od_img img[OD_MAXIMG]; int nimg;
    struct od_cls cls[OD_MAXCLS]; int ncls;
    uint64_t nbatch;
    double ovh, ticks_per_s;
    uint64_t fill_px, copy_px;
    uint32_t vi_origin;
    uint32_t pl[8192], pd[8192], pz[8192];   /* per primitive id: lanes, provably dead lanes (ez states), lanes in a state that cannot test Z first */
    uint64_t hist_batch_n[9];   /* batch occupancy histogram: n/8 */
} od;

static double od_overhead(void)
{
    uint64_t sum = 0;
    for (int i = 0; i < 200000; i++) { uint64_t a = od_tick(), b = od_tick(); sum += b - a; }
    return (double)sum / 200000.0;
}

void od_reset(struct rdp *r, uint32_t vi_origin)
{
    free(od.sh);
    memset(&od, 0, sizeof od);
    od.nsh = r->rdram_size / 2;
    od.sh = calloc(od.nsh, sizeof(struct od_ent));
    od.ovh = od_overhead();
    od.vi_origin = vi_origin;
    if (od_mutate < 0) od_mutate = getenv("FOIR_OD_MUTATE") != NULL;
#if defined(__aarch64__)
    uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); od.ticks_per_s = (double)f;
#else
    {   /* TSC: calibrate against CLOCK_MONOTONIC */
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); uint64_t t0 = od_tick();
        do clock_gettime(CLOCK_MONOTONIC, &b); while ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec) < 5e7);
        od.ticks_per_s = (double)(od_tick() - t0) / ((b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9);
    }
#endif
}

static struct od_img *od_find_img(const struct rdp *r)
{
    for (int i = 0; i < od.nimg; i++)
        if (od.img[i].addr == r->fb_address && od.img[i].size == r->fb_size && od.img[i].width == r->fb_width) return &od.img[i];
    if (od.nimg == OD_MAXIMG) return &od.img[OD_MAXIMG - 1];
    struct od_img *m = &od.img[od.nimg++];
    m->addr = r->fb_address; m->size = r->fb_size; m->width = r->fb_width; m->kmin = ~0u;
    return m;
}

static inline size_t od_key(const struct rdp *r, uint32_t idx)
{
    uint32_t bpp = r->fb_size == PIXEL_SIZE_32BIT ? 4 : r->fb_size == PIXEL_SIZE_16BIT ? 2 : 1;
    size_t k = ((size_t)r->fb_address + (size_t)idx * bpp) >> 1;
    return k < od.nsh ? k : od.nsh - 1;
}

static struct od_cls *od_find_cls(const struct rdp *r, int blmem, int *ez_ok)
{
    const struct other_modes *om = &r->om;
    uint64_t key = (uint64_t)om->cycle_type | (uint64_t)om->z_compare_en << 2 | (uint64_t)om->z_update_en << 3 | (uint64_t)om->z_mode << 4 |
                   (uint64_t)(om->alpha_compare_en != 0) << 6 | (uint64_t)(om->cvg_times_alpha != 0) << 7 | (uint64_t)(om->antialias_en != 0) << 8 |
                   (uint64_t)(om->force_blend != 0) << 9 | (uint64_t)(om->image_read_en != 0) << 10 | (uint64_t)r->pf.need_tex << 11 |
                   (uint64_t)blmem << 12 | (uint64_t)(om->alpha_cvg_select != 0) << 13 | (uint64_t)om->cvg_dest << 14 | (uint64_t)(om->color_on_cvg != 0) << 16 |
                   (uint64_t)(om->dither_alpha_en != 0) << 17 | (uint64_t)(om->z_source_sel != 0) << 18 | (uint64_t)(r->fb_size) << 19;
    *ez_ok = om->z_compare_en && !om->cvg_times_alpha;
    for (int i = 0; i < od.ncls; i++) if (od.cls[i].key == key) return &od.cls[i];
    if (od.ncls == OD_MAXCLS) return &od.cls[OD_MAXCLS - 1];
    struct od_cls *c = &od.cls[od.ncls++];
    c->key = key; c->ez_ok = *ez_ok;
    snprintf(c->desc, sizeof c->desc, "cyc=%d zc=%d zu=%d zm=%d ac=%d cta=%d aa=%d fb=%d rd=%d tex=%d blmem=%d acs=%d cd=%d coc=%d dal=%d zs=%d fbs=%d",
             om->cycle_type + 1, om->z_compare_en, om->z_update_en, om->z_mode, om->alpha_compare_en != 0, om->cvg_times_alpha != 0, om->antialias_en != 0,
             om->force_blend != 0, om->image_read_en != 0, r->pf.need_tex, blmem, om->alpha_cvg_select != 0, om->cvg_dest, om->color_on_cvg != 0,
             om->dither_alpha_en != 0, om->z_source_sel != 0, r->fb_size);
    return c;
}

/* does the colour computed for this lane depend on the memory colour (so the earlier write is not simply replaced)? */
static inline int od_lane_reads_mem(const struct rdp *r, const struct px_batch *b, int i, const uint8_t *wz)
{
    (void)wz;
    const struct other_modes *om = &r->om;
    const int two = om->cycle_type == CYCLE_TYPE_2, cyc = two ? 1 : 0;
    int mem;
    if (!om->color_on_cvg || b->prewrap[i]) {
        int dontblend = om->partialreject[cyc] && b->pixel[CH_A][i] >= 0xff;
        if (!b->blend_en[i] || dontblend) mem = (om->blend_m1a[cyc] & 3) == 1;
        else mem = (om->blend_m1a[cyc] & 3) == 1 || (om->blend_m2a[cyc] & 3) == 1 || (om->blend_m2b[cyc] & 3) == 1;
    } else mem = (om->blend_m2a[cyc] & 3) == 1;
    if (two) mem |= (om->blend_m1a[0] & 3) == 1 || (om->blend_m2a[0] & 3) == 1 || (om->blend_m2b[0] & 3) == 1;
    return mem;
}

static inline int od_state_blmem(const struct rdp *r)
{
    const struct other_modes *om = &r->om;
    int m = 0;
    for (int c = 0; c <= (om->cycle_type == CYCLE_TYPE_2 ? 1 : 0); c++)
        m |= (om->blend_m1a[c] & 3) == 1 || (om->blend_m2a[c] & 3) == 1 || (om->blend_m2b[c] & 3) == 1;
    return m;
}

void od_stages(struct rdp *r, struct px_batch *b)
{
    const struct foir_kernels *k = r->k;
    const struct other_modes *om = &r->om;
    const int n = b->n;
    uint8_t prov[FOIR_LANES];   /* lane provably dead after Z without running texture / combine */
    int need_any = om->z_compare_en && om->cvg_times_alpha;
    uint64_t te = 0;
    if (need_any) {
        /* cvg_times_alpha: the combiner changes the coverage the Z compare sees (cvg' = ((alpha * cvg + 4) >> 3 >> 5) & 15, 0..8), so the Z compare cannot
         * run first with the real value; a lane is provably dead only if the Z compare fails for every possible cvg' (1..8; 0 is dead anyway) */
        static struct px_batch bc;
        memset(prov, 1, (size_t)n);
        for (int v = om->antialias_en ? 1 : 0; v <= 8; v++) {   /* cvg' = 0 is dead at blend only with antialiasing on */
            bc = *b;
            for (int i = 0; i < n; i++) bc.cvg[i] = (uint8_t)v;
            uint64_t ta = od_tick();
            k->zcmp(r, &bc);
            te += od_tick() - ta - (uint64_t)(od.ovh + 0.5);
            for (int i = 0; i < n; i++) prov[i] &= bc.wen[i] == 0;
        }
    }
    static struct px_batch pre;
    const int ez_state = om_early_z_safe(om);   /* the rule batch_flush() uses */
    if (ez_state) { pre = *b; k->zcmp(r, &pre); }   /* before any stage and before any store of this batch */
    uint64_t t0 = od_tick(), t1, t2, t3, t4, t5;
    if (r->pf.need_tex) k->texture(r, b);
    t1 = od_tick();
    k->combine(r, b);
    t2 = od_tick();
    k->zcmp(r, b);
    t3 = od_tick();
    uint8_t wz[FOIR_LANES], wb[FOIR_LANES];
    memcpy(wz, b->wen, (size_t)n);
    k->blend(r, b);
    t4 = od_tick();
    memcpy(wb, b->wen, (size_t)n);
    k->store(r, b);
    t5 = od_tick();

    od.nbatch++;
    int ez_ok;
    struct od_cls *c = od_find_cls(r, od_state_blmem(r), &ez_ok);
    struct od_img *im = od_find_img(r);
    const double ov = od.ovh;
    double tt[5] = { (double)(t1 - t0) - ov, (double)(t2 - t1) - ov, (double)(t3 - t2) - ov, (double)(t4 - t3) - ov, (double)(t5 - t4) - ov };
    if (!r->pf.need_tex) tt[0] = 0;
    for (int s = 0; s < 5; s++) { c->t[s] += tt[s]; }

    int nz = 0, nw = 0;
    for (int i = 0; i < n; i++) { nz += wz[i] != 0; nw += wb[i] != 0; }
    const int ng = (n + 7) / 8;
    int alive_g = 0;
    for (int g = 0; g < ng; g++) {
        int any = 0;
        for (int i = g * 8; i < g * 8 + 8 && i < n; i++) any |= wz[i] != 0;
        alive_g += any;
    }
    const int comp_g = (nz + 7) / 8;
    if (c->nprim == 0 || c->last_prim != r->stat_prims) { c->nprim++; c->last_prim = r->stat_prims; }
    c->batches++; c->lanes += (uint64_t)n; c->zfail += (uint64_t)(n - nz); c->late += (uint64_t)(nz - nw); c->written += (uint64_t)nw;
    c->groups += (uint64_t)ng; c->groups_alive += (uint64_t)alive_g; c->groups_compact += (uint64_t)comp_g;
    c->dead_batches += nz == 0;
    od.hist_batch_n[(n - 1) / 8]++;
    const double tc = tt[0] + tt[1];
    if (ez_state) {
        /* exactness of hoisting: the Z compare on the batch as it was BEFORE texture / combine must give the same outputs as the one that ran after them */
        if (od_mutate) pre.wen[0] ^= 1;
        int bad = 0;
        for (int i = 0; i < n; i++)
            bad |= pre.wen[i] != wz[i] || pre.cvg[i] != b->cvg[i] || pre.blend_en[i] != b->blend_en[i] || pre.prewrap[i] != b->prewrap[i] || pre.sz[i] != b->sz[i] ||
                   pre.memcvg[i] != b->memcvg[i] || pre.blshifta[i] != b->blshifta[i] || pre.blshiftb[i] != b->blshiftb[i] ||
                   pre.mem[0][i] != b->mem[0][i] || pre.mem[1][i] != b->mem[1][i] || pre.mem[2][i] != b->mem[2][i] || pre.mem[3][i] != b->mem[3][i];
        c->ez_checked++; c->ez_mismatch += bad != 0;
        if (bad && getenv("FOIR_OD_DEBUG")) {
            for (int i = 0; i < n; i++)
                if (pre.wen[i] != wz[i] || pre.cvg[i] != b->cvg[i] || pre.blend_en[i] != b->blend_en[i] || pre.prewrap[i] != b->prewrap[i] || pre.sz[i] != b->sz[i] ||
                    pre.memcvg[i] != b->memcvg[i] || pre.blshifta[i] != b->blshifta[i] || pre.blshiftb[i] != b->blshiftb[i] || pre.mem[0][i] != b->mem[0][i] || pre.mem[1][i] != b->mem[1][i] || pre.mem[2][i] != b->mem[2][i] || pre.mem[3][i] != b->mem[3][i]) {
                    fprintf(stderr, "lane %d wen %d/%d cvg %d/%d ben %d/%d pw %d/%d sz %d/%d mcvg %d/%d bsa %d/%d bsb %d/%d mem %d/%d %d/%d %d/%d %d/%d\n", i, pre.wen[i], wz[i], pre.cvg[i], b->cvg[i],
                            pre.blend_en[i], b->blend_en[i], pre.prewrap[i], b->prewrap[i], pre.sz[i], b->sz[i], pre.memcvg[i], b->memcvg[i], pre.blshifta[i], b->blshifta[i],
                            pre.blshiftb[i], b->blshiftb[i], pre.mem[0][i], b->mem[0][i], pre.mem[1][i], b->mem[1][i], pre.mem[2][i], b->mem[2][i], pre.mem[3][i], b->mem[3][i]);
                    break;
                }
        }
    }
    if (need_any) {
        int viol = 0;
        if (od_mutate) for (int i = 0; i < n; i++) if (wb[i]) { prov[i] = 1; break; }
        for (int i = 0; i < n; i++) viol |= prov[i] && wb[i];
        c->prov_checked++; c->prov_violation += viol != 0;
    }
    if (om->z_compare_en) {
        if (!need_any) for (int i = 0; i < n; i++) prov[i] = !wz[i];
        int pn = 0, pg_alive = 0;
        for (int g = 0; g < ng; g++) {
            int any = 0;
            for (int i = g * 8; i < g * 8 + 8 && i < n; i++) any |= !prov[i];
            pg_alive += any;
        }
        for (int i = 0; i < n; i++) pn += !prov[i];
        c->prov += (uint64_t)(n - pn);
        { uint32_t id = r->stat_prims & 8191; od.pl[id] += (uint32_t)n; od.pd[id] += (uint32_t)(n - pn); if (!ez_state) od.pz[id] += (uint32_t)n; }
        c->t_early += need_any ? (double)te : 0;
        if (pn == 0) { c->sv1 += tc; c->t_zc_dead += tt[2]; }
        c->sv2 += tc * (double)(ng - pg_alive) / (double)ng;
        c->sv3 += tc * (double)(ng - (pn + 7) / 8) / (double)ng;
    }

    /* per-lane attribution of the stage cost */
    const double cpre = (tt[0] + tt[1] + tt[2]) / (double)n;
    const double cbl = nz ? tt[3] / (double)nz : 0, cst = nw ? tt[4] / (double)nw : 0;
    const int ac = om->alpha_compare_en != 0, zu = om->z_update_en != 0;
    const int grp = om->force_blend ? 0 : ac ? 1 : 2;
    const int cd_ok = om->cvg_dest == CVG_CLAMP || om->cvg_dest == CVG_ZAP;
    im->proc += (uint64_t)n; im->zfail += (uint64_t)(n - nz); im->late += (uint64_t)(nz - nw); im->written += (uint64_t)nw;
    for (int i = 0; i < n; i++) {
        size_t key = od_key(r, b->fbidx[i]);
        struct od_ent *e = &od.sh[key];
        if (key < im->kmin) im->kmin = (uint32_t)key;
        if (key > im->kmax) im->kmax = (uint32_t)key;
        if (e->np < 65535) e->np++;
        if (e->ng[grp] < 255) e->ng[grp]++;
        if (!wz[i] && e->nzf < 255) e->nzf++;
        if (!wz[i]) { im->cost[OC_ZFAIL] += cpre; continue; }
        if (!wb[i]) { im->cost[OC_LATE] += cpre + cbl; continue; }
        const double cost = cpre + cbl + cst;
        const int repl = b->prewrap[i] && cd_ok && !od_lane_reads_mem(r, b, i, wz);
        if (e->nw < 65535) e->nw++;
        if (repl) {
            int kill_mask = zu ? 0xf : 0x5;     /* a replacing write that does not update Z leaves the Z left by earlier writes in force */
            im->replacing++;
            for (int bk = 0; bk < 4; bk++) {
                if (!((kill_mask >> bk) & 1) || !e->cnt[bk]) continue;
                im->dead += e->cnt[bk]; im->cost[OC_DEAD] += (double)e->cost[bk];
                if (bk & 1) { im->dead_ac += e->cnt[bk]; im->cost_dead_ac += (double)e->cost[bk]; }
                else if (!ac && !om->cvg_times_alpha) { im->dead_pre += e->cnt[bk]; im->cost_dead_pre += (double)e->cost[bk]; }
                e->cnt[bk] = 0; e->cost[bk] = 0;
            }
        }
        int bk = zu * 2 + ac;
        if (e->cnt[bk] < 255) e->cnt[bk]++;
        e->cost[bk] += (float)cost;
    }
    b->all_full = 1;
}

/* a FILL / COPY row replaces the colour of its pixels */
static void od_kill_row(struct rdp *r, int y, int x0, int x1)
{
    struct od_img *im = od_find_img(r);
    for (int x = x0; x <= x1; x++) {
        size_t key = od_key(r, (uint32_t)(r->fb_width * y + x));
        struct od_ent *e = &od.sh[key];
        if (key < im->kmin) im->kmin = (uint32_t)key;
        if (key > im->kmax) im->kmax = (uint32_t)key;
        for (int bk = 0; bk < 4; bk += 2) {   /* writes that did not update Z */
            if (!e->cnt[bk]) continue;
            im->dead += e->cnt[bk]; im->cost[OC_DEAD] += (double)e->cost[bk];
            e->cnt[bk] = 0; e->cost[bk] = 0;
        }
    }
}

void od_fill(struct rdp *r, int y, int x0, int x1)
{
    od_kill_row(r, y, x0, x1);
    od.fill_px += (uint64_t)(x1 - x0 + 1);
    od_find_img(r)->fill += (uint64_t)(x1 - x0 + 1);
}

void od_copy(struct rdp *r, int y, const struct span *sp)
{
    int x0 = sp->lx < sp->rx ? sp->lx : sp->rx, x1 = sp->lx < sp->rx ? sp->rx : sp->lx;
    if (x0 < 0) x0 = 0;
    if (x1 >= r->fb_width) x1 = r->fb_width - 1;
    if (x1 < x0) return;
    od_kill_row(r, y, x0, x1);
    od.copy_px += (uint64_t)(x1 - x0 + 1);
    od_find_img(r)->copy += (uint64_t)(x1 - x0 + 1);
}

static int cls_cmp(const void *a, const void *b)
{
    const struct od_cls *x = a, *y = b;
    return x->lanes < y->lanes ? 1 : x->lanes > y->lanes ? -1 : 0;
}

void od_report(FILE *f)
{
    const double ms = 1000.0 / od.ticks_per_s;
    /* survivors */
    for (int i = 0; i < od.nimg; i++) {
        struct od_img *im = &od.img[i];
        if (im->kmin == ~0u) continue;
        uint64_t covered = 0, hist[34] = { 0 }, whist[34] = { 0 }, live = 0;
        for (uint32_t k = im->kmin; k <= im->kmax; k++) {
            struct od_ent *e = &od.sh[k];
            for (int bk = 0; bk < 4; bk++) { live += e->cnt[bk]; im->cost[OC_SURV] += (double)e->cost[bk]; }
            if (e->np) { covered++; hist[e->np > 32 ? 33 : e->np]++; }
            if (e->nw) whist[e->nw > 32 ? 33 : e->nw]++;
        }
        uint32_t o = od.vi_origin & 0xffffff;
        fprintf(f, "OD_IMG addr=0x%06x size=%d width=%d vi=%d proc=%llu zfail=%llu late=%llu written=%llu replacing=%llu dead=%llu surv=%llu fill=%llu copy=%llu covered=%llu",
                im->addr, im->size, im->width, o >= im->addr && o < im->addr + (uint32_t)im->width * 2u * 240u && im->size == PIXEL_SIZE_16BIT,
                (unsigned long long)im->proc, (unsigned long long)im->zfail, (unsigned long long)im->late, (unsigned long long)im->written,
                (unsigned long long)im->replacing, (unsigned long long)im->dead, (unsigned long long)live, (unsigned long long)im->fill,
                (unsigned long long)im->copy, (unsigned long long)covered);
        fprintf(f, " dead_ac=%llu dead_pre=%llu cost_ms zfail=%.4f late=%.4f dead=%.4f dead_ac=%.4f dead_pre=%.4f surv=%.4f\n", (unsigned long long)im->dead_ac, (unsigned long long)im->dead_pre, im->cost[OC_ZFAIL] * ms, im->cost[OC_LATE] * ms, im->cost[OC_DEAD] * ms, im->cost_dead_ac * ms, im->cost_dead_pre * ms, im->cost[OC_SURV] * ms);
        fprintf(f, "OD_HIST_PROC addr=0x%06x", im->addr);
        for (int k = 1; k < 34; k++) fprintf(f, " %llu", (unsigned long long)hist[k]);
        fprintf(f, "\nOD_HIST_WRITTEN addr=0x%06x", im->addr);
        for (int k = 1; k < 34; k++) fprintf(f, " %llu", (unsigned long long)whist[k]);
        fprintf(f, "\n");
    }
    {   /* FOIR_OD_HEAT=<prefix>: per-pixel planes of the busiest colour image (total depth, depth per state group XLU / alpha compare / other, Z fails), 8 bit each, width x 240 */
        const char *hp = getenv("FOIR_OD_HEAT");
        struct od_img *bi = NULL;
        for (int i = 0; i < od.nimg; i++) if (!bi || od.img[i].proc > bi->proc) bi = &od.img[i];
        if (hp && bi && bi->proc && bi->size == PIXEL_SIZE_16BIT) {
            char path[512]; snprintf(path, sizeof path, "%s.%dx240.planes", hp, bi->width);
            FILE *o = fopen(path, "wb");
            if (o) {
                size_t npx = (size_t)bi->width * 240, base = (size_t)bi->addr >> 1;
                for (int pl = 0; pl < 5; pl++)
                    for (size_t k = 0; k < npx; k++) {
                        struct od_ent *e = &od.sh[base + k];
                        unsigned v = pl == 0 ? e->np : pl <= 3 ? e->ng[pl - 1] : e->nzf;
                        fputc(v > 255 ? 255 : (int)v, o);
                    }
                fclose(o);
            }
        }
    }
    qsort(od.cls, (size_t)od.ncls, sizeof od.cls[0], cls_cmp);
    double tot[5] = { 0 };
    for (int i = 0; i < od.ncls; i++) {
        struct od_cls *c = &od.cls[i];
        for (int s = 0; s < 5; s++) tot[s] += c->t[s];
        fprintf(f, "OD_CLS %s ez=%d batches=%llu lanes=%llu zfail=%llu late=%llu written=%llu groups=%llu alive=%llu compact=%llu deadbatches=%llu prov=%llu prims=%llu"
                   " ezchk=%llu ezbad=%llu provchk=%llu provbad=%llu ms t_zcdead=%.4f t_early=%.4f t_tex=%.4f t_comb=%.4f t_zcmp=%.4f t_blend=%.4f t_store=%.4f sv_batch=%.4f sv_group=%.4f sv_compact=%.4f\n",
                c->desc, c->ez_ok, (unsigned long long)c->batches, (unsigned long long)c->lanes, (unsigned long long)c->zfail, (unsigned long long)c->late,
                (unsigned long long)c->written, (unsigned long long)c->groups, (unsigned long long)c->groups_alive, (unsigned long long)c->groups_compact,
                (unsigned long long)c->dead_batches, (unsigned long long)c->prov, (unsigned long long)c->nprim, (unsigned long long)c->ez_checked, (unsigned long long)c->ez_mismatch, (unsigned long long)c->prov_checked, (unsigned long long)c->prov_violation, c->t_zc_dead * ms, c->t_early * ms, c->t[0] * ms, c->t[1] * ms, c->t[2] * ms, c->t[3] * ms, c->t[4] * ms, c->sv1 * ms, c->sv2 * ms, c->sv3 * ms);
    }
    fprintf(f, "OD_TOT batches=%llu fill_px=%llu copy_px=%llu ms t_tex=%.4f t_comb=%.4f t_zcmp=%.4f t_blend=%.4f t_store=%.4f (timer %.1f MHz, read overhead %.3f ticks)\n",
            (unsigned long long)od.nbatch, (unsigned long long)od.fill_px, (unsigned long long)od.copy_px, tot[0] * ms, tot[1] * ms, tot[2] * ms, tot[3] * ms, tot[4] * ms,
            od.ticks_per_s / 1e6, od.ovh);
    {   /* primitives that are (almost) completely hidden: what a primitive-level Z test could reject before the batch builder runs */
        uint64_t np = 0, d100 = 0, d95 = 0, l100 = 0, l95 = 0, lt = 0;
        for (int i = 0; i < 8192; i++) {
            if (!od.pl[i]) continue;
            np++; lt += od.pl[i];
            if (od.pd[i] == od.pl[i]) { d100++; l100 += od.pl[i]; }
            if (od.pd[i] * 100u >= od.pl[i] * 95u) { d95++; l95 += od.pl[i]; }
        }
        fprintf(f, "OD_PRIM prims=%llu dead100=%llu lanes_dead100=%llu dead95=%llu lanes_dead95=%llu lanes=%llu\n", (unsigned long long)np, (unsigned long long)d100,
                (unsigned long long)l100, (unsigned long long)d95, (unsigned long long)l95, (unsigned long long)lt);
    }
    fprintf(f, "OD_BATCHN");
    for (int i = 0; i < 9; i++) fprintf(f, " %llu", (unsigned long long)od.hist_batch_n[i]);
    fprintf(f, "\n");
}
#endif
