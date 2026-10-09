/*
 * mupen64plus-rdp-foir - raster.c
 * Span driver. Takes the scanlines produced by the edge walker, collects their
 * pixels (possibly from several consecutive scanlines) into struct-of-arrays
 * pixel batches and runs them through the kernel table (texture -> combine ->
 * zcmp -> blend -> store). FILL and COPY cycle types are dispatched to
 * dedicated kernels.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"
#include "mt.h"
#include "spec.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

static struct { uint8_t cvg, cvbit, xoff, yoff; } cvarray[256];

static uint16_t decompress_cvmask_frombyte(uint8_t x)
{
    return (uint16_t)((x & 0x5) | ((x & 0x5a) << 4) | ((x & 0xa0) << 8));
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
    done = true;
}

/* ---- coverage mask for one scanline -------------------------------------- */

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

/* interpolant lanes: dst[k] = base + step * k for k in [0, n) (may write up to 3 extra lanes) */
FOIR_INLINE void fill_ramp(int32_t *dst, int32_t base, int32_t step, int n)
{
#if defined(FOIR_NEON)
    static const int32_t lane[4] = { 0, 1, 2, 3 };
    int32x4_t v = vmlaq_n_s32(vdupq_n_s32(base), vld1q_s32(lane), step);
    int32x4_t inc = vdupq_n_s32(step * 4);
    for (int i = 0; i < n; i += 4) {
        vst1q_s32(dst + i, v);
        v = vaddq_s32(v, inc);
    }
#else
    for (int i = 0; i < n; i++) dst[i] = base + step * i;
#endif
}

static void fill_idx(struct px_batch *b, int pos, uint32_t idx0, uint8_t dity, int x0, int n)
{
#if defined(FOIR_NEON)
    static const uint32_t lane[4] = { 0, 1, 2, 3 };
    static const uint8_t pat[32] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 };
    uint32x4_t v = vaddq_u32(vdupq_n_u32(idx0), vld1q_u32(lane));
    for (int i = 0; i < n; i += 4) {
        vst1q_u32(&b->fbidx[pos + i], v);
        v = vaddq_u32(v, vdupq_n_u32(4));
    }
    uint8x16_t d = vorrq_u8(vld1q_u8(pat + (x0 & 3)), vdupq_n_u8(dity));
    for (int i = 0; i < n; i += 16) vst1q_u8(&b->ditidx[pos + i], d);
    static const int16_t lane16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int16x8_t xv = vaddq_s16(vdupq_n_s16((int16_t)x0), vld1q_s16(lane16));
    for (int i = 0; i < n; i += 8) {
        vst1q_s16(&b->xpos[pos + i], xv);
        xv = vaddq_s16(xv, vdupq_n_s16(8));
    }
#else
    for (int i = 0; i < n; i++) {
        b->fbidx[pos + i] = idx0 + (uint32_t)i;
        b->ditidx[pos + i] = (uint8_t)(dity | ((x0 + i) & 3));
        b->xpos[pos + i] = (int16_t)(x0 + i);
    }
#endif
}

static void fill_cvg(struct px_batch *b, int pos, const uint8_t *m, int n)
{
#if defined(FOIR_NEON)
    /* mask bytes -> coverage count (popcount) and the "full" bit; the sub-pixel offsets are only non-zero
     * for partially covered pixels, which are fixed up from the table afterwards. Whole vectors are stored
     * (the batch arrays have slack) and the lanes past n are ignored. */
    static const uint8_t tailmask[32] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int full = 1;
    for (int i = 0; i < n; i += 16) {
        uint8x16_t v = vld1q_u8(m + i);
        int left = n - i;
        uint8x16_t pad = left >= 16 ? vdupq_n_u8(0) : vld1q_u8(tailmask + 16 - left);
        uint8x16_t isfull = vceqq_u8(v, vdupq_n_u8(0xff));
        vst1q_u8(&b->cvg[pos + i], vcntq_u8(v));
        vst1q_u8(&b->cvbit[pos + i], vshrq_n_u8(v, 7));
        vst1q_u8(&b->offx[pos + i], vdupq_n_u8(0));
        vst1q_u8(&b->offy[pos + i], vdupq_n_u8(0));
        uint8x16_t partial = vbicq_u8(vmvnq_u8(isfull), pad);       /* lanes < n with mask != 0xff */
        uint64_t bits = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(partial), 4)), 0);
        if (bits) {
            full = 0;
            while (bits) {
                int k = __builtin_ctzll(bits) >> 2;
                bits &= ~(0xfull << (k * 4));
                uint8_t c = m[i + k];
                b->offx[pos + i + k] = cvarray[c].xoff;
                b->offy[pos + i + k] = cvarray[c].yoff;
            }
        }
    }
    if (!full) b->all_full = 0;
#else
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
#endif
}

/* The colour buffer, Z buffer and hidden-bit lines of a run of pixels are touched by the zcmp / store stages a whole texture +
 * combiner pass later. Asking for them as soon as the run is known hides the cache misses of the first access (the Cortex-A55
 * does not reorder): 0.5-2 % of the replay time on one thread. Prefetches never fault. */
#ifndef FOIR_PREFETCH
#define FOIR_PREFETCH 3       /* locality hint of the prefetches; 0 = no prefetching */
#endif

#if defined(FOIR_NEON) && FOIR_PREFETCH
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

#if defined(FOIR_NEON)
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

