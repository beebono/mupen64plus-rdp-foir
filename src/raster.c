/*
 * mupen64plus-rdp-foir - raster.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mt.h"
#include "spec.h"
#include "walk.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

static struct { uint8_t cvg, cvbit, xoff, yoff; } cvarray[256];
#if defined(FOIR_NEON)
/* indexed by subscanline * 8 + (x & 7) */
static uint8_t cvg_ltab[32] ALIGN16, cvg_rtab[32] ALIGN16;
#endif

static uint16_t decompress_cvmask_frombyte(uint8_t x)
{
    return (uint16_t)((x & 0x5) | ((x & 0x5a) << 4) | ((x & 0xa0) << 8));
}

static uint32_t rightcvghex(uint32_t x, uint32_t fmask)
{
    uint32_t covered = ((x & 7) + 1) >> 1;
    covered = 0xf0 >> covered;
    return covered & fmask;
}

static uint32_t leftcvghex(uint32_t x, uint32_t fmask)
{
    uint32_t covered = ((x & 7) + 1) >> 1;
    covered = 0xf >> covered;
    return covered & fmask;
}

void raster_init_tables(void)
{
    static bool done;
    if (done) return;
    kernels_scalar_init_tables();
    static const uint8_t yarray[16] = { 0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0 };
    static const uint8_t xarray[16] = { 0, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 256; i++) {
        uint16_t mask = decompress_cvmask_frombyte((uint8_t)i);
        cvarray[i].cvg = 0;
        cvarray[i].cvbit = (i >> 7) & 1;
        for (int k = 0; k < 8; k++) cvarray[i].cvg += (i >> k) & 1;
        uint16_t masky = 0;
        for (int k = 0; k < 4; k++) masky |= ((mask & (0xf000 >> (k << 2))) > 0) << k;
        uint8_t offy = yarray[masky];
        uint16_t maskx = (mask & (0xf000 >> (offy << 2))) >> ((offy ^ 3) << 2);
        cvarray[i].xoff = xarray[maskx];
        cvarray[i].yoff = offy;
    }
#if defined(FOIR_NEON)
    for (int i = 0; i < 4; i++) {
        const uint32_t fmask = 0xa >> (i & 1), shift = (uint32_t)((i - 2) & 4);
        for (int x = 0; x < 8; x++) {
            cvg_ltab[i * 8 + x] = (uint8_t)(leftcvghex((uint32_t)x, fmask) << shift);
            cvg_rtab[i * 8 + x] = (uint8_t)(rightcvghex((uint32_t)x, fmask) << shift);
        }
    }
#endif
    done = true;
}

static void compute_cvg(struct rdp *r, const struct span *sp, int flip)
{
    uint8_t *cvg = r->cvgbuf;
    int purgestart = flip ? sp->rx : sp->lx;
    int purgeend = flip ? sp->lx : sp->rx;
    int length = purgeend - purgestart;
    if (length < 0) return;

    memset(&cvg[purgestart], 0xff, (size_t)length + 1);
    for (int i = 0; i < 4; i++) {
        int fmask = 0xa >> (i & 1);
        int maskshift = (i - 2) & 4;
        int fmaskshifted = fmask << maskshift;

        if (!sp->invalyscan[i]) {
            int minorcur = sp->minorx[i], majorcur = sp->majorx[i];
            int minorcurint = minorcur >> 3, majorcurint = majorcur >> 3;
            if (!flip) {
                for (int k = purgestart; k <= minorcurint; k++) cvg[k] &= ~fmaskshifted;
                for (int k = majorcurint; k <= purgeend; k++) cvg[k] &= ~fmaskshifted;
                if (majorcurint > minorcurint) {
                    cvg[minorcurint] |= (leftcvghex(minorcur, fmask) << maskshift);
                    cvg[majorcurint] |= (rightcvghex(majorcur, fmask) << maskshift);
                } else if (minorcurint == majorcurint) {
                    int same = leftcvghex(minorcur, fmask) & rightcvghex(majorcur, fmask);
                    cvg[majorcurint] |= (same << maskshift);
                }
            } else {
                for (int k = purgestart; k <= majorcurint; k++) cvg[k] &= ~fmaskshifted;
                for (int k = minorcurint; k <= purgeend; k++) cvg[k] &= ~fmaskshifted;
                if (minorcurint > majorcurint) {
                    cvg[minorcurint] |= (rightcvghex(minorcur, fmask) << maskshift);
                    cvg[majorcurint] |= (leftcvghex(majorcur, fmask) << maskshift);
                } else if (minorcurint == majorcurint) {
                    int same = rightcvghex(minorcur, fmask) & leftcvghex(majorcur, fmask);
                    cvg[majorcurint] |= (same << maskshift);
                }
            }
        } else {
            for (int k = purgestart; k <= purgeend; k++) cvg[k] &= ~fmaskshifted;
        }
    }
}

#ifdef FOIR_STAGETIME
static inline uint64_t stage_tick(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
#define ST_LAP(i) do { const uint64_t t_ = stage_tick(); r->st_tk[i] += t_ - st_t; st_t = t_; } while (0)
#define ST_TEX() do { r->st_n[0] += r->pf.need_tex; ST_LAP(0); } while (0)
#define ST_COMB() do {                                                                                                         \
        const int kind_ = !(r->spec_mask & SPEC_BIT(SPEC_COMB)) ? 2 : r->spec_comb_shape ? 1 : 0;                              \
        const uint64_t t_ = stage_tick();                                                                                      \
        r->st_tk[5 + kind_] += t_ - st_t; st_t = t_; r->st_n[5 + kind_]++; r->st_cpix[kind_] += (uint64_t)b->n;                \
    } while (0)
#else
#define ST_LAP(i) ((void)0)
#define ST_TEX() ((void)0)
#define ST_COMB() ((void)0)
#endif

/* any lane of the first b->n still write-enabled (lanes past n are not written by every kernel) */
static inline bool batch_has_live_lane(const struct px_batch *b)
{
    uint64_t acc = 0;
    int i = 0;
    for (; i + 8 <= b->n; i += 8) { uint64_t w; memcpy(&w, &b->wen[i], sizeof w); acc |= w; }
    for (; i < b->n; i++) acc |= b->wen[i];
    return acc != 0;
}

