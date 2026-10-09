/*
 * mupen64plus-rdp-foir - kneon_mem.c
 * NEON framebuffer / Z-buffer access: Z compare (no lookup tables), the
 * blender memory inputs, FB + Z store, FILL.
 *
 * RDRAM is held as host 32-bit words (so 16-bit pixel i lives at host halfword
 * i ^ 1) and every 16-bit pixel has a "hidden" byte (see rdram.h). A group of
 * eight pixels with consecutive indices is read / written with vector accesses
 * (reversing halfword pairs); anything else takes a per-lane path.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kneon.h"
#include "spec.h"

#if FOIR_HALF_XOR != 1 || FOIR_BYTE_XOR != 3
#error "the NEON memory kernels assume a little-endian host with word-swapped RDRAM"
#endif

/* ---- 16-bit pixel (+ hidden bits) groups ------------------------------------------------ */

/* A group of eight lanes with consecutive pixel indices is read / written with one vector window (a load, or a
 * load-modify-write with the lane mask). About a fifth of the groups straddle two scanlines of the batch and the tail
 * group of a batch may be short: those take the per-lane scalar path. (Splitting a straddling group into its two runs
 * and accessing each with its own window was measured and does not pay on the Cortex-A55: -2 % instructions, no change in
 * cycles, see the README.) The RDRAM bounds checks of the windows are done once per batch and buffer from the lowest and
 * highest pixel index of the batch instead of once per window. */

FOIR_INLINE bool grp_contig(const struct px_batch *b, int o)
{
    static const uint32_t iota0[4] = { 0, 1, 2, 3 }, iota1[4] = { 4, 5, 6, 7 };
    uint32x4_t a = vld1q_u32(&b->fbidx[o]), c = vld1q_u32(&b->fbidx[o + 4]);
    uint32x4_t base = vdupq_n_u32(b->fbidx[o]);
    uint32x4_t e0 = vceqq_u32(a, vaddq_u32(base, vld1q_u32(iota0)));
    uint32x4_t e1 = vceqq_u32(c, vaddq_u32(base, vld1q_u32(iota1)));
    return vminvq_u32(vandq_u32(e0, e1)) == 0xffffffffu;
}

FOIR_INLINE uint8x8_t hv_resolve(uint8x8_t h, uint16x8_t px)
{
    uint8x8_t lsb = vand_u8(vmovn_u16(px), vdup_n_u8(1));
    uint8x8_t valid = vtst_u8(h, vdup_n_u8(8));
    uint8x8_t match = vceq_u8(vand_u8(vshr_n_u8(h, 2), vdup_n_u8(1)), lsb);
    return vbsl_u8(vand_u8(valid, match), vand_u8(h, vdup_n_u8(3)), vmul_u8(lsb, vdup_n_u8(3)));
}

