/*
 * mupen64plus-rdp-foir - raster.c
 * Span driver. Takes the scanlines produced by the edge walker, collects their
 * pixels (possibly from several consecutive scanlines) into struct-of-arrays
 * pixel batches and runs them through the kernel table (texture -> combine ->
 * zcmp -> blend -> store). FILL and COPY cycle types are dispatched to
 * dedicated kernels.
 *
 * Two span builders produce the batches: render_span_ref (portable C, with the
 * per-scanline coverage buffer of compute_cvg; the scalar renderer, and the
 * reference of the NEON builder in tools/kfuzz.c -m 64) and render_span_cycle
 * (NEON: vector stores for whole 16-lane blocks, coverage computed for the
 * edge pixels only). They must produce identical batches.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"
#include "mt.h"
#include "spec.h"
#include "walk.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

static struct { uint8_t cvg, cvbit, xoff, yoff; } cvarray[256];
#if defined(FOIR_NEON)
/* edge coverage of one subscanline at a pixel, indexed by subscanline * 8 + (x & 7): leftcvghex / rightcvghex already
 * shifted to the subscanline's place in the 8-bit mask */
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

/* ---- coverage mask for one scanline: the reference (scalar renderer, and the test of the NEON builder) ---- */

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

/* ---- batch building --------------------------------------------------------- */

#ifdef FOIR_STAGETIME
/* per-stage timing build (-DFOIR_STAGETIME, aarch64 only): generic timer ticks per batch stage, the combine stage split
 * by kind of kernel; printed by foir-replay (inline rendering, -t 0) */