/* Pixel stages of one batch: texture -> combine -> zcmp -> blend -> store.
 * Early Z (pf.early_z, see rdp_update_derived): when the Z compare does not read the combiner's output it runs first, and a batch with no live lane (every
 * lane failed Z) skips texture, combine, blend and store. Exact: texture and combine only write texel, colour and dither arrays of the batch, the Z compare
 * only reads coverage / Z / the buffers, and blend and store already ignore lanes with wen = 0, so the live lanes see the same inputs in either order. */
static void batch_flush(struct rdp *r, struct px_batch *b)
{
    if (b->n <= 0) return;
    const struct foir_kernels *k = r->k;
#ifdef FOIR_STATS
    foir_stats_batch(r, b->n);
#endif
#ifdef FOIR_OVERDRAW
    od_stages(r, b);       /* measurement build: always the original order */
#else
#ifdef FOIR_STAGETIME
    uint64_t st_t = stage_tick();
#endif
    if (r->pf.early_z) {
        k->zcmp(r, b);
        ST_LAP(2);
        if (batch_has_live_lane(b)) {
            if (r->pf.need_tex) k->texture(r, b);
            ST_TEX();
            k->combine(r, b);
            ST_COMB();
            k->blend(r, b);
            ST_LAP(3);
            k->store(r, b);
            ST_LAP(4);
        }
    } else {
        if (r->pf.need_tex) k->texture(r, b);
        ST_TEX();
        k->combine(r, b);
        ST_COMB();
        k->zcmp(r, b);
        ST_LAP(2);
        k->blend(r, b);
        ST_LAP(3);
        k->store(r, b);
        ST_LAP(4);
    }
#endif
    r->stat_pixels += (uint32_t)b->n;
    r->cov_pix[5] += (uint64_t)b->n;
    if (r->pf.need_tex) r->cov_tex_all += (uint64_t)b->n;
    for (unsigned m = r->spec_mask; m; m &= m - 1) r->cov_pix[__builtin_ctz(m)] += (uint64_t)b->n;
    if (r->spec_comb_shape && (r->spec_mask & SPEC_BIT(SPEC_COMB))) r->cov_pix[6] += (uint64_t)b->n;
    if (r->spec_tex_any && (r->spec_mask & SPEC_BIT(SPEC_TEX)) && r->pf.need_tex) r->cov_pix[7] += (uint64_t)b->n;
    b->n = 0;
    b->all_full = 1;
    b->fbmin = ~0u; b->fbmax = 0;
}

/* portable span builder: the reference of the NEON builder (tools/kfuzz.c checks the two produce identical batches) */
static void fill_ramp_c(int32_t *dst, int32_t base, int32_t step, int n)
{
    for (int i = 0; i < n; i++) dst[i] = base + step * i;
}

static void fill_idx_c(struct px_batch *b, int pos, uint32_t idx0, uint8_t dity, int x0, int n)
{
    for (int i = 0; i < n; i++) {
        b->fbidx[pos + i] = idx0 + (uint32_t)i;
        b->ditidx[pos + i] = (uint8_t)(dity | ((x0 + i) & 3));
        b->xpos[pos + i] = (int16_t)(x0 + i);
    }
}

static void fill_cvg_c(struct px_batch *b, int pos, const uint8_t *m, int n)
{
    int full = 1;
    for (int i = 0; i < n; i++) {
        uint8_t c = m[i];
        b->cvg[pos + i] = cvarray[c].cvg;
        b->cvbit[pos + i] = cvarray[c].cvbit;
        b->offx[pos + i] = cvarray[c].xoff;
        b->offy[pos + i] = cvarray[c].yoff;
        full &= (c == 0xff);
    }
    if (!full) b->all_full = 0;
}

static void render_span_ref(struct rdp *r, int y, const struct span *sp)
{
    struct prim_ctx *pc = &r->pc;
    struct px_batch *b = &r->batch;
    int flip = pc->flip;
    const struct pipe_flags *pf = &r->pf;

    int xstart = sp->lx, xend = sp->unscrx, xendsc = sp->rx;
    int length, scdiff;
    if (!flip) { length = xendsc - xstart; scdiff = xend - xendsc; }
    else       { length = xstart - xendsc; scdiff = xendsc - xend; }
    if (length < 0) return;
    compute_cvg(r, sp, flip);

    /* the hardware iterates from xendsc; pixels are stored in ascending x, so every interpolant advances by +d from the left end */
    int32_t rr = sp->r, gg = sp->g, bb = sp->b, aa = sp->a;
    int32_t zz = r->om.z_source_sel ? (int32_t)r->primitive_z : sp->z;
    int32_t ss = sp->s, tt = sp->t, ww = sp->w;

    if (scdiff) {
        scdiff &= 0xfff;
        rr += pc->drinc * scdiff; gg += pc->dginc * scdiff; bb += pc->dbinc * scdiff; aa += pc->dainc * scdiff;
        zz += pc->dzinc * scdiff;
        ss += pc->dsinc * scdiff; tt += pc->dtinc * scdiff; ww += pc->dwinc * scdiff;
    }
    if (!flip) {
        int32_t L = length;
        rr -= pc->dr * L; gg -= pc->dg * L; bb -= pc->db * L; aa -= pc->da * L;
        zz -= pc->dz * L;
        ss -= pc->ds * L; tt -= pc->dt * L; ww -= pc->dw * L;
    }
    int xlow = flip ? xendsc : xstart;
    int total = length + 1;
    if (xlow + total > r->fb_width) total = r->fb_width - xlow;   /* pixels past the row end would land in the next row */
    if (total <= 0) return;
    uint32_t fbrow = (uint32_t)(r->fb_width * y);
    uint8_t dity = (uint8_t)(((y >> r->scfield) & 3) << 2);

    int done = 0;
    while (done < total) {
        int pos = b->n;
        int take = total - done;
        if (take > FOIR_BATCH - pos) take = FOIR_BATCH - pos;
        int x0 = xlow + done;

        fill_idx_c(b, pos, fbrow + (uint32_t)x0, dity, x0, take);
        {
            const uint32_t lo = fbrow + (uint32_t)x0, hi = lo + (uint32_t)take - 1;
            if (lo < b->fbmin) b->fbmin = lo;
            if (hi > b->fbmax) b->fbmax = hi;
        }
        if (pf->shade_rgb) {
            fill_ramp_c(&b->cr[pos], rr + pc->dr * done, pc->dr, take);
            fill_ramp_c(&b->cg[pos], gg + pc->dg * done, pc->dg, take);
            fill_ramp_c(&b->cb[pos], bb + pc->db * done, pc->db, take);
        }
        if (pf->shade_a || pf->shade_rgb)
            fill_ramp_c(&b->ca[pos], aa + pc->da * done, pc->da, take);
        if (pf->need_cz)
            fill_ramp_c(&b->cz[pos], zz + pc->dz * done, pc->dz, take);
        if (pf->need_tex) {
            fill_ramp_c(&b->s[pos], ss + pc->ds * done, pc->ds, take);
            fill_ramp_c(&b->t[pos], tt + pc->dt * done, pc->dt, take);
            fill_ramp_c(&b->w[pos], ww + pc->dw * done, pc->dw, take);
        }
        fill_cvg_c(b, pos, &r->cvgbuf[(x0) & 0x3ff], take);

        b->n = pos + take;
        done += take;
        if (b->n >= FOIR_BATCH) batch_flush(r, b);
    }
}