/* lowest / highest pixel index of the batch (builder maintained; batches made by other code are scanned) */
FOIR_INLINE void batch_range(const struct px_batch *b, uint32_t *lo, uint32_t *hi)
{
    if (FOIR_LIKELY(b->fbrange)) { *lo = b->fbmin; *hi = b->fbmax; return; }
    uint32_t mn = ~0u, mx = 0;
    for (int i = 0; i < b->n; i++) { uint32_t v = b->fbidx[i]; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    *lo = mn; *hi = mx;
}

/* every window (and every frame an odd window rewrites) of the batch lies in RDRAM, for the 16-bit pixel buffer at halfword base16 */
FOIR_INLINE bool win16_safe(const struct rdp *r, uint32_t base16, uint32_t lo, uint32_t hi)
{
    return base16 + lo >= 8 && base16 + hi + 16 < (RDRAM_MASK >> 1) && ((base16 + hi + 16) << 1) <= r->rdram_size;
}

/* eight pixels from pixel index i0 (RDRAM is held as host words: pixel i lives at halfword i ^ 1), in pixel order */
FOIR_INLINE uint16x8_t win16_ld(const struct rdp *r, uint32_t i0)
{
    const uint16_t *h = (const uint16_t *)r->rdram;
    if (!(i0 & 1)) return vrev32q_u16(vld1q_u16(h + i0));
    uint16x8_t P0 = vrev32q_u16(vld1q_u16(h + i0 - 1)), P1 = vrev32q_u16(vld1q_u16(h + i0 + 7));
    return vextq_u16(P0, P1, 1);
}

/* write the lanes of m (0xffff) of px to the eight pixels from index i0 (and their hidden bytes); the window (an odd i0
 * rewrites the 16-pixel frame it overlaps) must be inside RDRAM and inside one row */
FOIR_INLINE void win16_st(struct rdp *r, uint32_t i0, uint16x8_t px, uint8x8_t hnew, uint16x8_t m)
{
    uint16_t *h = (uint16_t *)r->rdram;
    if (!(i0 & 1)) {
        uint16x8_t old = vrev32q_u16(vld1q_u16(h + i0));
        vst1q_u16(h + i0, vrev32q_u16(vbslq_u16(m, px, old)));
    } else {
        uint16x8_t P0 = vrev32q_u16(vld1q_u16(h + i0 - 1)), P1 = vrev32q_u16(vld1q_u16(h + i0 + 7));
        uint16x8_t old = vextq_u16(P0, P1, 1);
        uint16x8_t merged = vbslq_u16(m, px, old);
        uint16x8_t X = vextq_u16(merged, merged, 7);       /* m7 m0 m1 .. m6 */
        uint16x8_t N0 = vsetq_lane_u16(vgetq_lane_u16(P0, 0), X, 0);
        uint16x8_t N1 = vsetq_lane_u16(vgetq_lane_u16(X, 0), P1, 0);
        vst1q_u16(h + i0 - 1, vrev32q_u16(N0));
        vst1q_u16(h + i0 + 7, vrev32q_u16(N1));
    }
    vst1_u8(r->hidden + i0, vbsl_u8(vmovn_u16(m), hnew, vld1_u8(r->hidden + i0)));
}

/* hidden byte of a written pixel: valid | lsb copy | hidden bits */
FOIR_INLINE uint8x8_t hidden_new(uint16x8_t px, uint8x8_t hv)
{
    return vorr_u8(vorr_u8(vdup_n_u8(8), vshl_n_u8(vand_u8(vmovn_u16(px), vdup_n_u8(1)), 2)), vand_u8(hv, vdup_n_u8(3)));
}

/* 16-bit pixels without the hidden bits (used for the Z value when the delta-Z logic is skipped) */
FOIR_INLINE uint16x8_t fast_z_load(const struct rdp *r, uint32_t base16, const struct px_batch *b, int o, int cnt, bool contig, bool ok)
{
    if (ok && contig) return win16_ld(r, base16 + b->fbidx[o]);
    uint16_t pa[8] ALIGN16;
    memset(pa, 0, sizeof(pa));
    for (int l = 0; l < cnt; l++) pa[l] = rdram_rd16(r, base16 + b->fbidx[o + l]);
    return vld1q_u16(pa);
}

FOIR_INLINE void ld16(const struct rdp *r, uint32_t base16, const struct px_batch *b, int o, int cnt, bool contig, bool ok,
                      uint16x8_t *px, uint8x8_t *hv)
{
    if (ok && contig) {
        const uint32_t i0 = base16 + b->fbidx[o];
        uint16x8_t P = win16_ld(r, i0);
        *px = P;
        *hv = hv_resolve(vld1_u8(r->hidden + i0), P);
        return;
    }
    uint16_t pa[8] ALIGN16;
    uint8_t ha[8] ALIGN16;
    memset(pa, 0, sizeof(pa)); memset(ha, 0, sizeof(ha));
    for (int l = 0; l < cnt; l++) rdram_rd_pair16(r, base16 + b->fbidx[o + l], &pa[l], &ha[l]);
    *px = vld1q_u16(pa);
    *hv = vld1_u8(ha);
}

FOIR_INLINE void st16(struct rdp *r, uint32_t base16, const struct px_batch *b, int o, int cnt, bool contig, bool ok,
                      uint16x8_t px, uint8x8_t hv, uint16x8_t mask)
{
    if (ok && contig) {
        /* An odd-aligned group rewrites the whole 16-pixel frame it overlaps (the pixels outside the group with
         * their old value). Those must belong to the same row: other rows may be rendered by another worker. */
        const uint32_t i0 = base16 + b->fbidx[o];
        const int x0 = b->xpos[o];
        if (FOIR_LIKELY(!(i0 & 1) || (x0 >= 1 && x0 + 14 < r->fb_width))) {
            win16_st(r, i0, px, hidden_new(px, hv), mask);
            return;
        }
    }
    uint16_t pa[8] ALIGN16, ma[8] ALIGN16;
    uint8_t ha[8] ALIGN16;
    vst1q_u16(pa, px); vst1q_u16(ma, mask); vst1_u8(ha, hv);
    for (int l = 0; l < cnt; l++)
        if (ma[l]) rdram_wr_pair16(r, base16 + b->fbidx[o + l], pa[l], ha[l]);
}

/* ---- 32-bit pixels --------------------------------------------------------------------------- */

static void ld32(const struct rdp *r, uint32_t base32, const struct px_batch *b, int o, int cnt, bool contig,
                 uint32x4_t *lo, uint32x4_t *hi)
{
    if (contig) {
        uint32_t i0 = (base32 + b->fbidx[o]) & (RDRAM_MASK >> 2);
        if (i0 < ((RDRAM_MASK >> 2) - 8) && ((i0 + 8) << 2) <= r->rdram_size) {
            const uint32_t *w = (const uint32_t *)r->rdram;
            *lo = vld1q_u32(w + i0); *hi = vld1q_u32(w + i0 + 4);
            return;
        }
    }
    uint32_t a[8] ALIGN16;
    memset(a, 0, sizeof(a));
    for (int l = 0; l < cnt; l++) a[l] = rdram_rd32(r, base32 + b->fbidx[o + l]);
    *lo = vld1q_u32(a); *hi = vld1q_u32(a + 4);
}

static void st32(struct rdp *r, uint32_t base32, const struct px_batch *b, int o, int cnt, bool contig,
                 uint32x4_t lo, uint32x4_t hi, uint16x8_t mask)
{
    int16x8_t ms = vreinterpretq_s16_u16(mask);
    uint32x4_t mlo = vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(ms)));
    uint32x4_t mhi = vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(ms)));
    if (contig) {
        uint32_t i0 = (base32 + b->fbidx[o]) & (RDRAM_MASK >> 2);
        if (i0 < ((RDRAM_MASK >> 2) - 8) && ((i0 + 8) << 2) <= r->rdram_size) {
            uint32_t *w = (uint32_t *)r->rdram;
            vst1q_u32(w + i0, vbslq_u32(mlo, lo, vld1q_u32(w + i0)));
            vst1q_u32(w + i0 + 4, vbslq_u32(mhi, hi, vld1q_u32(w + i0 + 4)));
            return;
        }
    }
    uint32_t a[8] ALIGN16;
    uint16_t ma[8] ALIGN16;
    vst1q_u32(a, lo); vst1q_u32(a + 4, hi); vst1q_u16(ma, mask);
    for (int l = 0; l < cnt; l++)
        if (ma[l]) rdram_wr32(r, base32 + b->fbidx[o + l], a[l]);
}

