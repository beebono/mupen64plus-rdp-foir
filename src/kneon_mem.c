/*
 * mupen64plus-rdp-foir - kneon_mem.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kneon.h"
#include "spec.h"

#if FOIR_HALF_XOR != 1 || FOIR_BYTE_XOR != 3
#error "the NEON memory kernels assume a little-endian host with word-swapped RDRAM"
#endif

/* every lane of an 8-bit mask set: a 64-bit move and compare, not a (slow, long-latency) across-lanes reduction */
FOIR_INLINE bool mask8_all(uint8x8_t m) { return vget_lane_u64(vreinterpret_u64_u8(m), 0) == ~0ull; }

/* 0 / 0xffff lanes -> 0 / 1 bytes */
FOIR_INLINE uint8x8_t mask_bit(uint16x8_t m) { return vshr_n_u8(vmovn_u16(m), 7); }

FOIR_INLINE bool grp_contig(const struct px_batch *b, int o)
{
    static const uint32_t iota0[4] = { 0, 1, 2, 3 }, iota1[4] = { 4, 5, 6, 7 };
    uint32x4_t a = vld1q_u32(&b->fbidx[o]), c = vld1q_u32(&b->fbidx[o + 4]);
    uint32x4_t base = vdupq_n_u32(b->fbidx[o]);
    uint32x4_t e0 = vceqq_u32(a, vaddq_u32(base, vld1q_u32(iota0)));
    uint32x4_t e1 = vceqq_u32(c, vaddq_u32(base, vld1q_u32(iota1)));
    return vminvq_u32(vandq_u32(e0, e1)) == 0xffffffffu;
}

/* How a group of eight lanes lies in the colour / Z buffer: one run of consecutive pixels (kind 1), the end of one scanline and the start of the
 * next, i.e. two runs (kind 2), or anything else (kind 0, per-lane access). Kind 2 reads a window at lane 0's pixel and one placed so that lane k
 * is the first pixel of the second run, and takes lanes < k from the first. */
struct grp { int kind; uint32_t a, bk; uint16x8_t lt; };

FOIR_INLINE void grp_classify(const struct px_batch *b, int o, int cnt, struct grp *g)
{
    g->kind = 0;
    if (cnt < 8) return;
    static const uint32_t iota0[4] = { 0, 1, 2, 3 }, iota1[4] = { 4, 5, 6, 7 };
    static const uint16_t iota16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const uint32_t a0 = b->fbidx[o];
    const uint32x4_t base = vdupq_n_u32(a0);
    const uint32x4_t d0 = vsubq_u32(vld1q_u32(&b->fbidx[o]), vaddq_u32(base, vld1q_u32(iota0)));
    const uint32x4_t d1 = vsubq_u32(vld1q_u32(&b->fbidx[o + 4]), vaddq_u32(base, vld1q_u32(iota1)));
    const uint64_t brk = vget_lane_u64(vreinterpret_u64_u8(vmovn_u16(vcombine_u16(vmovn_u32(vtstq_u32(d0, d0)), vmovn_u32(vtstq_u32(d1, d1))))), 0);
    if (FOIR_LIKELY(!brk)) { g->kind = 1; g->a = a0; return; }
    const int k = __builtin_ctzll(brk) >> 3;                          /* first lane that is not consecutive: 1..7 */
    const uint32_t b0 = b->fbidx[o + k];
    const uint32x4_t dl = vdupq_n_u32(b0 - (a0 + (uint32_t)k));
    const uint16x8_t eq = vcombine_u16(vmovn_u32(vceqq_u32(d0, dl)), vmovn_u32(vceqq_u32(d1, dl)));
    const uint16x8_t ge = vcgeq_u16(vld1q_u16(iota16), vdupq_n_u16((uint16_t)k));
    if (vget_lane_u64(vreinterpret_u64_u8(vmovn_u16(vbicq_u16(ge, eq))), 0)) return;   /* a lane >= k is off the second run */
    g->kind = 2; g->a = a0; g->bk = b0 - (uint32_t)k; g->lt = vmvnq_u16(ge);
}

/* hidden bits of eight pixels: the stored ones when the byte is valid and its LSB copy matches the pixel's, else both equal the pixel's LSB; *l4 = LSB << 2 */
FOIR_INLINE uint8x8_t hv_resolve(uint8x8_t h, uint16x8_t px, uint8x8_t *l4)
{
    uint8x8_t lsb = vand_u8(vmovn_u16(px), vdup_n_u8(1));
    *l4 = vshl_n_u8(lsb, 2);
    uint8x8_t use = vceq_u8(vand_u8(veor_u8(h, *l4), vdup_n_u8(0xc)), vdup_n_u8(8));      /* bit 3 set, bit 2 equal to the LSB */
    return vbsl_u8(use, vand_u8(h, vdup_n_u8(3)), vmul_u8(lsb, vdup_n_u8(3)));
}