#if defined(FOIR_NEON)

/* zcmp / store touch these lines a whole texture + combine pass later; prefetching early hides the misses (the Cortex-A55 does not reorder) */
#ifndef FOIR_PREFETCH
#define FOIR_PREFETCH 3
#endif

#if FOIR_PREFETCH
static void prefetch_piece(const struct rdp *r, uint32_t idx0, int take)
{
    const uint32_t pix = (uint32_t)take, bpp = r->fb_size == PIXEL_SIZE_32BIT ? 4u : r->fb_size == PIXEL_SIZE_16BIT ? 2u : 1u;
    uint32_t a = (r->fb_address + bpp * idx0) & RDRAM_MASK;
    for (uint32_t o = a & ~63u; o < a + bpp * pix; o += 64) __builtin_prefetch(r->rdram + o, 1, FOIR_PREFETCH);
    if (bpp == 2) {
        const uint8_t *hf = r->hidden + (((r->fb_address >> 1) + idx0) & (RDRAM_MASK >> 1));
        for (uint32_t o = 0; o < pix + 63; o += 64) __builtin_prefetch(hf + o, 1, FOIR_PREFETCH);
    }
    if (r->pf.need_z) {
        uint32_t z = (r->zb_address + 2u * idx0) & RDRAM_MASK;
        for (uint32_t o = z & ~63u; o < z + 2u * pix; o += 64) __builtin_prefetch(r->rdram + o, 1, FOIR_PREFETCH);
        const uint8_t *hz = r->hidden + (((r->zb_address >> 1) + idx0) & (RDRAM_MASK >> 1));
        for (uint32_t o = 0; o < pix + 63; o += 64) __builtin_prefetch(hz + o, 1, FOIR_PREFETCH);
    }
}
#endif

struct span_vec { int32x4_t dI[8], d4[8]; };

static void span_vec_init(struct span_vec *sv, const struct prim_ctx *pc)
{
    static const int32_t lane[4] = { 0, 1, 2, 3 };
    const int32x4_t iota = vld1q_s32(lane);
    const int32_t d[8] = { pc->dr, pc->dg, pc->db, pc->da, pc->ds, pc->dt, pc->dw, pc->dz };
    for (int k = 0; k < 8; k++) {
        sv->dI[k] = vmulq_n_s32(iota, d[k]);
        sv->d4[k] = vdupq_n_s32(d[k] * 4);
    }
}

/* Subscanline i covers pixels le_i < x < re_i fully and le_i, re_i partially (leftcvghex / rightcvghex); le / re are minorx / majorx, swapped when flipped.
 * Mask of pixel x = A_i(x) & B_i(x), A_i = x > le_i ? F_i : x == le_i ? L_i : 0, B_i likewise with re_i / R_i (F_i: the subscanline's bits of the 8-bit mask);
 * right for le_i == re_i (L & R) and le_i > re_i (nothing). Pixels right of all le_i and left of all re_i share one mask, so only the edge zones are evaluated. */
struct cvg_edges {
    int32x4_t lei, rei;
    uint32x4_t fm, lh, rh;        /* F_i, L_i, R_i of the edge pixels (0 for invalid subscanlines) */
    uint32_t v;                   /* mask of the pixels between the edge zones */
    int nl, rs;                   /* span-relative: left zone [0, nl), right zone [rs, ...), interior in between (nl <= rs) */
};

FOIR_INLINE void cvg_edges_init(struct cvg_edges *e, const struct span *sp, int flip, int lo)
{
    const int32_t *lp = flip ? sp->majorx : sp->minorx, *rp = flip ? sp->minorx : sp->majorx;
    const uint32x4_t lx = vreinterpretq_u32_s32(vld1q_s32(lp)), rx = vreinterpretq_u32_s32(vld1q_s32(rp));
    const uint32x4_t iv = vreinterpretq_u32_s32(vld1q_s32(sp->invalyscan));
    const uint32x4_t inv = vtstq_u32(iv, iv);
    static const uint32_t lane8[4] = { 0, 8, 16, 24 }, fmv[4] = { 0xa0, 0x50, 0x0a, 0x05 };
    const uint32x4_t seven = vdupq_n_u32(7);
    const uint32x4_t ho = vorrq_u32(vdupq_n_u32(0xffffff00u), inv);            /* table index: byte 0 = (x & 7) + lane * 8, 0xff = none */
    const uint32x4_t lane = vld1q_u32(lane8);
    const uint8x16x2_t lt = vld1q_u8_x2(cvg_ltab), rt = vld1q_u8_x2(cvg_rtab);
    e->lh = vreinterpretq_u32_u8(vqtbl2q_u8(lt, vreinterpretq_u8_u32(vorrq_u32(vaddq_u32(vandq_u32(lx, seven), lane), ho))));
    e->rh = vreinterpretq_u32_u8(vqtbl2q_u8(rt, vreinterpretq_u8_u32(vorrq_u32(vaddq_u32(vandq_u32(rx, seven), lane), ho))));
    e->fm = vbicq_u32(vld1q_u32(fmv), inv);
    const uint32x4_t li = vshrq_n_u32(lx, 3), ri = vshrq_n_u32(rx, 3);
    e->lei = vreinterpretq_s32_u32(li);
    e->rei = vreinterpretq_s32_u32(ri);
    e->v = vaddvq_u32(e->fm);
    const int maxle = (int)vmaxvq_u32(vbicq_u32(li, inv)), minre = (int)vminvq_u32(vorrq_u32(ri, inv));
    const int nl = maxle - lo + 1, rs = minre - lo;
    e->nl = nl;
    e->rs = rs < nl ? nl : rs;
}