/* ---- Z compare --------------------------------------------------------------------------------- */

FOIR_INLINE uint32x4_t vsel_u32(uint32x4_t m, uint32x4_t a, uint32x4_t b) { return vbslq_u32(m, a, b); }

struct z4_out {
    uint32x4_t wen, blend_en, prewrap, cvg;
    int32x4_t sha, shb;
};

/* The Z / colour buffer read state the kernel depends on. The generic kernel fills it from the render state at run
 * time, a specialised variant (kspec_mem.inc) passes a literal one, so the same body serves both. */
struct zs {
    int is16;        /* 16-bit colour buffer (else 32) */
    int rd;          /* IMAGE_READ_EN */
    int rgba;        /* colour buffer format is RGBA */
    int zc;          /* Z_COMPARE_EN */
    int zmode;
    int aa, force;
    int shifts;      /* the blender reads the memory alpha: the delta-Z shifts are needed */
    int relaxed;     /* RelaxedPrecision */
};

FOIR_INLINE void z_compare4(const struct rdp *r, const struct zs *S, uint32x4_t sz, uint32x4_t zval, uint32x4_t hval,
                            uint32x4_t cvg, uint32x4_t memcvg, struct z4_out *o)
{
    const int32_t dzpixenc = r->pc.dzpixenc;
    const uint32_t dzpix = (uint16_t)r->pc.dzpix;
    const uint32x4_t one = vdupq_n_u32(1), zero = vdupq_n_u32(0);

    int32x4_t e = vreinterpretq_s32_u32(vshrq_n_u32(zval, 13));
    int32x4_t esh = vmaxq_s32(vsubq_s32(vdupq_n_s32(6), e), vdupq_n_s32(0));
    uint32x4_t mant = vandq_u32(vshrq_n_u32(zval, 2), vdupq_n_u32(0x7ff));
    uint32x4_t add = vsubq_u32(vdupq_n_u32(0x40000), vshlq_u32(vdupq_n_u32(0x40000), vnegq_s32(e)));
    uint32x4_t oz = vandq_u32(vaddq_u32(vshlq_u32(mant, esh), add), vdupq_n_u32(0x3ffff));

    uint32x4_t rawdz = vorrq_u32(vshlq_n_u32(vandq_u32(zval, vdupq_n_u32(3)), 2), hval);
    int32x4_t rd = vreinterpretq_s32_u32(rawdz);
    uint32x4_t dzmem = vshlq_u32(one, rd);
    o->sha = vminq_s32(vmaxq_s32(vsubq_s32(vdupq_n_s32(dzpixenc), rd), vdupq_n_s32(0)), vdupq_n_s32(4));
    o->shb = vminq_s32(vmaxq_s32(vsubq_s32(rd, vdupq_n_s32(dzpixenc)), vdupq_n_s32(0)), vdupq_n_s32(4));

    uint32x4_t lt3 = vcltq_u32(vshrq_n_u32(zval, 13), vdupq_n_u32(3));
    uint32x4_t is8000 = vceqq_u32(dzmem, vdupq_n_u32(0x8000));
    uint32x4_t modifier = vshlq_u32(vdupq_n_u32(16), vnegq_s32(e));
    uint32x4_t dz2 = vmaxq_u32(vshlq_n_u32(dzmem, 1), modifier);
    uint32x4_t force_cop = vandq_u32(lt3, is8000);
    dzmem = vsel_u32(lt3, vsel_u32(is8000, vdupq_n_u32(0xffff), dz2), dzmem);

    uint32x4_t dzor = vandq_u32(vorrq_u32(vdupq_n_u32(dzpix), dzmem), vdupq_n_u32(0xffff));
    uint32x4_t dznot = vshlq_u32(one, vreinterpretq_s32_u32(vsubq_u32(vdupq_n_u32(31), vclzq_u32(dzor))));
    uint32x4_t dznew = vshlq_n_u32(dznot, 3);

    uint32x4_t farther = vorrq_u32(force_cop, vcgeq_u32(vaddq_u32(sz, dznew), oz));
    uint32x4_t overflow = vtstq_u32(vaddq_u32(memcvg, cvg), vdupq_n_u32(8));
    uint32x4_t blend_en = vdupq_n_u32(0);
    if (S->force) blend_en = vdupq_n_u32(~0u);
    else if (S->aa) blend_en = vandq_u32(vmvnq_u32(overflow), farther);
    o->blend_en = blend_en;
    o->prewrap = overflow;
    o->cvg = cvg;

    uint32x4_t infront = vcltq_u32(sz, oz);
    int32x4_t diff = vsubq_s32(vreinterpretq_s32_u32(sz), vreinterpretq_s32_u32(dznew));
    uint32x4_t nearer = vorrq_u32(force_cop, vcleq_s32(diff, vreinterpretq_s32_u32(oz)));
    uint32x4_t maxz = vceqq_u32(oz, vdupq_n_u32(0x3ffff));
    uint32x4_t base_wen = vorrq_u32(maxz, vsel_u32(overflow, infront, nearer));
    uint32x4_t wen;
    switch (S->zmode) {
    case ZMODE_OPAQUE:
        wen = base_wen;
        break;
    case ZMODE_INTERPENETRATING: {
        uint32x4_t special = vandq_u32(vandq_u32(infront, farther), overflow);
        int32x4_t dzenc = vmaxq_s32(vreinterpretq_s32_u32(vsubq_u32(vdupq_n_u32(31), vclzq_u32(dznot))), vdupq_n_s32(0));
        int32x4_t nsh = vnegq_s32(dzenc);
        uint32x4_t coeff = vandq_u32(vsubq_u32(vshlq_u32(oz, nsh), vshlq_u32(sz, nsh)), vdupq_n_u32(0xf));
        uint32x4_t ncvg = vandq_u32(vshrq_n_u32(vmulq_u32(coeff, cvg), 3), vdupq_n_u32(0xf));
        o->cvg = vsel_u32(special, ncvg, cvg);
        wen = vorrq_u32(special, base_wen);
        break;
    }
    case ZMODE_TRANSPARENT:
        wen = vorrq_u32(infront, maxz);
        break;
    default:
        wen = vandq_u32(vandq_u32(farther, nearer), vmvnq_u32(maxz));
        break;
    }
    o->wen = wen;
    (void)zero;
}