static inline uint64_t stage_tick(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
#endif

static void batch_flush(struct rdp *r, struct px_batch *b)
{
    if (b->n <= 0) return;
    const struct foir_kernels *k = r->k;
#ifdef FOIR_STATS
    foir_stats_batch(r, b->n);
#endif
#ifdef FOIR_STAGETIME
    uint64_t t0 = stage_tick(), t1, t2;
    if (r->pf.need_tex) k->texture(r, b);
    t1 = stage_tick();
    k->combine(r, b);
    t2 = stage_tick();
    r->st_tk[0] += t1 - t0; r->st_n[0] += r->pf.need_tex;
    {
        const int kind = !(r->spec_mask & SPEC_BIT(SPEC_COMB)) ? 2 : r->spec_comb_shape ? 1 : 0;
        r->st_tk[5 + kind] += t2 - t1; r->st_n[5 + kind]++; r->st_cpix[kind] += (uint64_t)b->n;
    }
    k->zcmp(r, b);
    t0 = stage_tick(); r->st_tk[2] += t0 - t2;
    k->blend(r, b);
    t1 = stage_tick(); r->st_tk[3] += t1 - t0;
    k->store(r, b);
    t2 = stage_tick(); r->st_tk[4] += t2 - t1;
#else
    if (r->pf.need_tex) k->texture(r, b);
    k->combine(r, b);
    k->zcmp(r, b);
    k->blend(r, b);
    k->store(r, b);
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

/* ---- portable span builder: the reference ------------------------------------------------------------------------------
 * Plain C, used by the scalar renderer and, in the NEON build, by the batch-builder test of tools/kfuzz.c that checks
 * the NEON builder against it (raster_render_job_ref). */

/* interpolant lanes: dst[k] = base + step * k for k in [0, n) */
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

    /* hardware iterates from xendsc (right end when !flip); we store pixels in ascending
     * x order, so every interpolant advances by +d per pixel from the left end */
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

/* ---- NEON span builder ------------------------------------------------------------------------------------------------ */

#if defined(FOIR_NEON)

/* The colour buffer, Z buffer and hidden-bit lines of a run of pixels are touched by the zcmp / store stages a whole texture +
 * combiner pass later. Asking for them as soon as the run is known hides the cache misses of the first access (the Cortex-A55
 * does not reorder): 0.5-2 % of the replay time on one thread (3 % in Donkey Kong 64). Prefetches never fault. */
#ifndef FOIR_PREFETCH
#define FOIR_PREFETCH 3       /* locality hint of the prefetches; 0 = no prefetching */
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

/* ramps of the eight interpolants: dI = d * {0,1,2,3}, d4 = 4 * d */
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

/* Coverage of a span without the per-pixel coverage buffer of the portable builder (compute_cvg).
 *
 * Subscanline i (0..3) covers pixels le_i < x < re_i completely, le_i and re_i partially (leftcvghex / rightcvghex
 * of the edge position) and nothing else; le / re are minorx / majorx, swapped when the primitive is flipped. As a
 * mask of pixel x that is  A_i(x) & B_i(x)  with
 *     A_i = x > le_i ? F_i : x == le_i ? L_i : 0      B_i = x < re_i ? F_i : x == re_i ? R_i : 0
 * (F_i: the subscanline's bits of the 8-bit mask) -- which also gives the right answer for le_i == re_i (L & R) and
 * for le_i > re_i (nothing). Invalid subscanlines have no bits. The span runs from the smallest valid le_i to the
 * largest valid re_i, so every pixel right of all le_i and left of all re_i has the same mask (v: all bits of the
 * valid subscanlines, 0xff when all four are valid): only the pixels of the "edge zones" (up to the largest le_i, from
 * the smallest re_i) need evaluating, which is a vector operation across the four subscanlines. minorx / majorx are
 * the 13-bit values of the edge walker. */
struct cvg_edges {
    int32x4_t lei, rei;           /* integer pixel of the edges, per subscanline */
    uint32x4_t fm, lh, rh;        /* F_i, L_i, R_i of the edge pixels (0 for invalid subscanlines) */
    uint32_t v;                   /* mask of the pixels between the edge zones */
    int nl, rs;                   /* span-relative: left zone [0, nl), right zone [rs, ...), interior in between (nl <= rs) */
};

FOIR_INLINE void cvg_edges_init(struct cvg_edges *e, const struct span *sp, int flip, int lo)
{
    const int32_t *lp = flip ? sp->majorx : sp->minorx, *rp = flip ? sp->minorx : sp->majorx;
    const uint32x4_t lx = vreinterpretq_u32_s32(vld1q_s32(lp)), rx = vreinterpretq_u32_s32(vld1q_s32(rp));
    const uint32x4_t iv = vreinterpretq_u32_s32(vld1q_s32(sp->invalyscan));
    const uint32x4_t inv = vtstq_u32(iv, iv);                                  /* all ones in invalid subscanlines */
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

/* the 8-bit coverage mask of pixel x */
FOIR_INLINE uint32_t cvg_mask_at(const struct cvg_edges *e, int x)
{
    const int32x4_t xv = vdupq_n_s32(x);
    const uint32x4_t a = vbslq_u32(vcgtq_s32(xv, e->lei), e->fm, vandq_u32(vceqq_s32(xv, e->lei), e->lh));
    const uint32x4_t bm = vbslq_u32(vcgtq_s32(e->rei, xv), e->fm, vandq_u32(vceqq_s32(xv, e->rei), e->rh));
    return vaddvq_u32(vandq_u32(a, bm));
}

/* coverage of the pixel at span-relative position p into lane `lane` of the batch; returns the "full" bit */
FOIR_INLINE int cvg_patch(struct px_batch *b, const struct cvg_edges *e, int lo, int p, int lane)
{
    const uint32_t m = cvg_mask_at(e, lo + p);
    b->cvg[lane] = cvarray[m].cvg;
    b->cvbit[lane] = cvarray[m].cvbit;
    b->offx[lane] = cvarray[m].xoff;
    b->offy[lane] = cvarray[m].yoff;
    return m == 0xff;
}

/* The vector stores of one piece of a span (take pixels; the batch arrays have 16 lanes of slack and the lanes past the piece
 * are ignored, so the ramps run in groups of four lanes and the byte arrays are stored as whole 16-lane vectors): batch
 * geometry, the interpolant ramps of the channels the pipeline reads and the constant coverage of the interior. M selects the channels (1 shade rgb + alpha, 2 shade alpha only, 4 Z, 8 texture
 * coordinates); it is a constant, so each instance is straight-line code with no per-channel branches. */
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
    /* four lanes of every channel (they advance together), then on to the next four */
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

    /* hardware iterates from xendsc (right end when !flip); pixels are stored in ascending x order,
     * so every interpolant advances by +d per pixel from the left end */
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
    const uint8x16x4_t kc4 = vld4q_dup_u8(&cvarray[e.v].cvg);             /* the table entry of the interior mask, splatted */
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
            /* the pixels of the edge zones that fall into this piece have their own coverage */
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
#endif /* FOIR_NEON */

/* ---- entry ------------------------------------------------------------------ */

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

/* Render the rows start..end of a job that this context owns. The spans come from the setup record `pw` (computed here, one
 * row at a time, by walk_row) or, for the tests, from the ready-made array `rows` (pw == NULL). */
FOIR_INLINE void render_job(struct rdp *r, const struct span *rows, int jlo, const struct prim_walk *pw, int start, int end, int ref)
{
    const struct foir_kernels *k = r->k;
    struct px_batch *b = &r->batch;
    int ct = r->om.cycle_type;
    (void)ref;
    int nw = r->nworkers > 1 ? r->nworkers : 1;
    int wid = r->worker_id;
    struct span row;

#define NEXT_ROW() \
        if (nw > 1 && ((y >> FOIR_STRIPE_SHIFT) % nw) != wid) continue; \
        const struct span *sp; \
        if (pw) { if (!walk_row(pw, y, &row)) continue; sp = &row; } \
        else { sp = &rows[y - jlo]; if (!sp->validline) continue; }

    if (ct == CYCLE_TYPE_FILL) {
        if (r->fb_size == PIXEL_SIZE_4BIT) return;
        for (int y = start; y <= end; y++) {
            NEXT_ROW();
            int x0 = sp->lx < sp->rx ? sp->lx : sp->rx;
            int x1 = sp->lx < sp->rx ? sp->rx : sp->lx;
            if (x1 >= r->fb_width) x1 = r->fb_width - 1;   /* no wrap into the next row (owned by another worker) */
            if (x1 < x0) continue;
            k->fill(r, y, x0, x1);
            r->stat_pixels += (uint32_t)(x1 - x0 + 1);   /* FILL/COPY do not go through batch_flush */
        }
        return;
    }
    if (ct == CYCLE_TYPE_COPY) {
        if (r->fb_size == PIXEL_SIZE_32BIT) return;
        for (int y = start; y <= end; y++) {
            NEXT_ROW();
            k->copy(r, y, sp, r->pc.flip);
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
    for (int y = start; y <= end; y++) {
        NEXT_ROW();
#if defined(FOIR_NEON)
        if (ref) render_span_ref(r, y, sp);
        else render_span_cycle(r, y, sp, &sv);
#else
        render_span_ref(r, y, sp);
#endif
    }
#undef NEXT_ROW
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

/* the same with the portable span builder (tools/kfuzz.c compares the two) */
void raster_render_job_ref(struct rdp *r, const struct span *rows, int jlo, int start, int end)
{
    render_job(r, rows, jlo, NULL, start, end, 1);
}

void raster_draw(struct rdp *r, const struct prim_walk *pw, int start, int end)
{
    if (r->cfg.debug_nopixels || r->skip_pixels) return;
    prim_prepare(r);
    if (r->mt) {
        mt_submit(r, pw, start, end);
        return;
    }
    r->nworkers = 1;
    r->worker_id = 0;
    raster_render_walk(r, pw, start, end);
}