FOIR_INLINE uint32_t cvg_mask_at(const struct cvg_edges *e, int x)
{
    const int32x4_t xv = vdupq_n_s32(x);
    const uint32x4_t a = vbslq_u32(vcgtq_s32(xv, e->lei), e->fm, vandq_u32(vceqq_s32(xv, e->lei), e->lh));
    const uint32x4_t bm = vbslq_u32(vcgtq_s32(e->rei, xv), e->fm, vandq_u32(vceqq_s32(xv, e->rei), e->rh));
    return vaddvq_u32(vandq_u32(a, bm));
}

FOIR_INLINE int cvg_patch(struct px_batch *b, const struct cvg_edges *e, int lo, int p, int lane)
{
    const uint32_t m = cvg_mask_at(e, lo + p);
    b->cvg[lane] = cvarray[m].cvg;
    b->cvbit[lane] = cvarray[m].cvbit;
    b->offx[lane] = cvarray[m].xoff;
    b->offy[lane] = cvarray[m].yoff;
    return m == 0xff;
}

/* The batch arrays have 16 lanes of slack and lanes past the piece are ignored, so ramps run in groups of four lanes and byte arrays are stored as whole
 * 16-lane vectors. M is a constant selecting the channels (1 shade rgb + alpha, 2 shade alpha only, 4 Z, 8 texture coordinates): straight-line code per instance. */
struct cv_const { uint8x16_t cvg, cvbit, offx, offy; };

FOIR_INLINE void piece_fill(struct px_batch *b, int pos, int take, uint32_t idx0, int x0, uint8_t dity, const struct span_vec *sv,
                            int32x4_t c_lo, int32x4_t c_hi, const struct cv_const *kc, const int M)
{
    static const uint32_t lane4[4] = { 0, 1, 2, 3 };
    static const uint8_t pat[32] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 };
    static const int16_t lane8[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const int RGB = M & 1, A = (M & 3) != 0, Z = (M & 4) != 0, T = (M & 8) != 0;
    const uint32x4_t c4 = vdupq_n_u32(4);
    const int16x8_t c8 = vdupq_n_s16(8);
    uint32x4_t iv = vaddq_u32(vdupq_n_u32(idx0), vld1q_u32(lane4));
    int16x8_t xv = vaddq_s16(vdupq_n_s16((int16_t)x0), vld1q_s16(lane8));
    const uint8x16_t dit = vorrq_u8(vld1q_u8(pat + (x0 & 3)), vdupq_n_u8(dity));
    int32x4_t vr = vdupq_n_s32(0), vg = vr, vbl = vr, va = vr, vz = vr, vs = vr, vt = vr, vw = vr;
    if (RGB) {
        vr = vaddq_s32(vdupq_laneq_s32(c_lo, 0), sv->dI[0]);
        vg = vaddq_s32(vdupq_laneq_s32(c_lo, 1), sv->dI[1]);
        vbl = vaddq_s32(vdupq_laneq_s32(c_lo, 2), sv->dI[2]);
    }
    if (A) va = vaddq_s32(vdupq_laneq_s32(c_lo, 3), sv->dI[3]);
    if (Z) vz = vaddq_s32(vdupq_laneq_s32(c_hi, 3), sv->dI[7]);
    if (T) {
        vs = vaddq_s32(vdupq_laneq_s32(c_hi, 0), sv->dI[4]);
        vt = vaddq_s32(vdupq_laneq_s32(c_hi, 1), sv->dI[5]);
        vw = vaddq_s32(vdupq_laneq_s32(c_hi, 2), sv->dI[6]);
    }
#define PF_STEP() do { \
        vst1q_u32(&b->fbidx[q], iv); \
        if (RGB) { vst1q_s32(&b->cr[q], vr); vst1q_s32(&b->cg[q], vg); vst1q_s32(&b->cb[q], vbl); } \
        if (A) vst1q_s32(&b->ca[q], va); \
        if (Z) vst1q_s32(&b->cz[q], vz); \
        if (T) { vst1q_s32(&b->s[q], vs); vst1q_s32(&b->t[q], vt); vst1q_s32(&b->w[q], vw); } \
    } while (0)
#define PF_ADV() do { \
        iv = vaddq_u32(iv, c4); \
        if (RGB) { vr = vaddq_s32(vr, sv->d4[0]); vg = vaddq_s32(vg, sv->d4[1]); vbl = vaddq_s32(vbl, sv->d4[2]); } \
        if (A) va = vaddq_s32(va, sv->d4[3]); \
        if (Z) vz = vaddq_s32(vz, sv->d4[7]); \
        if (T) { vs = vaddq_s32(vs, sv->d4[4]); vt = vaddq_s32(vt, sv->d4[5]); vw = vaddq_s32(vw, sv->d4[6]); } \
    } while (0)
    for (int i = 0; i < take; i += 4) {
        const int q = pos + i;
        PF_STEP();  PF_ADV();
    }
    for (int i = 0; i < take; i += 16) {
        const int q = pos + i;
        vst1q_u8(&b->ditidx[q], dit);
        vst1q_s16(&b->xpos[q], xv);                     xv = vaddq_s16(xv, c8);
        vst1q_s16(&b->xpos[q + 8], xv);                 xv = vaddq_s16(xv, c8);
        vst1q_u8(&b->cvg[q], kc->cvg);
        vst1q_u8(&b->cvbit[q], kc->cvbit);
        vst1q_u8(&b->offx[q], kc->offx);
        vst1q_u8(&b->offy[q], kc->offy);
    }
#undef PF_STEP
#undef PF_ADV
}