FOIR_INLINE void ramp4(int32_t *dst, int32x4_t base, int32x4_t dI, int32x4_t d4, int n)
{
    int32x4_t v = vaddq_s32(base, dI);
    for (int i = 0; i < n; i += 4) {
        vst1q_s32(dst + i, v);
        v = vaddq_s32(v, d4);
    }
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
    compute_cvg(r, sp, flip);

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

    int done = 0;
    while (done < total) {
        int pos = b->n;
        int take = total - done;
        if (take > FOIR_BATCH - pos) take = FOIR_BATCH - pos;
        int x0 = xlow + done;

        fill_idx(b, pos, fbrow + (uint32_t)x0, dity, x0, take);
        {
            const uint32_t lo = fbrow + (uint32_t)x0, hi = lo + (uint32_t)take - 1;
            if (lo < b->fbmin) b->fbmin = lo;
            if (hi > b->fbmax) b->fbmax = hi;
        }
#if FOIR_PREFETCH
        prefetch_piece(r, fbrow + (uint32_t)x0, take);
#endif
        if (pf->shade_rgb || pf->shade_a || pf->need_cz || pf->need_tex) {
            int32x4_t c_lo = vmlaq_n_s32(b_lo, d_lo, done), c_hi = vmlaq_n_s32(b_hi, d_hi, done);
            if (pf->shade_rgb) {
                ramp4(&b->cr[pos], vdupq_laneq_s32(c_lo, 0), sv->dI[0], sv->d4[0], take);
                ramp4(&b->cg[pos], vdupq_laneq_s32(c_lo, 1), sv->dI[1], sv->d4[1], take);
                ramp4(&b->cb[pos], vdupq_laneq_s32(c_lo, 2), sv->dI[2], sv->d4[2], take);
            }
            if (pf->shade_a || pf->shade_rgb)
                ramp4(&b->ca[pos], vdupq_laneq_s32(c_lo, 3), sv->dI[3], sv->d4[3], take);
            if (pf->need_cz)
                ramp4(&b->cz[pos], vdupq_laneq_s32(c_hi, 3), sv->dI[7], sv->d4[7], take);
            if (pf->need_tex) {
                ramp4(&b->s[pos], vdupq_laneq_s32(c_hi, 0), sv->dI[4], sv->d4[4], take);
                ramp4(&b->t[pos], vdupq_laneq_s32(c_hi, 1), sv->dI[5], sv->d4[5], take);
                ramp4(&b->w[pos], vdupq_laneq_s32(c_hi, 2), sv->dI[6], sv->d4[6], take);
            }
        }
        fill_cvg(b, pos, &r->cvgbuf[(x0) & 0x3ff], take);

        b->n = pos + take;
        done += take;
        if (b->n >= FOIR_BATCH) batch_flush(r, b);
    }
}
#else
static void render_span_cycle(struct rdp *r, int y, const struct span *sp)
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

        fill_idx(b, pos, fbrow + (uint32_t)x0, dity, x0, take);
        if (pf->shade_rgb) {
            fill_ramp(&b->cr[pos], rr + pc->dr * done, pc->dr, take);
            fill_ramp(&b->cg[pos], gg + pc->dg * done, pc->dg, take);
            fill_ramp(&b->cb[pos], bb + pc->db * done, pc->db, take);
        }
        if (pf->shade_a || pf->shade_rgb)
            fill_ramp(&b->ca[pos], aa + pc->da * done, pc->da, take);
        if (pf->need_cz)
            fill_ramp(&b->cz[pos], zz + pc->dz * done, pc->dz, take);
        if (pf->need_tex) {
            fill_ramp(&b->s[pos], ss + pc->ds * done, pc->ds, take);
            fill_ramp(&b->t[pos], tt + pc->dt * done, pc->dt, take);
            fill_ramp(&b->w[pos], ww + pc->dw * done, pc->dw, take);
        }
        fill_cvg(b, pos, &r->cvgbuf[(x0) & 0x3ff], take);

        b->n = pos + take;
        done += take;
        if (b->n >= FOIR_BATCH) batch_flush(r, b);
    }
}
#endif

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

void raster_render_job(struct rdp *r, const struct span *rows, int jlo, int start, int end)
{
    const struct foir_kernels *k = r->k;
    struct px_batch *b = &r->batch;
    int ct = r->om.cycle_type;
    int nw = r->nworkers > 1 ? r->nworkers : 1;
    int wid = r->worker_id;

    if (ct == CYCLE_TYPE_FILL) {
        if (r->fb_size == PIXEL_SIZE_4BIT) return;
        for (int y = start; y <= end; y++) {
            if (nw > 1 && ((y >> FOIR_STRIPE_SHIFT) % nw) != wid) continue;
            const struct span *sp = &rows[y - jlo];
            if (!sp->validline) continue;
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
            if (nw > 1 && ((y >> FOIR_STRIPE_SHIFT) % nw) != wid) continue;
            const struct span *sp = &rows[y - jlo];
            if (!sp->validline) continue;
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
        if (nw > 1 && ((y >> FOIR_STRIPE_SHIFT) % nw) != wid) continue;
        const struct span *sp = &rows[y - jlo];
        if (!sp->validline) continue;
#if defined(FOIR_NEON)
        render_span_cycle(r, y, sp, &sv);
#else
        render_span_cycle(r, y, sp);
#endif
    }
    batch_flush(r, b);
}

void raster_draw(struct rdp *r, struct span *rows, int jlo, int start, int end)
{
    if (r->cfg.debug_nopixels || r->skip_pixels) return;
    prim_prepare(r);
    if (r->mt) {
        mt_submit(r, rows, jlo, start, end);
        return;
    }
    r->nworkers = 1;
    r->worker_id = 0;
    raster_render_job(r, rows, jlo, start, end);
}