FOIR_INLINE void zcmp_body(struct rdp *r, struct px_batch *b, const struct zs S)
{
    NEON_HANDLED(2);

    const struct prim_ctx *pc = &r->pc;
    const int n = b->n, ng = NGROUPS(n);
    const uint32_t fb16 = r->fb_address >> 1, fb32 = r->fb_address >> 2, zb16 = r->zb_address >> 1;
    const int is16 = S.is16;
    static const uint16_t lane_iota[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const uint16x8_t iota = vld1q_u16(lane_iota);
    const int32_t dzpixenc = pc->dzpixenc;
    /* opaque / transparent depth compare: the delta-Z logic is only needed for partially covered memory pixels */
    const int fast_z = S.zc && (S.zmode == ZMODE_OPAQUE || S.zmode == ZMODE_TRANSPARENT);
    const int need_shifts = S.shifts;
    const int relaxed_z = S.relaxed;
    uint32_t pmin, pmax;
    batch_range(b, &pmin, &pmax);
    const bool fb_ok = win16_safe(r, fb16, pmin, pmax), z_ok = win16_safe(r, zb16, pmin, pmax);

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        const int cnt = n - o < 8 ? n - o : 8;
        const bool contig = cnt == 8 && grp_contig(b, o);
        const uint16x8_t lanemask = vcltq_u16(iota, vdupq_n_u16((uint16_t)cnt));
        const uint8x8_t cvg8 = vld1_u8(&b->cvg[o]);

        /* ---- Z interpolant ---- */
        int32x4_t szv[2];
        {
            int32x4_t v0 = vandq_s32(vshrq_n_s32(vld1q_s32(&b->cz[o]), 10), vdupq_n_s32(0x3fffff));
            int32x4_t v1 = vandq_s32(vshrq_n_s32(vld1q_s32(&b->cz[o + 4]), 10), vdupq_n_s32(0x3fffff));
            if (vminv_u8(cvg8) == 8) {
                szv[0] = vshrq_n_s32(v0, 3); szv[1] = vshrq_n_s32(v1, 3);
            } else {
                uint16x8_t ox = vmovl_u8(vld1_u8(&b->offx[o])), oy = vmovl_u8(vld1_u8(&b->offy[o]));
                int32x4_t oxl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(ox)));
                int32x4_t oxh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(ox)));
                int32x4_t oyl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(oy)));
                int32x4_t oyh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(oy)));
                szv[0] = vshrq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vshlq_n_s32(v0, 2), oxl, pc->cdz), oyl, pc->dzdy), 5);
                szv[1] = vshrq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vshlq_n_s32(v1, 2), oxh, pc->cdz), oyh, pc->dzdy), 5);
            }
            for (int h = 0; h < 2; h++) {
                int32x4_t t = vandq_s32(vshrq_n_s32(szv[h], 17), vdupq_n_s32(3));
                int32x4_t masked = vandq_s32(szv[h], vdupq_n_s32(0x3ffff));
                int32x4_t r2 = vbslq_s32(vceqq_s32(t, vdupq_n_s32(2)), vdupq_n_s32(0x3ffff), masked);
                szv[h] = vbslq_s32(vceqq_s32(t, vdupq_n_s32(3)), vdupq_n_s32(0), r2);
            }
            vst1q_s32(&b->sz[o], szv[0]); vst1q_s32(&b->sz[o + 4], szv[1]);
        }

        /* ---- colour buffer read ---- */
        uint8x8_t memcvg8 = vdup_n_u8(7);
        if (S.rd) {
            int16x8_t mr, mg, mb, ma;
            if (is16) {
                uint16x8_t px; uint8x8_t hv;
                ld16(r, fb16, b, o, cnt, contig, fb_ok, &px, &hv);
                int16x8_t f = vreinterpretq_s16_u16(px);
                int16x8_t low = vorrq_s16(vshlq_n_s16(vandq_s16(f, vdupq_n_s16(1)), 2),
                                          vreinterpretq_s16_u16(vmovl_u8(hv)));
                if (S.rgba) {
                    mr = vreinterpretq_s16_u16(vandq_u16(vshrq_n_u16(px, 8), vdupq_n_u16(0xf8)));
                    mg = vreinterpretq_s16_u16(vshrq_n_u16(vandq_u16(px, vdupq_n_u16(0x7c0)), 3));
                    mb = vreinterpretq_s16_u16(vshlq_n_u16(vandq_u16(px, vdupq_n_u16(0x3e)), 2));
                    memcvg8 = vmovn_u16(vreinterpretq_u16_s16(low));
                    ma = vshlq_n_s16(low, 5);
                } else {
                    mr = mg = mb = vreinterpretq_s16_u16(vshrq_n_u16(px, 8));
                    low = vandq_s16(vshrq_n_s16(f, 5), vdupq_n_s16(7));
                    memcvg8 = vmovn_u16(vreinterpretq_u16_s16(low));
                    ma = vshlq_n_s16(low, 5);
                }
            } else {
                uint32x4_t lo, hi;
                ld32(r, fb32, b, o, cnt, contig, &lo, &hi);
                mr = vcombine_s16(vmovn_s32(vreinterpretq_s32_u32(vshrq_n_u32(lo, 24))), vmovn_s32(vreinterpretq_s32_u32(vshrq_n_u32(hi, 24))));
                mg = vcombine_s16(vmovn_s32(vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(lo, 16), vdupq_n_u32(0xff)))),
                                  vmovn_s32(vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(hi, 16), vdupq_n_u32(0xff)))));
                mb = vcombine_s16(vmovn_s32(vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(lo, 8), vdupq_n_u32(0xff)))),
                                  vmovn_s32(vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(hi, 8), vdupq_n_u32(0xff)))));
                uint32x4_t cl = vandq_u32(vshrq_n_u32(lo, 5), vdupq_n_u32(7)), ch = vandq_u32(vshrq_n_u32(hi, 5), vdupq_n_u32(7));
                memcvg8 = vmovn_u16(vcombine_u16(vmovn_u32(cl), vmovn_u32(ch)));
                ma = vcombine_s16(vmovn_s32(vreinterpretq_s32_u32(vandq_u32(lo, vdupq_n_u32(0xe0)))),
                                  vmovn_s32(vreinterpretq_s32_u32(vandq_u32(hi, vdupq_n_u32(0xe0)))));
            }
            vst1q_s16(&b->mem[CH_R][o], mr); vst1q_s16(&b->mem[CH_G][o], mg);
            vst1q_s16(&b->mem[CH_B][o], mb); vst1q_s16(&b->mem[CH_A][o], ma);
        } else {
            vst1q_s16(&b->mem[CH_R][o], vdupq_n_s16(0)); vst1q_s16(&b->mem[CH_G][o], vdupq_n_s16(0));
            vst1q_s16(&b->mem[CH_B][o], vdupq_n_s16(0)); vst1q_s16(&b->mem[CH_A][o], vdupq_n_s16(0xe0));
        }
        vst1_u8(&b->memcvg[o], memcvg8);

        /* ---- Z compare ---- */
        const uint16x8_t memcvg16 = vmovl_u8(memcvg8);
        const uint16x8_t cvg16 = vmovl_u8(cvg8);
        uint32x4_t wen[2], ben[2], pre[2], ncvg[2];
        int32x4_t sha[2], shb[2];
        const uint16x8_t ovf16 = vtstq_u16(vaddq_u16(memcvg16, cvg16), vdupq_n_u16(8));
        if (fast_z && (relaxed_z || vminvq_u16(vorrq_u16(ovf16, vmvnq_u16(lanemask))) == 0xffff)) {
            /* Plain depth compare (opaque / transparent modes). Exact when every lane has "overflowing"
             * coverage (the delta-Z logic only matters for partially covered memory pixels); with
             * RelaxedPrecision the delta-Z terms are dropped for the remaining lanes as well. */
            uint16x8_t zpx;
            uint8x8_t zhv = vdup_n_u8(0);
            if (need_shifts) ld16(r, zb16, b, o, cnt, contig, z_ok, &zpx, &zhv);
            else zpx = fast_z_load(r, zb16, b, o, cnt, contig, z_ok);
            uint32x4_t wl, wh, il, ih;
            for (int h = 0; h < 2; h++) {
                uint32x4_t zv = vmovl_u16(h ? vget_high_u16(zpx) : vget_low_u16(zpx));
                int32x4_t e = vreinterpretq_s32_u32(vshrq_n_u32(zv, 13));
                uint32x4_t mant = vandq_u32(vshrq_n_u32(zv, 2), vdupq_n_u32(0x7ff));
                uint32x4_t add = vsubq_u32(vdupq_n_u32(0x40000), vshlq_u32(vdupq_n_u32(0x40000), vnegq_s32(e)));
                uint32x4_t oz = vaddq_u32(vshlq_u32(mant, vmaxq_s32(vsubq_s32(vdupq_n_s32(6), e), vdupq_n_s32(0))), add);
                uint32x4_t inf = vcltq_u32(vreinterpretq_u32_s32(szv[h]), oz);
                uint32x4_t w = vorrq_u32(vceqq_u32(oz, vdupq_n_u32(0x3ffff)), inf);
                if (h) { wh = w; ih = inf; } else { wl = w; il = inf; }
            }
            uint16x8_t w16 = vandq_u16(vcombine_u16(vmovn_u32(wl), vmovn_u32(wh)), lanemask);
#ifdef FOIR_STATS
            if (vmaxvq_u16(w16) == 0) NEON_HANDLED(7); else NEON_FALLBACK(7);
#endif
            vst1_u8(&b->wen[o], vmovn_u16(vandq_u16(w16, vdupq_n_u16(1))));
            uint16x8_t ben16 = vdupq_n_u16(0);
            if (S.force) ben16 = vdupq_n_u16(1);
            else if (relaxed_z && S.aa)
                ben16 = vandq_u16(vandq_u16(vmvnq_u16(ovf16), vmvnq_u16(vcombine_u16(vmovn_u32(il), vmovn_u32(ih)))), vdupq_n_u16(1));
            vst1_u8(&b->blend_en[o], vmovn_u16(ben16));
            vst1_u8(&b->prewrap[o], vmovn_u16(vandq_u16(ovf16, vdupq_n_u16(1))));
            if (need_shifts) {
                /* blender shifts from the memory pixel's delta-Z exponent */
                int16x8_t rd = vreinterpretq_s16_u16(vorrq_u16(vshlq_n_u16(vandq_u16(zpx, vdupq_n_u16(3)), 2), vmovl_u8(zhv)));
                int16x8_t enc16 = vdupq_n_s16((int16_t)dzpixenc);
                int16x8_t sha8 = vminq_s16(vmaxq_s16(vsubq_s16(enc16, rd), vdupq_n_s16(0)), vdupq_n_s16(4));
                int16x8_t shb8 = vminq_s16(vmaxq_s16(vsubq_s16(rd, enc16), vdupq_n_s16(0)), vdupq_n_s16(4));
                vst1_s8(&b->blshifta[o], vmovn_s16(sha8));
                vst1_s8(&b->blshiftb[o], vmovn_s16(shb8));
            } else {
                vst1_s8(&b->blshifta[o], vdup_n_s8(0));
                vst1_s8(&b->blshiftb[o], vdup_n_s8(0));
            }
            NEON_HANDLED(6);
            continue;
        }
        if (S.zc) NEON_FALLBACK(6);
        if (S.zc) {
            uint16x8_t zpx; uint8x8_t zhv;
            ld16(r, zb16, b, o, cnt, contig, z_ok, &zpx, &zhv);
            uint16x8_t hv16 = vmovl_u8(zhv);
            struct z4_out z0, z1;
            z_compare4(r, &S, vreinterpretq_u32_s32(szv[0]), vmovl_u16(vget_low_u16(zpx)), vmovl_u16(vget_low_u16(hv16)),
                       vmovl_u16(vget_low_u16(cvg16)), vmovl_u16(vget_low_u16(memcvg16)), &z0);
            z_compare4(r, &S, vreinterpretq_u32_s32(szv[1]), vmovl_u16(vget_high_u16(zpx)), vmovl_u16(vget_high_u16(hv16)),
                       vmovl_u16(vget_high_u16(cvg16)), vmovl_u16(vget_high_u16(memcvg16)), &z1);
            wen[0] = z0.wen; wen[1] = z1.wen; ben[0] = z0.blend_en; ben[1] = z1.blend_en;
            pre[0] = z0.prewrap; pre[1] = z1.prewrap; ncvg[0] = z0.cvg; ncvg[1] = z1.cvg;
            sha[0] = z0.sha; sha[1] = z1.sha; shb[0] = z0.shb; shb[1] = z1.shb;
        } else {
            for (int h = 0; h < 2; h++) {
                uint32x4_t mc = vmovl_u16(h ? vget_high_u16(memcvg16) : vget_low_u16(memcvg16));
                uint32x4_t cv = vmovl_u16(h ? vget_high_u16(cvg16) : vget_low_u16(cvg16));
                uint32x4_t overflow = vtstq_u32(vaddq_u32(mc, cv), vdupq_n_u32(8));
                wen[h] = vdupq_n_u32(~0u);
                ben[h] = S.force ? vdupq_n_u32(~0u) : (S.aa ? vmvnq_u32(overflow) : vdupq_n_u32(0));
                pre[h] = overflow; ncvg[h] = cv;
                sha[h] = vdupq_n_s32(0);
                shb[h] = vdupq_n_s32(dzpixenc < 0xb ? 4 : 0xf - dzpixenc);
            }
        }
        uint16x8_t wen16 = vandq_u16(vcombine_u16(vmovn_u32(wen[0]), vmovn_u32(wen[1])), lanemask);