static void render_span_cycle(struct rdp *r, int y, const struct span *sp, const struct span_vec *sv)
{
    struct prim_ctx *pc = &r->pc;
    struct px_batch *b = &r->batch;
    int flip = pc->flip;
    const struct pipe_flags *pf = &r->pf;

    int xstart = sp->lx, xend = sp->unscrx, xendsc = sp->rx;
    int length, scdiff;
    if (!flip) { length = xendsc - xstart; scdiff = xend - xendsc; }
    else       { length = xstart - xendsc; scdiff = xendsc - xend; }
    if (length < 0) return;

    /* the hardware iterates from xendsc; pixels are stored in ascending x, so every interpolant advances by +d from the left end */
    int32x4_t b_lo = vld1q_s32(&sp->r), b_hi = vld1q_s32(&sp->s);       /* r g b a | s t w z */
    if (r->om.z_source_sel) b_hi = vsetq_lane_s32((int32_t)r->primitive_z, b_hi, 3);
    if (scdiff) {
        int32_t sd = scdiff & 0xfff;
        b_lo = vmlaq_n_s32(b_lo, vld1q_s32(&pc->drinc), sd);
        b_hi = vmlaq_n_s32(b_hi, vld1q_s32(&pc->dsinc), sd);
    }
    if (!flip) {
        b_lo = vmlsq_n_s32(b_lo, vld1q_s32(&pc->dr), length);
        b_hi = vmlsq_n_s32(b_hi, vld1q_s32(&pc->ds), length);
    }
    int xlow = flip ? xendsc : xstart;
    int total = length + 1;
    if (xlow + total > r->fb_width) total = r->fb_width - xlow;   /* pixels past the row end would land in the next row */
    if (total <= 0) return;
    uint32_t fbrow = (uint32_t)(r->fb_width * y);
    uint8_t dity = (uint8_t)(((y >> r->scfield) & 3) << 2);
    const int32x4_t d_lo = vld1q_s32(&pc->dr), d_hi = vld1q_s32(&pc->ds);
    const int f_rgb = pf->shade_rgb, f_a = pf->shade_a || pf->shade_rgb;
    const int mask = (f_rgb ? 1 : f_a ? 2 : 0) | (pf->need_cz ? 4 : 0) | (pf->need_tex ? 8 : 0);

    struct cvg_edges e;
    cvg_edges_init(&e, sp, flip, xlow);
    const uint8x16x4_t kc4 = vld4q_dup_u8(&cvarray[e.v].cvg);
    const struct cv_const kcv = { kc4.val[0], kc4.val[1], kc4.val[2], kc4.val[3] };

    int done = 0;
    while (done < total) {
        int pos = b->n;
        int take = total - done;
        if (take > FOIR_BATCH - pos) take = FOIR_BATCH - pos;
        int x0 = xlow + done;
        const uint32_t idx0 = fbrow + (uint32_t)x0;

        {
            const uint32_t hi = idx0 + (uint32_t)take - 1;
            if (idx0 < b->fbmin) b->fbmin = idx0;
            if (hi > b->fbmax) b->fbmax = hi;
        }
#if FOIR_PREFETCH
        prefetch_piece(r, idx0, take);
#endif
        {
            int32x4_t c_lo = b_lo, c_hi = b_hi;
            if (mask) { c_lo = vmlaq_n_s32(b_lo, d_lo, done); c_hi = vmlaq_n_s32(b_hi, d_hi, done); }
            switch (mask) {
#define PC(m) case m: piece_fill(b, pos, take, idx0, x0, dity, sv, c_lo, c_hi, &kcv, m); break;
            PC(0) PC(1) PC(2) PC(4) PC(5) PC(6) PC(8) PC(9) PC(10) PC(12) PC(13) PC(14)
#undef PC
            default: __builtin_unreachable();
            }
        }
        {
            const int pe = done + take;
            int full = e.v == 0xff;
            int pa = done, pb = e.nl < pe ? e.nl : pe;
            for (; pa < pb; pa++) full &= cvg_patch(b, &e, xlow, pa, pos + pa - done);
            for (pa = e.rs > done ? e.rs : done; pa < pe; pa++) full &= cvg_patch(b, &e, xlow, pa, pos + pa - done);
            if (!full) b->all_full = 0;
        }

        b->n = pos + take;
        done += take;
        if (b->n >= FOIR_BATCH) batch_flush(r, b);
    }
}
#endif

static void prim_prepare(struct rdp *r)
{
    struct prim_ctx *pc = &r->pc;
    if (r->om.z_source_sel) {
        pc->dz = 0; pc->cdz = 0; pc->dzdy = 0;
        pc->dzpix = r->primitive_delta_z;
    }
    pc->dzpixenc = (int32_t)dz_compress((uint32_t)pc->dzpix & 0xffff);
    int f = pc->flip;
    pc->drinc = f ? pc->dr : -pc->dr; pc->dginc = f ? pc->dg : -pc->dg;
    pc->dbinc = f ? pc->db : -pc->db; pc->dainc = f ? pc->da : -pc->da;
    pc->dsinc = f ? pc->ds : -pc->ds; pc->dtinc = f ? pc->dt : -pc->dt;
    pc->dwinc = f ? pc->dw : -pc->dw; pc->dzinc = f ? pc->dz : -pc->dz;
}

static inline int row_first_owned(int y, int nw, int wid)
{
    int s = y >> FOIR_STRIPE_SHIFT, m = s % nw;
    if (m == wid) return y;
    int ds = wid - m;
    if (ds < 0) ds += nw;
    return (s + ds) << FOIR_STRIPE_SHIFT;
}