FOIR_INLINE void batch_range(const struct px_batch *b, uint32_t *lo, uint32_t *hi)
{
    if (FOIR_LIKELY(b->fbrange)) { *lo = b->fbmin; *hi = b->fbmax; return; }
    uint32_t mn = ~0u, mx = 0;
    for (int i = 0; i < b->n; i++) { uint32_t v = b->fbidx[i]; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    *lo = mn; *hi = mx;
}

/* every window (and the frame an odd window rewrites) of the batch lies in RDRAM; base16 = halfword base of the buffer */
FOIR_INLINE bool win16_safe(const struct rdp *r, uint32_t base16, uint32_t lo, uint32_t hi)
{
    return base16 + lo >= 8 && base16 + hi + 16 < (RDRAM_MASK >> 1) && ((base16 + hi + 16) << 1) <= r->rdram_size;
}

/* RDRAM is held as host words: pixel i lives at halfword i ^ 1 */
FOIR_INLINE uint16x8_t win16_ld(const struct rdp *r, uint32_t i0)
{
    const uint16_t *h = (const uint16_t *)r->rdram;
    if (!(i0 & 1)) return vrev32q_u16(vld1q_u16(h + i0));
    uint16x8_t P0 = vrev32q_u16(vld1q_u16(h + i0 - 1)), P1 = vrev32q_u16(vld1q_u16(h + i0 + 7));
    return vextq_u16(P0, P1, 1);
}

/* an odd i0 rewrites the 16-pixel frame it overlaps; the window must lie in RDRAM and in one row */
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

FOIR_INLINE uint16x8_t win16_grp(const struct rdp *r, uint32_t base16, const struct grp *g)
{
    uint16x8_t w = win16_ld(r, base16 + g->a);
    if (g->kind == 2) w = vbslq_u16(g->lt, w, win16_ld(r, base16 + g->bk));
    return w;
}

FOIR_INLINE uint8x8_t hid_grp(const struct rdp *r, uint32_t base16, const struct grp *g)
{
    uint8x8_t h = vld1_u8(r->hidden + (base16 + g->a));
    if (g->kind == 2) h = vbsl_u8(vmovn_u16(g->lt), h, vld1_u8(r->hidden + (base16 + g->bk)));
    return h;
}

FOIR_INLINE uint16x8_t px16_load(const struct rdp *r, uint32_t base16, const struct px_batch *b, int o, int cnt, const struct grp *g, bool ok)
{
    if (ok && g->kind) return win16_grp(r, base16, g);
    uint16_t pa[8] ALIGN16;
    memset(pa, 0, sizeof(pa));
    for (int l = 0; l < cnt; l++) pa[l] = rdram_rd16(r, base16 + b->fbidx[o + l]);
    return vld1q_u16(pa);
}

/* the pixels and the raw hidden bytes of a group (hv_resolve() turns them into the hidden bits); a per-lane read hands back hidden bytes that resolve to what it read */
FOIR_INLINE void ld16_raw(const struct rdp *r, uint32_t base16, const struct px_batch *b, int o, int cnt, const struct grp *g, bool ok,
                          uint16x8_t *px, uint8x8_t *hraw)
{
    if (ok && g->kind) {
        *px = win16_grp(r, base16, g);
        *hraw = hid_grp(r, base16, g);
        return;
    }
    uint16_t pa[8] ALIGN16;
    uint8_t ha[8] ALIGN16;
    memset(pa, 0, sizeof(pa)); memset(ha, 0, sizeof(ha));
    for (int l = 0; l < cnt; l++) {
        rdram_rd_pair16(r, base16 + b->fbidx[o + l], &pa[l], &ha[l]);
        ha[l] |= (uint8_t)(8 | ((pa[l] & 1) << 2));
    }
    *px = vld1q_u16(pa);
    *hraw = vld1_u8(ha);
}

FOIR_INLINE void st16(struct rdp *r, uint32_t base16, const struct px_batch *b, int o, int cnt, bool contig, bool ok,
                      uint16x8_t px, uint8x8_t hv, uint16x8_t mask)
{
    if (ok && contig) {
        /* an odd-aligned group rewrites the whole 16-pixel frame; its other pixels must be in the same row (other rows belong to other workers) */
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

FOIR_INLINE uint32x4_t vsel_u32(uint32x4_t m, uint32x4_t a, uint32x4_t b) { return vbslq_u32(m, a, b); }

struct z4_out {
    uint32x4_t wen, blend_en, prewrap, cvg;
    int32x4_t sha, shb;
};

struct zs {
    int is16;
    int rd;
    int rgba;
    int zc;
    int zmode;
    int aa, force;
    int shifts;      /* the blender reads the memory alpha: delta-Z shifts needed */
    int relaxed;
};

/* Q = 0x40000 - oz for the Z memory value, oz being its decoded 18-bit depth. The format (exponent e, mantissa m) gives oz = 0x40000 - ((base - m) << max(6 - e, 0)),
 * base = 0x1000, or 0x800 for e = 7; so sz < oz <=> sz + Q < 0x40000, and oz == 0x3ffff (the farthest depth) <=> zpx >= 0xfffc. Eight lanes in 16-bit arithmetic. */
FOIR_INLINE void z_dist8(uint16x8_t zpx, uint32x4_t *qlo, uint32x4_t *qhi)
{
    const uint16x8_t e = vshrq_n_u16(zpx, 13);
    const uint16x8_t mant = vandq_u16(vshrq_n_u16(zpx, 2), vdupq_n_u16(0x7ff));
    const int16x8_t sh = vmaxq_s16(vsubq_s16(vdupq_n_s16(6), vreinterpretq_s16_u16(e)), vdupq_n_s16(0));
    const uint16x8_t pw = vshlq_u16(vdupq_n_u16(1), sh);
    const uint16x8_t base = vsubq_u16(vdupq_n_u16(0x1000), vandq_u16(vceqq_u16(e, vdupq_n_u16(7)), vdupq_n_u16(0x800)));
    const uint16x8_t mp = vsubq_u16(base, mant);
    *qlo = vmull_u16(vget_low_u16(mp), vget_low_u16(pw));
    *qhi = vmull_high_u16(mp, pw);
}

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

/* The Z compare of eight lanes for the opaque, transparent and decal modes, in 16-bit arithmetic wherever the values fit (the interpenetrating mode also
 * rewrites the coverage and stays on z_compare4). Same decisions as z_compare4, with oz replaced by Q = 0x40000 - oz (see z_dist8). Out of line: it only serves
 * the exact path for partially covered pixels and the decal mode, and inlined it costs the RelaxedPrecision loop 1-2 % in register allocation. */
static __attribute__((noinline)) void z_compare8(const struct rdp *r, const struct zs *S, struct px_batch *b, int o, const uint32x4_t szv[2], uint16x8_t zpx,
                                                  uint8x8_t zhv, uint16x8_t ovf16, uint16x8_t lanemask)
{
    const int32_t dzpixenc = r->pc.dzpixenc;
    const uint16x8_t one = vdupq_n_u16(1);
    /* the memory pixel's delta-Z exponent and the blender shifts it implies */
    const uint8x8_t rd8 = vorr_u8(vshl_n_u8(vand_u8(vmovn_u16(zpx), vdup_n_u8(3)), 2), zhv);
    const uint8x8_t enc8 = vdup_n_u8((uint8_t)dzpixenc), four = vdup_n_u8(4);
    vst1_u8((uint8_t *)&b->blshifta[o], vmin_u8(vqsub_u8(enc8, rd8), four));
    vst1_u8((uint8_t *)&b->blshiftb[o], vmin_u8(vqsub_u8(rd8, enc8), four));

    /* delta-Z of the memory pixel, widened for the low precision exponents, ORed with the primitive's; its highest bit is the compare's delta-Z */
    const uint16x8_t e = vshrq_n_u16(zpx, 13);
    uint16x8_t dzmem = vshlq_u16(one, vreinterpretq_s16_u16(vmovl_u8(rd8)));
    const uint16x8_t lt3 = vcltq_u16(e, vdupq_n_u16(3));
    const uint16x8_t is8000 = vceqq_u16(dzmem, vdupq_n_u16(0x8000));
    const uint16x8_t dz2 = vmaxq_u16(vshlq_n_u16(dzmem, 1), vshlq_u16(vdupq_n_u16(16), vnegq_s16(vreinterpretq_s16_u16(e))));
    const uint16x8_t force_cop = vandq_u16(lt3, is8000);
    dzmem = vbslq_u16(lt3, vbslq_u16(is8000, vdupq_n_u16(0xffff), dz2), dzmem);
    const uint16x8_t dzor = vorrq_u16(vdupq_n_u16((uint16_t)r->pc.dzpix), dzmem);
    const uint16x8_t dznot = vshlq_u16(one, vsubq_s16(vdupq_n_s16(15), vreinterpretq_s16_u16(vclzq_u16(dzor))));
    uint32x4_t ql, qh;
    z_dist8(zpx, &ql, &qh);
    const uint32x4_t lim = vdupq_n_u32(0x40000);
    const uint32x4_t tl = vaddq_u32(szv[0], ql), th = vaddq_u32(szv[1], qh);                       /* sz + Q: sz < oz <=> sz + Q < 0x40000 */
    const uint32x4_t dnl = vshll_n_u16(vget_low_u16(dznot), 3), dnh = vshll_high_n_u16(dznot, 3);  /* delta-Z of the compare << 3 */
    const uint16x8_t infront = vcombine_u16(vmovn_u32(vcltq_u32(tl, lim)), vmovn_u32(vcltq_u32(th, lim)));
    const uint16x8_t maxz = vcgeq_u16(zpx, vdupq_n_u16(0xfffc));
    const uint16x8_t farther = vorrq_u16(force_cop, vcombine_u16(vmovn_u32(vcgeq_u32(vaddq_u32(tl, dnl), lim)), vmovn_u32(vcgeq_u32(vaddq_u32(th, dnh), lim))));
    const uint16x8_t nearer = vorrq_u16(force_cop, vcombine_u16(vmovn_u32(vcleq_s32(vsubq_s32(vreinterpretq_s32_u32(tl), vreinterpretq_s32_u32(dnl)), vreinterpretq_s32_u32(lim))),
                                                                 vmovn_u32(vcleq_s32(vsubq_s32(vreinterpretq_s32_u32(th), vreinterpretq_s32_u32(dnh)), vreinterpretq_s32_u32(lim)))));
    uint16x8_t wen;
    switch (S->zmode) {
    case ZMODE_OPAQUE:
        wen = vorrq_u16(maxz, vbslq_u16(ovf16, infront, nearer));
        break;
    case ZMODE_TRANSPARENT:
        wen = vorrq_u16(infront, maxz);
        break;
    default:
        wen = vbicq_u16(vandq_u16(farther, nearer), maxz);
        break;
    }
    uint8x8_t ben8 = vdup_n_u8(0);
    if (S->force) ben8 = vdup_n_u8(1);
    else if (S->aa) ben8 = mask_bit(vbicq_u16(farther, ovf16));
    vst1_u8(&b->wen[o], mask_bit(vandq_u16(wen, lanemask)));
    vst1_u8(&b->blend_en[o], ben8);
    vst1_u8(&b->prewrap[o], mask_bit(ovf16));
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
    const int fast_z = S.zc && (S.zmode == ZMODE_OPAQUE || S.zmode == ZMODE_TRANSPARENT);
    const int need_shifts = S.shifts;
    const int relaxed_z = S.relaxed;
    const int z_hidden = S.zc && (need_shifts || !fast_z || !relaxed_z);      /* the delta-Z bits are read (shifts, or the exact path for partially covered pixels) */
    uint32_t pmin, pmax;
    batch_range(b, &pmin, &pmax);
    const bool fb_ok = win16_safe(r, fb16, pmin, pmax), z_ok = win16_safe(r, zb16, pmin, pmax);

    for (int gi = 0; gi < ng; gi++) {
        const int o = gi * 8;
        const int cnt = n - o < 8 ? n - o : 8;
        struct grp g;
        grp_classify(b, o, cnt, &g);
        const bool contig = g.kind == 1;
        const uint16x8_t lanemask = cnt == 8 ? vdupq_n_u16(0xffff) : vcltq_u16(iota, vdupq_n_u16((uint16_t)cnt));
        const uint8x8_t cvg8 = vld1_u8(&b->cvg[o]);

        /* the buffer reads first: the A55 does not reorder, so their latency hides behind the arithmetic that follows */
        uint16x8_t fbpx = vdupq_n_u16(0), zpx = vdupq_n_u16(0);
        uint8x8_t fbh = vdup_n_u8(0), zh = vdup_n_u8(0);
        if (S.rd && is16) {
            if (S.rgba) ld16_raw(r, fb16, b, o, cnt, &g, fb_ok, &fbpx, &fbh);
            else fbpx = px16_load(r, fb16, b, o, cnt, &g, fb_ok);
        }
        if (S.zc) {
            if (z_hidden) ld16_raw(r, zb16, b, o, cnt, &g, z_ok, &zpx, &zh);
            else zpx = px16_load(r, zb16, b, o, cnt, &g, z_ok);
        }

        uint32x4_t szv[2];
        {
            /* sz = 19-bit field of the interpolated Z (bits 17 / 18: 2 -> 0x3ffff, 3 -> 0, else the low 18 bits) */
            const uint32x4_t m19 = vdupq_n_u32(0x7ffff);
            uint32x4_t s0, s1;
            if (mask8_all(vceq_u8(cvg8, vdup_n_u8(8)))) {
                s0 = vshrq_n_u32(vreinterpretq_u32_s32(vld1q_s32(&b->cz[o])), 13);
                s1 = vshrq_n_u32(vreinterpretq_u32_s32(vld1q_s32(&b->cz[o + 4])), 13);
            } else {
                int32x4_t v0 = vandq_s32(vshrq_n_s32(vld1q_s32(&b->cz[o]), 10), vdupq_n_s32(0x3fffff));
                int32x4_t v1 = vandq_s32(vshrq_n_s32(vld1q_s32(&b->cz[o + 4]), 10), vdupq_n_s32(0x3fffff));
                uint16x8_t ox = vmovl_u8(vld1_u8(&b->offx[o])), oy = vmovl_u8(vld1_u8(&b->offy[o]));
                int32x4_t oxl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(ox)));
                int32x4_t oxh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(ox)));
                int32x4_t oyl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(oy)));
                int32x4_t oyh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(oy)));
                s0 = vandq_u32(vreinterpretq_u32_s32(vshrq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vshlq_n_s32(v0, 2), oxl, pc->cdz), oyl, pc->dzdy), 5)), m19);
                s1 = vandq_u32(vreinterpretq_u32_s32(vshrq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vshlq_n_s32(v1, 2), oxh, pc->cdz), oyh, pc->dzdy), 5)), m19);
            }
            const uint32x4_t lim = vdupq_n_u32(0x60000), top = vdupq_n_u32(0x3ffff);
            szv[0] = vandq_u32(vminq_u32(s0, top), vcltq_u32(s0, lim));
            szv[1] = vandq_u32(vminq_u32(s1, top), vcltq_u32(s1, lim));
            vst1q_u32((uint32_t *)&b->sz[o], szv[0]); vst1q_u32((uint32_t *)&b->sz[o + 4], szv[1]);
        }

        uint8x8_t memcvg8 = vdup_n_u8(7);
        if (S.rd) {
            int16x8_t mr, mg, mb, ma;
            if (is16) {
                const uint16x8_t px = fbpx;
                uint8x8_t l4, low8;
                if (S.rgba) {
                    low8 = vorr_u8(hv_resolve(fbh, px, &l4), l4);                   /* memory coverage: LSB, hidden bits */
                    mr = vreinterpretq_s16_u16(vandq_u16(vshrq_n_u16(px, 8), vdupq_n_u16(0xf8)));
                    mg = vreinterpretq_s16_u16(vshrq_n_u16(vandq_u16(px, vdupq_n_u16(0x7c0)), 3));
                    mb = vreinterpretq_s16_u16(vshlq_n_u16(vandq_u16(px, vdupq_n_u16(0x3e)), 2));
                } else {
                    low8 = vmovn_u16(vandq_u16(vshrq_n_u16(px, 5), vdupq_n_u16(7)));
                    mr = mg = mb = vreinterpretq_s16_u16(vshrq_n_u16(px, 8));
                }
                memcvg8 = low8;
                ma = vreinterpretq_s16_u16(vshll_n_u8(low8, 5));
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

        const uint16x8_t memcvg16 = vmovl_u8(memcvg8);
        const uint16x8_t cvg16 = vmovl_u8(cvg8);
        uint32x4_t wen[2], ben[2], pre[2], ncvg[2];
        int32x4_t sha[2], shb[2];
        const uint16x8_t ovf16 = vtstq_u16(vaddq_u16(memcvg16, cvg16), vdupq_n_u16(8));
        if (fast_z && (relaxed_z || mask8_all(vmovn_u16(vorrq_u16(ovf16, vmvnq_u16(lanemask)))))) {
            /* exact when every lane has overflowing coverage (delta-Z only matters for partially covered memory pixels); RelaxedPrecision drops the delta-Z terms for the rest too */
            uint8x8_t l4_;
            const uint8x8_t zhv = need_shifts ? hv_resolve(zh, zpx, &l4_) : vdup_n_u8(0);
            uint32x4_t ql, qh;
            z_dist8(zpx, &ql, &qh);
            const uint32x4_t lim = vdupq_n_u32(0x40000);
            const uint16x8_t inf16 = vcombine_u16(vmovn_u32(vcltq_u32(vaddq_u32(szv[0], ql), lim)), vmovn_u32(vcltq_u32(vaddq_u32(szv[1], qh), lim)));
            const uint16x8_t w16 = vandq_u16(vorrq_u16(inf16, vcgeq_u16(zpx, vdupq_n_u16(0xfffc))), lanemask);
#ifdef FOIR_STATS
            if (vmaxvq_u16(w16) == 0) NEON_HANDLED(7); else NEON_FALLBACK(7);
#endif
            vst1_u8(&b->wen[o], mask_bit(w16));
            uint8x8_t ben8 = vdup_n_u8(0);
            if (S.force) ben8 = vdup_n_u8(1);
            else if (relaxed_z && S.aa)
                ben8 = veor_u8(mask_bit(vorrq_u16(ovf16, inf16)), vdup_n_u8(1));
            vst1_u8(&b->blend_en[o], ben8);
            vst1_u8(&b->prewrap[o], mask_bit(ovf16));
            if (need_shifts) {
                /* rd = delta-Z exponent of the memory pixel (0..15); the shifts are its saturating differences to the primitive's, capped at 4 */
                uint8x8_t rd8 = vorr_u8(vshl_n_u8(vand_u8(vmovn_u16(zpx), vdup_n_u8(3)), 2), zhv);
                uint8x8_t enc8 = vdup_n_u8((uint8_t)dzpixenc), four = vdup_n_u8(4);
                vst1_u8((uint8_t *)&b->blshifta[o], vmin_u8(vqsub_u8(enc8, rd8), four));
                vst1_u8((uint8_t *)&b->blshiftb[o], vmin_u8(vqsub_u8(rd8, enc8), four));
            } else {
                vst1_s8(&b->blshifta[o], vdup_n_s8(0));
                vst1_s8(&b->blshiftb[o], vdup_n_s8(0));
            }
            NEON_HANDLED(6);
            continue;
        }
        if (S.zc) NEON_FALLBACK(6);
        if (S.zc && S.zmode != ZMODE_INTERPENETRATING) {
            uint8x8_t l4_;
            z_compare8(r, &S, b, o, szv, zpx, hv_resolve(zh, zpx, &l4_), ovf16, lanemask);
            continue;
        }
        if (S.zc) {
            uint8x8_t l4_;
            const uint8x8_t zhv = hv_resolve(zh, zpx, &l4_);
            uint16x8_t hv16 = vmovl_u8(zhv);
            struct z4_out z0, z1;
            z_compare4(r, &S, szv[0], vmovl_u16(vget_low_u16(zpx)), vmovl_u16(vget_low_u16(hv16)),
                       vmovl_u16(vget_low_u16(cvg16)), vmovl_u16(vget_low_u16(memcvg16)), &z0);
            z_compare4(r, &S, szv[1], vmovl_u16(vget_high_u16(zpx)), vmovl_u16(vget_high_u16(hv16)),
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

/* inverse of the decode in z_compare4; sz is 18 bits */
FOIR_INLINE uint32x4_t z_encode4(uint32x4_t sz)
{
    int32x4_t lead = vreinterpretq_s32_u32(vclzq_u32(vmvnq_u32(vshlq_n_u32(sz, 14))));
    int32x4_t e = vminq_s32(lead, vdupq_n_s32(7));
    int32x4_t e6 = vminq_s32(lead, vdupq_n_s32(6));
    uint32x4_t shifted = vshlq_u32(sz, vsubq_s32(e6, vdupq_n_s32(4)));
    return vorrq_u32(vandq_u32(shifted, vdupq_n_u32(0x1ffc)), vshlq_n_u32(vreinterpretq_u32_s32(e), 13));
}

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
            for (; (i0 + (uint32_t)i) & 1; i++) {
                uint32_t hi = i0 + (uint32_t)i;
                uint16_t val = (hi & 1) ? (uint16_t)(r->fill_color & 0xffff) : (uint16_t)(r->fill_color >> 16);
                rdram_wr_pair16(r, hi, val, (uint8_t)(((val & 1) << 1) | (val & 1)));
            }
            /* host words: odd pixel = low half, even pixel = high half */
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