#ifdef FOIR_STATS
        if (S.zc) { if (vmaxvq_u16(wen16) == 0) NEON_HANDLED(7); else NEON_FALLBACK(7); }
#endif
        vst1_u8(&b->wen[o], vmovn_u16(vandq_u16(wen16, vdupq_n_u16(1))));
        vst1_u8(&b->blend_en[o], vmovn_u16(vandq_u16(vcombine_u16(vmovn_u32(ben[0]), vmovn_u32(ben[1])), vdupq_n_u16(1))));
        vst1_u8(&b->prewrap[o], vmovn_u16(vandq_u16(vcombine_u16(vmovn_u32(pre[0]), vmovn_u32(pre[1])), vdupq_n_u16(1))));
        vst1_s8(&b->blshifta[o], vmovn_s16(vcombine_s16(vmovn_s32(sha[0]), vmovn_s32(sha[1]))));
        vst1_s8(&b->blshiftb[o], vmovn_s16(vcombine_s16(vmovn_s32(shb[0]), vmovn_s32(shb[1]))));
        if (S.zc && S.zmode == ZMODE_INTERPENETRATING)
            vst1_u8(&b->cvg[o], vmovn_u16(vcombine_u16(vmovn_u32(ncvg[0]), vmovn_u32(ncvg[1]))));
    }
}