/* spans come from the setup record `pw`, or for the tests from the ready-made `rows` (pw == NULL) */
FOIR_INLINE void render_job(struct rdp *r, const struct span *rows, int jlo, const struct prim_walk *pw, int start, int end, int ref)
{
    const struct foir_kernels *k = r->k;
    struct px_batch *b = &r->batch;
    int ct = r->om.cycle_type;
    (void)ref;
    const int nw = r->nworkers > 1 ? r->nworkers : 1;
    const int wid = nw > 1 ? r->worker_id : 0;
    struct span row;

    /* stripes (y >> FOIR_STRIPE_SHIFT) with stripe % nw == wid; whole stripes owned by others are jumped over */
#define FIRST_ROW(y) ((nw == 1) ? (y) : row_first_owned((y), nw, wid))
#define NEXT_ROW_IDX(y) ((nw == 1 || (((y) + 1) & ((1 << FOIR_STRIPE_SHIFT) - 1))) ? (y) + 1 : (y) + 1 + (nw - 1) * (1 << FOIR_STRIPE_SHIFT))
#define NEXT_ROW() \
        const struct span *sp; \
        if (pw) { if (!walk_row(pw, y, &row)) continue; sp = &row; } \
        else { sp = &rows[y - jlo]; if (!sp->validline) continue; }

    if (ct == CYCLE_TYPE_FILL) {
        if (r->fb_size == PIXEL_SIZE_4BIT) return;
        for (int y = FIRST_ROW(start); y <= end; y = NEXT_ROW_IDX(y)) {
            NEXT_ROW();
            int x0 = sp->lx < sp->rx ? sp->lx : sp->rx;
            int x1 = sp->lx < sp->rx ? sp->rx : sp->lx;
            if (x1 >= r->fb_width) x1 = r->fb_width - 1;   /* no wrap into the next row (owned by another worker) */
            if (x1 < x0) continue;
            k->fill(r, y, x0, x1);
#ifdef FOIR_OVERDRAW
            od_fill(r, y, x0, x1);
#endif
            r->stat_pixels += (uint32_t)(x1 - x0 + 1);   /* FILL/COPY do not go through batch_flush */
        }
        return;
    }
    if (ct == CYCLE_TYPE_COPY) {
        if (r->fb_size == PIXEL_SIZE_32BIT) return;
        for (int y = FIRST_ROW(start); y <= end; y = NEXT_ROW_IDX(y)) {
            NEXT_ROW();
            k->copy(r, y, sp, r->pc.flip);
#ifdef FOIR_OVERDRAW
            od_copy(r, y, sp);
#endif
            r->stat_pixels += (uint32_t)(sp->lx < sp->rx ? sp->rx - sp->lx : sp->lx - sp->rx) + 1;
        }
        return;
    }
    if (r->spec_dirty || r->spec_tile != r->pc.tilenum) foir_spec_select(r);
    b->n = 0;
    b->all_full = 1;
    b->fbrange = 1; b->fbmin = ~0u; b->fbmax = 0;
#if defined(FOIR_NEON)
    struct span_vec sv;
    span_vec_init(&sv, &r->pc);
#endif
    for (int y = FIRST_ROW(start); y <= end; y = NEXT_ROW_IDX(y)) {
        NEXT_ROW();
#if defined(FOIR_NEON)
        if (ref) render_span_ref(r, y, sp);
        else render_span_cycle(r, y, sp, &sv);
#else
        render_span_ref(r, y, sp);
#endif
    }
#undef NEXT_ROW
#undef NEXT_ROW_IDX
#undef FIRST_ROW
    batch_flush(r, b);
}

void raster_render_walk(struct rdp *r, const struct prim_walk *pw, int start, int end)
{
    render_job(r, NULL, 0, pw, start, end, 0);
}

void raster_render_job(struct rdp *r, const struct span *rows, int jlo, int start, int end)
{
    render_job(r, rows, jlo, NULL, start, end, 0);
}

void raster_render_job_ref(struct rdp *r, const struct span *rows, int jlo, int start, int end)
{
    render_job(r, rows, jlo, NULL, start, end, 1);
}

void rdp_fb_history_reset(struct rdp *r)
{
    memset(r->fb_hist, 0, sizeof r->fb_hist);
    for (int i = 0; i < FOIR_FB_HIST; i++) r->fb_hist[i].addr = 0xffffffffu;
    memset(r->scan_addr, 0xff, sizeof r->scan_addr);
    r->fb_hist_last = r->fb_hist_next = 0;
    r->scan_next = 0;
    r->period = r->period_prims = 0;
    r->period_rendered = false;
    r->last_rendered = 0xffffffffu;
    r->good_addr = 0xffffffffu;
    r->vi_since = false;
}

static int fb_history_find(struct rdp *r)
{
    int i = r->fb_hist_last;
    if (r->fb_hist[i].addr != r->fb_address) {
        const int nh = r->cfg.skip_legacy ? 4 : FOIR_FB_HIST;   /* the legacy mode keeps the original four entries */
        for (i = 0; i < nh && r->fb_hist[i].addr != r->fb_address; i++) ;
        if (i == nh) {
            i = r->fb_hist_next;
            r->fb_hist_next = (i + 1) % nh;
            memset(&r->fb_hist[i], 0, sizeof r->fb_hist[i]);
            r->fb_hist[i].addr = r->fb_address;
        }
        r->fb_hist_last = i;
    }
    return i;
}

/* the VI origin may start a few lines into the buffer: a colour image at or below it, within 64 KB */
static bool scan_hit(const struct rdp *r, uint32_t addr)
{
    for (int i = 0; i < 8; i++)
        if (r->scan_addr[i] != 0xffffffffu && r->scan_addr[i] >= addr && r->scan_addr[i] - addr < 0x10000) return true;
    return false;
}

void rdp_note_scanout(struct rdp *r, uint32_t vi_origin)
{
    vi_origin &= 0xffffff;
    for (int i = 0; i < 8; i++) if (r->scan_addr[i] == vi_origin) return;
    r->scan_addr[r->scan_next] = vi_origin;
    r->scan_next = (r->scan_next + 1) & 7;
}

/* InterlaceHack applies to a buffer only while it is drawn in fields: other buffers (and this one once it is drawn whole) are shown as they are */
void rdp_fld_note(struct rdp *r, uint32_t addr, int width, int rowbytes, int odd)
{
    for (int i = 0; i < r->fld_n; i++)
        if (r->fld[i].addr == addr) { r->fld[i] = (struct fld_buf){ addr, width, rowbytes, odd }; return; }
    int i = r->fld_n < 4 ? r->fld_n++ : (r->fld_next++ & 3);
    r->fld[i] = (struct fld_buf){ addr, width, rowbytes, odd };
}

void rdp_fld_update(struct rdp *r)
{
    r->fld_dirty = false;
    if (r->scfield) {
        static const uint8_t bpp[4] = { 0, 1, 2, 4 };   /* bytes per pixel (4-bit buffers are never scanned out) */
        rdp_fld_note(r, r->fb_address, r->fb_width, r->fb_width * bpp[r->fb_size & 3], r->sckeepodd & 1);
        return;
    }
    for (int i = 0; i < r->fld_n; i++)
        if (r->fld[i].addr == r->fb_address) { r->fld[i] = r->fld[--r->fld_n]; break; }
}

const struct fld_buf *rdp_fld_find(const struct rdp *r, uint32_t origin, int *row_off)
{
    origin &= 0xffffff;
    for (int i = 0; i < r->fld_n; i++) {
        const struct fld_buf *f = &r->fld[i];
        if (f->rowbytes <= 0 || origin < f->addr || origin - f->addr > (uint32_t)f->rowbytes) continue;
        *row_off = origin != f->addr;
        return f;
    }
    return NULL;
}

void rdp_vi_scanned(struct rdp *r)
{
    r->vi_since = true;
}

bool rdp_fb_skipped(const struct rdp *r, uint32_t vi_origin)
{
    uint32_t o = vi_origin & 0xffffff, best = 0x10000;
    int bi = -1;
    for (int i = 0; i < FOIR_FB_HIST; i++)
        if (r->fb_hist[i].addr <= o && o - r->fb_hist[i].addr < best) { best = o - r->fb_hist[i].addr; bi = i; }
    return bi >= 0 && r->fb_hist[bi].skipped;
}

void rdp_fb_skip_log(struct rdp *r, uint32_t vi_origin, bool stale)
{
    char line[1024];
    int n = snprintf(line, sizeof line, "skiplog: vi origin %06x %s (refills %u);", vi_origin & 0xffffff, stale ? "HELD (stale)" : "shown", r->stat_repairs);
    for (int i = 0; i < FOIR_FB_HIST && n < (int)sizeof line - 120; i++) {
        struct fb_hist_ent *e = &r->fb_hist[i];
        if (e->addr == 0xffffffffu) continue;
        if (e->drawn || e->skip_tri || e->skip_fill || e->skip_copy)
            n += snprintf(line + n, sizeof line - (size_t)n, " [%06x w%d s%d %s%s drawn %u skipped tri %u fill %u copy %u]", e->addr, e->width, e->size,
                          scan_hit(r, e->addr) ? "scan" : "offscreen", e->skipped ? " skip-frame" : "", e->drawn, e->skip_tri, e->skip_fill, e->skip_copy);
        e->drawn = e->skip_tri = e->skip_fill = e->skip_copy = 0;
    }
    /* a hash of every scanned-out buffer's lines, to see when one changes without a frame starting in it */
    n = (int)strlen(line);
    n += snprintf(line + n, sizeof line - (size_t)n, " | hashes:");
    for (int i = 0; i < 8 && n < (int)sizeof line - 40; i++) {
        uint32_t a = r->scan_addr[i];
        if (a == 0xffffffffu) continue;
        uint32_t best = 0x10000;
        for (int j = 0; j < FOIR_FB_HIST; j++)
            if (r->fb_hist[j].addr <= a && a - r->fb_hist[j].addr < best) best = a - r->fb_hist[j].addr;
        if (best == 0x10000) continue;
        a -= best;       /* the colour image holding the scan origin */
        uint32_t hsh = 2166136261u;
        for (uint32_t k = 0; k < 0x25800 && a + k + 4 <= r->rdram_size; k += 4) hsh = (hsh ^ *(const uint32_t *)(r->rdram + a + k)) * 16777619u;
        n += snprintf(line + n, sizeof line - (size_t)n, " %06x=%08x", r->scan_addr[i], hsh);
    }
    foir_log(FOIR_LOG_INFO, "%s", line);
    if (getenv("FOIR_RDRAM_BLOCKS")) {     /* bisecting: a hash of each 32 KB block of RDRAM, to compare a skipped run with a full one */
        char *blk = malloc((size_t)r->rdram_size / 0x8000 * 5 + 64);
        int m = 0;
        for (uint32_t b = 0; blk && b + 0x8000 <= r->rdram_size; b += 0x8000) {
            uint32_t hsh = 2166136261u;
            for (uint32_t k = 0; k < 0x8000; k += 4) hsh = (hsh ^ *(const uint32_t *)(r->rdram + b + k)) * 16777619u;
            m += sprintf(blk + m, "%04x ", hsh & 0xffff);
        }
        if (blk) { foir_log(FOIR_LOG_INFO, "blocks: %s", blk); free(blk); }
    }
}

/* FrameSkip drops primitives, but not those into an off-screen target (a later primitive may texture from it), and not fills and copies
 * (cheap, and the only thing that ever repaints a border or a backdrop): a skipped frame is the triangles and texture rectangles that
 * go to a buffer the VI scans out */
static bool skip_this(struct rdp *r, struct fb_hist_ent *e)
{
    if (r->cfg.skip_opts & 32) return false;     /* dry run: all the bookkeeping, no skipping */
    if (!r->skip_pixels) return false;
    if (r->cfg.skip_legacy) return true;
    if (!(r->cfg.skip_opts & 8) && (r->om.cycle_type == CYCLE_TYPE_FILL || r->om.cycle_type == CYCLE_TYPE_COPY)) return false;
    if (!(r->cfg.skip_opts & 4) && !scan_hit(r, r->fb_address)) return false;
    return true;
}

/* A frame, for FrameSkip, starts with the first primitive into a scanned-out colour image after the game switched to another one or the VI
 * scanned this one out.  Counting SYNC_FULLs instead lets a game that syncs twice per frame (an off-screen target, then the screen) put every
 * screen frame on the same side of the skip pattern. */
/* A rendered frame that follows a skipped one into the same buffer starts from a buffer that lacks the skipped frame.  A game that
 * redraws everything does not care; one that draws on top of what is there (a pause menu over a frozen scene, a HUD) would keep the hole.
 * So the buffer is refilled with the last frame that was rendered, which is what the game's own picture was one frame ago. */
static void repair_from_good(struct rdp *r)
{
    if (r->good_addr == 0xffffffffu || r->good_addr == r->fb_address || (r->fb_size != 2 && r->fb_size != 3) || r->fb_width <= 0) return;
    int rows = (r->clip.yl + 3) >> 2;
    if (rows < 1 || rows > 576) rows = 240;
    const size_t bpp = r->fb_size == 3 ? 4 : 2, n = (size_t)r->fb_width * bpp * (size_t)rows;
    const uint32_t dst = r->fb_address & RDRAM_MASK, src = r->good_addr & RDRAM_MASK;
    if ((size_t)dst + n > r->rdram_size || (size_t)src + n > r->rdram_size) return;
    if ((dst < src ? src - dst : dst - src) < n) return;     /* overlapping images: not two buffers */
    if (r->mt) mt_sync(r);                                  /* the previous frame's workers must be done writing the source */
    memcpy(r->rdram + dst, r->rdram + src, n);
    if (r->fb_size == 2 && r->hidden) memcpy(r->hidden + (dst >> 1), r->hidden + (src >> 1), n >> 1);
    raster_mem_written(r);
    r->stat_repairs++;
}

/* FrameSkip N renders one frame in N + 1; auto follows skip_level (set from the achieved frame rate by the plugin).  A frame that follows a
 * light one is not worth skipping, and 2D / menu frames are the ones that draw on top of what is already in the buffer. */