void neon_zcmp(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    if (r->fb_size < PIXEL_SIZE_16BIT) { NEON_FALLBACK(2); foir_kernels_scalar.zcmp(r, b); return; }
    zcmp_body(r, b, (struct zs){ .is16 = r->fb_size == PIXEL_SIZE_16BIT, .rd = om->image_read_en, .rgba = r->fb_format == FORMAT_RGBA,
                                 .zc = om->z_compare_en, .zmode = om->z_mode, .aa = om->antialias_en, .force = om->force_blend,
                                 .shifts = om->real_blendshifts, .relaxed = r->cfg.relaxed_precision });
}

/* ---- store ---------------------------------------------------------------------------------------- */

/* Z value encoding (the inverse of the decode in z_compare4); sz is 18 bits */
FOIR_INLINE uint32x4_t z_encode4(uint32x4_t sz)
{
    int32x4_t lead = vreinterpretq_s32_u32(vclzq_u32(vmvnq_u32(vshlq_n_u32(sz, 14))));
    int32x4_t e = vminq_s32(lead, vdupq_n_s32(7));
    int32x4_t e6 = vminq_s32(lead, vdupq_n_s32(6));
    uint32x4_t shifted = vshlq_u32(sz, vsubq_s32(e6, vdupq_n_s32(4)));
    return vorrq_u32(vandq_u32(shifted, vdupq_n_u32(0x1ffc)), vshlq_n_u32(vreinterpretq_u32_s32(e), 13));
}