void rdp_skip_decide(struct rdp *r)
{
    const int fs = r->cfg.frame_skip;
    bool skip = false;
    r->skip_frames++;
    if (fs > 0) skip = (r->skip_frames % (unsigned)(fs + 1)) != 0;
    else if (fs == FOIR_FRAMESKIP_AUTO) {
        if (r->skip_level == 1) skip = (r->skip_frames & 1) != 0;
        else if (r->skip_level == 2) skip = (r->skip_frames % 3) != 0;
    }
    if (skip && !r->cfg.skip_legacy && !r->skip_eligible) skip = false;
    if (skip) r->stat_skipped++;
    r->skip_pixels = skip;
}

/* The frame FrameSkip decides on is a VI period: everything drawn into scanned-out buffers between two scan-outs.  Counting SYNC_FULLs
 * (or buffer switches) instead lets a game that syncs twice per frame, or draws a frame in two passes into two buffers (the second pass
 * textures from the first), put the passes on different sides of the skip pattern. */
static void frame_boundary(struct rdp *r)
{
    if (r->cfg.skip_legacy || !r->frame_start_cb || !r->vi_since || !scan_hit(r, r->fb_address)) return;
    r->vi_since = false;
    if (r->period_rendered && r->last_rendered != 0xffffffffu) r->good_addr = r->last_rendered;   /* the period that just ended was rendered */
    r->skip_eligible = ((int)r->period_prims >= r->cfg.skip_min_prims && !r->scfield) || (r->cfg.skip_opts & 2);   /* a field-rendered frame draws half the lines */
    r->period_prims = 0;
    r->period++;
    r->frame_start_cb(r->frame_end_ctx);
    r->period_rendered = !r->skip_pixels;
    if (r->cfg.skip_log)
        foir_log(FOIR_LOG_INFO, "skiplog: period %u starts with a primitive into %06x (field-rendered %d): heavy %d -> %s, good %06x", r->period, r->fb_address, r->scfield,
                 r->skip_eligible, r->skip_pixels ? "SKIP" : "render", r->good_addr & 0xffffff);
}

/* the bytes [*a0, *a1) the colour image (z = false) or the Z image can be written in under the current scissor */
static void image_range(const struct rdp *r, bool z, uint32_t *a0, uint32_t *a1)
{
    const uint32_t rows = ((uint32_t)r->clip.yl >> 2) + 1, w = (uint32_t)r->fb_width;
    const uint32_t bpp = z || r->fb_size == PIXEL_SIZE_16BIT ? 2 : r->fb_size == PIXEL_SIZE_32BIT ? 4 : 1;
    *a0 = z ? r->zb_address : r->fb_address;
    *a1 = *a0 + w * bpp * rows;
}

void raster_mem_written(struct rdp *r)
{
    r->fill_n = 0;
}

/* FILL_RECTs drawn since nothing else wrote their bytes. A FILL_RECT identical to one of them writes the same bytes again (Pokemon Snap clears
 * its Z buffer twice in a row): true, skip it. Else forget those the primitive about to be drawn may write, and note it if it is a FILL_RECT. */
static bool fill_note(struct rdp *r)
{
    const struct other_modes *om = &r->om;
    if (r->fill_pend)
        for (int i = 0; i < r->fill_n; i++) {
            const struct fill_rec *f = &r->fill_last[i];
            if (f->w0 == r->fill_w[0] && f->w1 == r->fill_w[1] && f->fb_address == r->fb_address && f->fill_color == r->fill_color &&
                f->fb_size == r->fb_size && f->fb_width == r->fb_width && f->scfield == r->scfield && f->sckeepodd == r->sckeepodd &&
                !memcmp(&f->clip, &r->clip, sizeof(f->clip))) {
                r->fill_pend = false;
                r->stat_fill_skips++;
                return true;
            }
        }
    uint32_t c0, c1, z0 = 0, z1 = 0;
    image_range(r, false, &c0, &c1);
    if ((om->cycle_type == CYCLE_TYPE_1 || om->cycle_type == CYCLE_TYPE_2) && om->z_update_en) image_range(r, true, &z0, &z1);
    int n = 0;
    for (int i = 0; i < r->fill_n; i++) {
        const struct fill_rec *f = &r->fill_last[i];
        if ((f->a0 < c1 && c0 < f->a1) || (f->a0 < z1 && z0 < f->a1)) continue;
        r->fill_last[n++] = *f;
    }
    r->fill_n = n;
    if (!r->fill_pend) return false;
    r->fill_pend = false;
    if (r->fill_n == 4) { memmove(&r->fill_last[0], &r->fill_last[1], 3 * sizeof(r->fill_last[0])); r->fill_n = 3; }
    struct fill_rec *f = &r->fill_last[r->fill_n++];
    f->w0 = r->fill_w[0]; f->w1 = r->fill_w[1];
    f->fb_address = r->fb_address; f->fill_color = r->fill_color; f->a0 = c0; f->a1 = c1;
    f->fb_size = r->fb_size; f->fb_width = r->fb_width; f->scfield = r->scfield; f->sckeepodd = r->sckeepodd; f->clip = r->clip;
    return false;
}

void raster_draw(struct rdp *r, const struct prim_walk *pw, int start, int end)
{
    frame_boundary(r);
    struct fb_hist_ent *e = &r->fb_hist[fb_history_find(r)];
    if (!r->cfg.skip_legacy && scan_hit(r, r->fb_address)) {
        if (e->period != r->period) {      /* the first drawing into this buffer in this period */
            e->period = r->period;
            if (!r->skip_pixels && e->skipped && !(r->cfg.skip_opts & 1)) repair_from_good(r);
            e->skipped = r->skip_pixels;
        }
        r->period_prims++;
        if (!r->skip_pixels) r->last_rendered = r->fb_address;
    } else {
        e->skipped = r->skip_pixels;
    }
    e->width = r->fb_width; e->size = r->fb_size;
    if (r->cfg.debug_nopixels) return;
    if (skip_this(r, e)) {
        if (r->om.cycle_type == CYCLE_TYPE_FILL) e->skip_fill++;
        else if (r->om.cycle_type == CYCLE_TYPE_COPY) e->skip_copy++;
        else e->skip_tri++;
        return;
    }
    if (fill_note(r)) return;
    e->drawn++;
    prim_prepare(r);
    if (r->mt) {
        mt_submit(r, pw, start, end);
        return;
    }
    r->nworkers = 1;
    r->worker_id = 0;
    raster_render_walk(r, pw, start, end);
}