/* the state the store kernel depends on (generic: from the render state; variants: literal) */
struct ss { int is16, rgba, cvgdest, zupd; };

FOIR_INLINE void store_body(struct rdp *r, struct px_batch *b, const struct ss S)
{
    NEON_HANDLED(4);

    const int n = b->n, ng = NGROUPS(n);
    const uint32_t fb16 = r->fb_address >> 1, fb32 = r->fb_address >> 2, zb16 = r->zb_address >> 1;
    const int is16 = S.is16;
    const int32_t enc = r->pc.dzpixenc;
    uint32_t pmin, pmax;
    batch_range(b, &pmin, &pmax);
    const bool fb_ok = win16_safe(r, fb16, pmin, pmax), z_ok = win16_safe(r, zb16, pmin, pmax);

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        const int cnt = n - o < 8 ? n - o : 8;
        uint16x8_t mask = vmovl_u8(vld1_u8(&b->wen[o]));
        mask = vreinterpretq_u16_s16(vnegq_s16(vreinterpretq_s16_u16(mask)));   /* 1 -> 0xffff */
        if (cnt < 8) {
            static const uint16_t lane_iota[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
            mask = vandq_u16(mask, vcltq_u16(vld1q_u16(lane_iota), vdupq_n_u16((uint16_t)cnt)));
        }
        if (vmaxvq_u16(mask) == 0) continue;
        const bool contig = cnt == 8 && grp_contig(b, o);

        int16x8_t fr = vld1q_s16(&b->fcol[0][o]), fg = vld1q_s16(&b->fcol[1][o]), fbl = vld1q_s16(&b->fcol[2][o]);

        /* final coverage */
        int16x8_t cvg = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(&b->cvg[o])));
        int16x8_t mcv = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(&b->memcvg[o])));
        uint16x8_t ben = vtstq_u16(vmovl_u8(vld1_u8(&b->blend_en[o])), vdupq_n_u16(1));
        int16x8_t fc;
        switch (S.cvgdest) {
        case CVG_CLAMP: {
            int16x8_t f = vbslq_s16(ben, vaddq_s16(cvg, mcv), vsubq_s16(cvg, vdupq_n_s16(1)));
            fc = vbslq_s16(vtstq_s16(f, vdupq_n_s16(8)), vdupq_n_s16(7), vandq_s16(f, vdupq_n_s16(7)));
            break;
        }
        case CVG_WRAP: fc = vandq_s16(vaddq_s16(cvg, mcv), vdupq_n_s16(7)); break;
        case CVG_ZAP:  fc = vdupq_n_s16(7); break;
        default:       fc = mcv; break;
        }

        if (is16) {
            int16x8_t color, hvv;
            if (S.rgba) {
                color = vorrq_s16(vorrq_s16(vshlq_n_s16(vandq_s16(fr, vdupq_n_s16(~7)), 8),
                                            vshlq_n_s16(vandq_s16(fg, vdupq_n_s16(~7)), 3)),
                                  vreinterpretq_s16_u16(vshrq_n_u16(vreinterpretq_u16_s16(vandq_s16(fbl, vdupq_n_s16(~7))), 2)));
                color = vorrq_s16(color, vshrq_n_s16(fc, 2));
                hvv = vandq_s16(fc, vdupq_n_s16(3));
            } else {
                color = vorrq_s16(vshlq_n_s16(fr, 8), vshlq_n_s16(fc, 5));
                hvv = vdupq_n_s16(0);
            }
            st16(r, fb16, b, o, cnt, contig, fb_ok, vreinterpretq_u16_s16(color), vmovn_u16(vreinterpretq_u16_s16(hvv)), mask);
        } else {
            int32x4_t cl = vshlq_n_s32(vmovl_s16(vget_low_s16(fc)), 5), ch = vshlq_n_s32(vmovl_s16(vget_high_s16(fc)), 5);
            uint32x4_t lo = vorrq_u32(vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(fr))), 24),
                                                vshlq_n_u32(vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(fg))), 16)),
                                      vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(fbl))), 8),
                                                vreinterpretq_u32_s32(cl)));
            uint32x4_t hi = vorrq_u32(vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(fr))), 24),
                                                vshlq_n_u32(vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(fg))), 16)),
                                      vorrq_u32(vshlq_n_u32(vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(fbl))), 8),
                                                vreinterpretq_u32_s32(ch)));
            st32(r, fb32, b, o, cnt, contig, lo, hi, mask);
        }

        if (S.zupd) {
            uint32x4_t e0 = z_encode4(vreinterpretq_u32_s32(vld1q_s32(&b->sz[o])));
            uint32x4_t e1 = z_encode4(vreinterpretq_u32_s32(vld1q_s32(&b->sz[o + 4])));
            uint16x8_t zv = vorrq_u16(vcombine_u16(vmovn_u32(e0), vmovn_u32(e1)), vdupq_n_u16((uint16_t)(enc >> 2)));
            st16(r, zb16, b, o, cnt, contig, z_ok, zv, vdup_n_u8((uint8_t)(enc & 3)), mask);
        }
    }
}

void neon_store(struct rdp *r, struct px_batch *b)
{
    if (r->fb_size < PIXEL_SIZE_16BIT) { NEON_FALLBACK(4); foir_kernels_scalar.store(r, b); return; }
    store_body(r, b, (struct ss){ .is16 = r->fb_size == PIXEL_SIZE_16BIT, .rgba = r->fb_format == FORMAT_RGBA,
                                  .cvgdest = r->om.cvg_dest, .zupd = r->om.z_update_en });
}

/* ---- FILL ---------------------------------------------------------------------------------------------- */

void neon_fill(struct rdp *r, int y, int x0, int x1)
{
    uint32_t base = (uint32_t)(r->fb_width * y);
    if (r->fb_size == PIXEL_SIZE_32BIT) {
        uint32_t i0 = ((r->fb_address >> 2) + base + (uint32_t)x0) & (RDRAM_MASK >> 2);
        int n = x1 - x0 + 1;
        if (n >= 4 && ((i0 + (uint32_t)n) << 2) <= r->rdram_size && i0 + (uint32_t)n <= (RDRAM_MASK >> 2)) {
            uint32_t *w = (uint32_t *)r->rdram + i0;
            uint32x4_t v = vdupq_n_u32(r->fill_color);
            int i = 0;
            for (; i + 4 <= n; i += 4) vst1q_u32(w + i, v);
            for (; i < n; i++) w[i] = r->fill_color;
            return;
        }
    } else if (r->fb_size == PIXEL_SIZE_16BIT) {
        uint32_t i0 = ((r->fb_address >> 1) + base + (uint32_t)x0) & (RDRAM_MASK >> 1);
        int n = x1 - x0 + 1;
        if (n >= 16 && ((i0 + (uint32_t)n + 2) << 1) <= r->rdram_size && i0 + (uint32_t)n + 2 <= (RDRAM_MASK >> 1)) {
            int i = 0;
            /* head: odd pixel first */
            for (; (i0 + (uint32_t)i) & 1; i++) {
                uint32_t hi = i0 + (uint32_t)i;
                uint16_t val = (hi & 1) ? (uint16_t)(r->fill_color & 0xffff) : (uint16_t)(r->fill_color >> 16);
                rdram_wr_pair16(r, hi, val, (uint8_t)(((val & 1) << 1) | (val & 1)));
            }
            /* body: whole 32-bit host words hold (odd pixel = low half, even pixel = high half) */
            uint32_t *w = (uint32_t *)r->rdram + ((i0 + (uint32_t)i) >> 1);
            uint8_t he = (uint8_t)((r->fill_color >> 16) & 1 ? 0xf : 0x8), ho = (uint8_t)((r->fill_color & 1) ? 0xf : 0x8);
            uint8_t *hid = r->hidden + i0 + (uint32_t)i;
            uint32x4_t v = vdupq_n_u32(r->fill_color);
            uint8x8_t hp = vreinterpret_u8_u16(vdup_n_u16((uint16_t)(he | (ho << 8))));
            int pairs = (n - i) >> 1;
            int p = 0;
            for (; p + 4 <= pairs; p += 4) {
                vst1q_u32(w + p, v);
                vst1_u8(hid + 2 * p, hp);
            }
            for (; p < pairs; p++) { w[p] = r->fill_color; hid[2 * p] = he; hid[2 * p + 1] = ho; }
            i += pairs * 2;
            for (; i < n; i++) {
                uint32_t hi = i0 + (uint32_t)i;
                uint16_t val = (hi & 1) ? (uint16_t)(r->fill_color & 0xffff) : (uint16_t)(r->fill_color >> 16);
                rdram_wr_pair16(r, hi, val, (uint8_t)(((val & 1) << 1) | (val & 1)));
            }
            return;
        }
    }
    NEON_FALLBACK(5);
    foir_kernels_scalar.fill(r, y, x0, x1);
}

#include "kspec_mem.inc"
