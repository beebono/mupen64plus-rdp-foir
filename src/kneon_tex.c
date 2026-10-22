/*
 * mupen64plus-rdp-foir - kneon_tex.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kneon.h"
#include "spec.h"
#ifdef FOIR_STATS
#include <stdio.h>
#include <stdlib.h>
static uint64_t lst_batches[4096], lst_pixels[4096], lst_groups[4096];
static uint64_t lst_dist[3][9], lst_distu[9], lst_fmt[8][4], lst_generic, lst_lodfb, lst_same12, lst_xhalves, lst_xslow;
#define LST_ADD(x, n) __atomic_fetch_add(&(x), (n), __ATOMIC_RELAXED)
static void lst_dump(void)
{
    uint64_t tb = 0, tp = 0;
    for (int i = 0; i < 4096; i++) { tb += lst_batches[i]; tp += lst_pixels[i]; }
    fprintf(stderr, "LODSTAT texture batches %llu pixels %llu; generic kernel (neon_texture entries from tex_generic) %llu, of which chain mismatch %llu\n",
            (unsigned long long)tb, (unsigned long long)tp, (unsigned long long)lst_generic, (unsigned long long)lst_lodfb);
    for (int i = 0; i < 4096; i++) {
        if (!lst_batches[i]) continue;
        fprintf(stderr, "LODSTAT mode two=%d persp=%d smp=%d tlut=%d lodt=%d dolod=%d detail=%d sharpen=%d any=%d nx=%d t1=%d : batches %llu pixels %llu groups %llu\n",
                i & 1, (i >> 1) & 1, (i >> 2) & 1, (i >> 3) & 1, (i >> 4) & 1, (i >> 5) & 1, (i >> 6) & 1, (i >> 7) & 1, (i >> 8) & 1, (i >> 9) & 1, (i >> 10) & 1,
                (unsigned long long)lst_batches[i], (unsigned long long)lst_pixels[i], (unsigned long long)lst_groups[i]);
    }
    static const char *kn[] = { "t1 (cycle 0 tiles)", "t2 (cycle 1 tiles)", "nx (next pixel tiles)" };
    for (int k = 0; k < 3; k++) {
        fprintf(stderr, "LODSTAT distinct %s per group:", kn[k]);
        for (int j = 1; j < 9; j++) fprintf(stderr, " %d:%llu", j, (unsigned long long)lst_dist[k][j]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "LODSTAT distinct tiles of t1 U t2 per group:");
    for (int j = 1; j < 9; j++) fprintf(stderr, " %d:%llu", j, (unsigned long long)lst_distu[j]);
    fprintf(stderr, "\nLODSTAT persp x-neighbour halves (4 lanes) %llu, of which not all lanes had the neighbour in the adjacent lane (divided again) %llu\n", (unsigned long long)lst_xhalves, (unsigned long long)lst_xslow);
    fprintf(stderr, "LODSTAT groups with t1 == t2 in every lane: %llu\n", (unsigned long long)lst_same12);
    for (int f = 0; f < 8; f++)
        for (int z = 0; z < 4; z++)
            if (lst_fmt[f][z]) fprintf(stderr, "LODSTAT lod batches tile0 format %d size %d: %llu\n", f, z, (unsigned long long)lst_fmt[f][z]);
}
static __attribute__((constructor)) void lst_init(void) { atexit(lst_dump); }
static unsigned lst_distinct(uint8x8_t v)
{
    uint8_t a[8] ALIGN16;
    vst1_u8(a, v);
    unsigned m = 0;
    for (int i = 0; i < 8; i++) m |= 1u << (a[i] & 7);
    return m;
}
#endif

#define S32(x) vdupq_n_s32(x)

FOIR_INLINE int32x4_t sign16(int32x4_t x) { return vshrq_n_s32(vshlq_n_s32(x, 16), 16); }

/* the out-of-range test (bits 29 .. 29 - shift of the product not all equal) is "fewer than shift leading sign bits once shifted up by 2"; the
 * quotient is the product shifted by shift - 13 (left by 1 for shift 14); the scalar code tests bit 29 of that quotient (of the product for
 * shift 14), which is the product's sign for shift <= 12 and its bit 29 for 13 and 14 */
struct tcdk { int32x4_t shift, cnt, e, rcp; uint32x4_t wcarry; };

FOIR_INLINE struct tcdk tcd_keys(int32x4_t sw)
{
    struct tcdk k;
    k.wcarry = vcleq_s32(sw, S32(0));
    int32_t idx[4] ALIGN16, tab[4] ALIGN16;
    vst1q_s32(idx, vandq_s32(sw, S32(0x7fff)));
    tab[0] = k_tcdiv[idx[0]]; tab[1] = k_tcdiv[idx[1]]; tab[2] = k_tcdiv[idx[2]]; tab[3] = k_tcdiv[idx[3]];
    int32x4_t tv = vld1q_s32(tab);
    k.shift = vandq_s32(tv, S32(0xf));
    k.rcp = vshrq_n_s32(tv, 4);
    k.cnt = vsubq_s32(k.shift, S32(13));
    k.e = vandq_s32(vreinterpretq_s32_u32(vcgeq_s32(k.shift, S32(13))), S32(2));
    return k;
}

/* FastPerspective (persp 2): the table divide is ss * rcp >> (13 - shift) with rcp << shift ~ 2^28 / w, so the quotient is ~ ss * 32768 / w,
 * here from a float reciprocal with one Newton step, rounded down. Against the table (kfuzz -D, clamped coordinates): no error above 31 (1/32
 * texel units), 1-3 in about a fifth of the coordinates */
FOIR_INLINE float32x4_t tcd_fast_rcp(int32x4_t sw)
{
    float32x4_t fw = vcvtq_f32_s32(sw);
    float32x4_t rc = vrecpeq_f32(fw);
    rc = vmulq_f32(rc, vrecpsq_f32(fw, rc));
    return vmulq_n_f32(rc, 32768.0f);
}

FOIR_INLINE int32x4_t tcd_fast_q(int32x4_t ss, float32x4_t rc) { return vcvtmq_s32_f32(vmulq_f32(vcvtq_f32_s32(ss), rc)); }

/* raw 17-bit coordinate with the out-of-range flags in bits 17 / 18 (scalar tc_divide); FastPerspective flags a quotient outside 17 bits by its sign */
FOIR_INLINE int32x4_t tcd_raw(int persp, int32x4_t x, const struct tcdk *k, float32x4_t rc)
{
    int32x4_t ou;
    int32x4_t q;
    if (persp == 2) {
        q = tcd_fast_q(x, rc);
        uint32x4_t oob = vcgtq_u32(vreinterpretq_u32_s32(vaddq_s32(q, S32(0x10000))), vdupq_n_u32(0x1ffff));
        ou = vandq_s32(vbslq_s32(vcltzq_s32(q), S32(1 << 17), S32(2 << 17)), vreinterpretq_s32_u32(oob));
    } else {
        int32x4_t p = vmulq_s32(x, k->rcp);
        q = vshlq_s32(p, k->cnt);
        uint32x4_t oob = vcltq_s32(vclsq_s32(vshlq_n_s32(p, 2)), k->shift);
        ou = vandq_s32(vbslq_s32(vcltzq_s32(vshlq_s32(p, k->e)), S32(1 << 17), S32(2 << 17)), vreinterpretq_s32_u32(oob));
    }
    ou = vorrq_s32(ou, vandq_s32(S32(2 << 17), vreinterpretq_s32_u32(k->wcarry)));
    return vorrq_s32(vandq_s32(q, S32(0x1ffff)), ou);
}

FOIR_INLINE void tc_divide4(int persp, int32x4_t s, int32x4_t t, int32x4_t w, int32x4_t *sss, int32x4_t *sst)
{
    /* raw 16.16 interpolants; the divide uses the integer parts (the arithmetic shift gives the sign-extended 16-bit values scalar tc_divide() starts from) */
    int32x4_t ss = vshrq_n_s32(s, 16), st = vshrq_n_s32(t, 16), sw = vshrq_n_s32(w, 16);
    if (!persp) {
        *sss = vandq_s32(ss, S32(0x1ffff));
        *sst = vandq_s32(st, S32(0x1ffff));
        return;
    }
    struct tcdk k = { 0 };
    float32x4_t rc = vdupq_n_f32(0);
    if (persp == 2) { k.wcarry = vcleq_s32(sw, S32(0)); rc = tcd_fast_rcp(sw); }
    else k = tcd_keys(sw);
    *sss = tcd_raw(persp, ss, &k, rc);
    *sst = tcd_raw(persp, st, &k, rc);
}

FOIR_INLINE int32x4_t clamp17v(int32x4_t x)
{
    int32x4_t anded = vandq_s32(x, S32(0x18000));
    int32x4_t v = vandq_s32(x, S32(0xffff));
    v = vbslq_s32(vceqq_s32(anded, S32(0x8000)), S32(0x7fff), v);
    v = vbslq_s32(vceqq_s32(anded, S32(0x10000)), S32(0x8000), v);
    v = vbslq_s32(vtstq_s32(x, S32(0x20000)), S32(0x8000), v);
    v = vbslq_s32(vtstq_s32(x, S32(0x40000)), S32(0x7fff), v);
    return v;
}

/* clamp17v(tc_divide4()) for the sampling path, which uses only the low 16 bits: the in-range quotient is sign-extended from 17 bits and
 * saturated, out of range gives -0x8000 / 0x7fff by the tested bit, w <= 0 gives 0x7fff. FastPerspective saturates its quotient (the 17-bit
 * wrap of the table divide made its large quotients worse) */
FOIR_INLINE int32x4_t tcd_clamped(int persp, int32x4_t x, const struct tcdk *k, float32x4_t rc)
{
    int32x4_t q;
    if (persp == 2) {
        q = vmaxq_s32(vminq_s32(tcd_fast_q(x, rc), S32(0x7fff)), S32(-0x8000));
    } else {
        int32x4_t p = vmulq_s32(x, k->rcp);
        q = vshlq_s32(p, k->cnt);
        q = vshrq_n_s32(vshlq_n_s32(q, 15), 15);
        q = vmaxq_s32(vminq_s32(q, S32(0x7fff)), S32(-0x8000));
        uint32x4_t oob = vcltq_s32(vclsq_s32(vshlq_n_s32(p, 2)), k->shift);
        q = vbslq_s32(oob, vbslq_s32(vcltzq_s32(vshlq_s32(p, k->e)), S32(-0x8000), S32(0x7fff)), q);
    }
    return vbslq_s32(k->wcarry, S32(0x7fff), q);
}

FOIR_INLINE void tc_divclamp4(int persp, int32x4_t s, int32x4_t t, int32x4_t w, int32x4_t *sss, int32x4_t *sst)
{
    int32x4_t ss = vshrq_n_s32(s, 16), st = vshrq_n_s32(t, 16), sw = vshrq_n_s32(w, 16);
    if (!persp) { *sss = ss; *sst = st; return; }
    struct tcdk k = { 0 };
    float32x4_t rc = vdupq_n_f32(0);
    if (persp == 2) { k.wcarry = vcleq_s32(sw, S32(0)); rc = tcd_fast_rcp(sw); }
    else k = tcd_keys(sw);
    *sss = tcd_clamped(persp, ss, &k, rc);
    *sst = tcd_clamped(persp, st, &k, rc);
}

/* test hook for kfuzz -D: persp 0..2, + 4 for the raw tc_divide4() */
void neon_tcdiv_test(int mode, const int32_t *s, const int32_t *t, const int32_t *w, int32_t *os, int32_t *ot)
{
    int32x4_t a, b;
    if (mode & 4) tc_divide4(mode & 3, vld1q_s32(s), vld1q_s32(t), vld1q_s32(w), &a, &b);
    else tc_divclamp4(mode & 3, vld1q_s32(s), vld1q_s32(t), vld1q_s32(w), &a, &b);
    vst1q_s32(os, a); vst1q_s32(ot, b);
}

struct axis {
    int shift, lo, hi, clampen, clampdiff, mask, mirror, maskclamped, maskbits, is_t;
};

static void axis_init(struct axis *a, const struct tile *tl, int is_t)
{
    if (!is_t) {
        a->shift = tl->shift_s; a->lo = tl->sl; a->hi = tl->sh; a->clampen = tl->clampens;
        a->clampdiff = tl->clampdiffs; a->mask = tl->mask_s; a->mirror = tl->ms; a->maskclamped = tl->masksclamped;
    } else {
        a->shift = tl->shift_t; a->lo = tl->tl; a->hi = tl->th; a->clampen = tl->clampent;
        a->clampdiff = tl->clampdifft; a->mask = tl->mask_t; a->mirror = tl->mt; a->maskclamped = tl->masktclamped;
    }
    a->maskbits = a->mask ? k_maskbits[a->mask] : 0;
    a->is_t = is_t;
}

FOIR_INLINE void axis_bilinear(const struct axis *a, int32x4_t sss, int32x4_t *S, int32x4_t *FR, int32x4_t *DF)
{
    int32x4_t c = a->shift < 11 ? vshlq_s32(sign16(sss), S32(-a->shift)) : sign16(vshlq_s32(sss, S32(16 - a->shift)));
    uint32x4_t maxv = vcgeq_s32(vshrq_n_s32(c, 3), S32(a->hi));
    c = vsubq_s32(c, S32(a->lo << 3));
    int32x4_t fr = vandq_s32(c, S32(0x1f));
    int32x4_t s = vshrq_n_s32(c, 5);
    if (a->clampen) {
        uint32x4_t neg = vtstq_s32(c, S32(0x10000));
        s = vbicq_s32(s, vreinterpretq_s32_u32(neg));
        fr = vbicq_s32(fr, vreinterpretq_s32_u32(neg));
        s = vbslq_s32(maxv, S32(a->clampdiff), s);
        fr = vbicq_s32(fr, vreinterpretq_s32_u32(maxv));
    }
    int32x4_t df;
    if (a->mask) {
        int32x4_t mb = S32(a->maskbits);
        if (a->mirror) {
            int32x4_t wrap = vandq_s32(vshlq_s32(s, S32(-a->maskclamped)), S32(1));
            s = veorq_s32(s, vnegq_s32(wrap));
            s = vandq_s32(s, mb);
            uint32x4_t full = vceqq_s32(vandq_s32(vsubq_s32(s, wrap), mb), mb);
            df = vbicq_s32(vsubq_s32(S32(1), vshlq_n_s32(wrap, 1)), vreinterpretq_s32_u32(full));
        } else {
            s = vandq_s32(s, mb);
            uint32x4_t full = vceqq_s32(s, mb);
            df = vbslq_s32(full, vnegq_s32(a->is_t ? vandq_s32(s, S32(0xff)) : s), S32(1));
        }
    } else {
        df = S32(1);
    }
    *S = s; *FR = fr; *DF = df;
}

FOIR_INLINE int32x4_t axis_point(const struct axis *a, int32x4_t sss)
{
    int32x4_t c = a->shift < 11 ? vshlq_s32(sign16(sss), S32(-a->shift)) : sign16(vshlq_s32(sss, S32(16 - a->shift)));
    uint32x4_t maxv = vcgeq_s32(vshrq_n_s32(c, 3), S32(a->hi));
    c = vsubq_s32(c, S32(a->lo << 3));
    int32x4_t s = vshrq_n_s32(c, 5);
    if (a->clampen) {
        uint32x4_t neg = vtstq_s32(c, S32(0x10000));
        s = vbicq_s32(s, vreinterpretq_s32_u32(neg));
        s = vbslq_s32(maxv, S32(a->clampdiff), s);
    }
    if (a->mask) {
        if (a->mirror) {
            int32x4_t wrap = vandq_s32(vshlq_s32(s, S32(-a->maskclamped)), S32(1));
            s = veorq_s32(s, vnegq_s32(wrap));
        }
        s = vandq_s32(s, S32(a->maskbits));
    }
    return s;
}

FOIR_INLINE uint16x8_t gather16(const uint8_t *tm, const uint16_t *ad)
{
    uint16x8_t v;
#define G16(k) vld1q_lane_u16((const uint16_t *)(tm + ((ad[k] & 0x7ff) << 1)), v, k)
    v = vld1q_dup_u16((const uint16_t *)(tm + ((ad[0] & 0x7ff) << 1)));
    v = G16(1); v = G16(2); v = G16(3); v = G16(4); v = G16(5); v = G16(6); v = G16(7);
#undef G16
    return vreinterpretq_u16_u8(vrev16q_u8(vreinterpretq_u8_u16(v)));
}

FOIR_INLINE uint8x8_t gather8(const uint8_t *tm, const uint16_t *ad)
{
    uint8x8_t v;
#define G8(k) vld1_lane_u8(tm + (ad[k] & 0xfff), v, k)
    v = vld1_dup_u8(tm + (ad[0] & 0xfff));
    v = G8(1); v = G8(2); v = G8(3); v = G8(4); v = G8(5); v = G8(6); v = G8(7);
#undef G8
    return v;
}

FOIR_INLINE int16x8_t expand5(uint16x8_t x)
{
    int16x8_t v = vreinterpretq_s16_u16(x);
    return vorrq_s16(vshlq_n_s16(v, 3), vshrq_n_s16(v, 2));
}

FOIR_INLINE void decode5551(uint16x8_t v, int16x8_t out[4])
{
    out[0] = expand5(vshrq_n_u16(v, 11));
    out[1] = expand5(vandq_u16(vshrq_n_u16(v, 6), vdupq_n_u16(0x1f)));
    out[2] = expand5(vandq_u16(vshrq_n_u16(v, 1), vdupq_n_u16(0x1f)));
    out[3] = vreinterpretq_s16_u16(vandq_u16(vtstq_u16(v, vdupq_n_u16(1)), vdupq_n_u16(0xff)));
}

FOIR_INLINE void decode_ia16(uint16x8_t v, int16x8_t out[4])
{
    int16x8_t hi = vreinterpretq_s16_u16(vshrq_n_u16(v, 8));
    out[0] = out[1] = out[2] = hi;
    out[3] = vreinterpretq_s16_u16(vandq_u16(v, vdupq_n_u16(0xff)));
}

FOIR_INLINE void decode_16_swapped(uint16x8_t v, int16x8_t out[4])   /* r=b=v>>8, g=a=v&0xff */
{
    int16x8_t hi = vreinterpretq_s16_u16(vshrq_n_u16(v, 8));
    int16x8_t lo = vreinterpretq_s16_u16(vandq_u16(v, vdupq_n_u16(0xff)));
    out[0] = out[2] = hi;
    out[1] = out[3] = lo;
}

FOIR_INLINE void all4(int16x8_t v, int16x8_t out[4]) { out[0] = out[1] = out[2] = out[3] = v; }

#define TSP_MID_RT 2       /* tsp.mid: read MID_TEXEL from the render state */

struct tsp {
    int size, fmt;
    int tlut, tlt;
    int smp, mid;
    int bilerp;           /* 0 selects the colour-conversion arithmetic */
};

/* swp = odd-row word swap */
struct rowt { int32x4_t base, swp; };

FOIR_INLINE struct rowt row_terms(const struct tile *tl, const struct tsp *sp, int32x4_t t)
{
    struct rowt R;
    int32x4_t tbase = vmlaq_n_s32(S32(tl->tmem), vandq_s32(t, S32(0xff)), tl->line);
    int32x4_t odd = vandq_s32(t, S32(1));
    if (sp->size == PIXEL_SIZE_4BIT) { R.base = vshlq_n_s32(tbase, 4); R.swp = vshlq_n_s32(odd, 2); }
    else if (sp->size == PIXEL_SIZE_8BIT) { R.base = vshlq_n_s32(tbase, 3); R.swp = vshlq_n_s32(odd, 2); }
    else { R.base = vshlq_n_s32(tbase, 2); R.swp = vshlq_n_s32(odd, 1); }
    return R;
}

/* halfword index for 16/32-bit texels, byte address for 4/8-bit */
FOIR_INLINE int32x4_t tap_addr4(const struct tsp *sp, struct rowt R, int32x4_t s, uint16_t *nib)
{
    int32x4_t a = vaddq_s32(R.base, s);
    if (sp->size == PIXEL_SIZE_4BIT) {
        vst1_u16(nib, vmovn_u32(vreinterpretq_u32_s32(vandq_s32(a, S32(1)))));
        a = vshrq_n_s32(a, 1);
    }
    a = veorq_s32(a, R.swp);
    if (sp->size == PIXEL_SIZE_32BIT && sp->fmt == FORMAT_RGBA && !sp->tlut) a = vandq_s32(a, S32(0x3ff));
    return a;
}

static void fetch_decode(const struct rdp *r, const struct tile *tl, const struct tsp *sp, const uint16_t *ad, const uint16_t *nib,
                         int16x8_t out[4])
{
    const uint8_t *tm = r->tmem;
    const int size = sp->size, fmt = sp->fmt;
    const bool tlut = sp->tlut;

    if (size == PIXEL_SIZE_4BIT || size == PIXEL_SIZE_8BIT) {
        uint8x8_t b = gather8(tm, ad);
        int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(b));
        int16x8_t p, nibv = v;
        if (size == PIXEL_SIZE_4BIT) {
            uint16x8_t odd = vtstq_u16(vld1q_u16(nib), vdupq_n_u16(1));
            nibv = vbslq_s16(odd, vandq_s16(v, vdupq_n_s16(0xf)), vshrq_n_s16(v, 4));
            if (tlut || fmt == FORMAT_CI) p = vorrq_s16(nibv, vdupq_n_s16(tl->palette << 4));
            else p = nibv;
        } else {
            p = v;
        }
        if (tlut) {
            uint16_t pa[8] ALIGN16;
            vst1q_u16(pa, vreinterpretq_u16_s16(vaddq_s16(vdupq_n_s16(0x400), vshlq_n_s16(vandq_s16(p, vdupq_n_s16(0xff)), 2))));
            uint16x8_t pv = gather16(tm, pa);
            if (!sp->tlt) decode5551(pv, out); else decode_ia16(pv, out);
            return;
        }
        if (size == PIXEL_SIZE_8BIT) {
            if (fmt == FORMAT_IA) {
                int16x8_t iv = vandq_s16(v, vdupq_n_s16(0xf0));
                iv = vorrq_s16(iv, vshrq_n_s16(iv, 4));
                int16x8_t av = vandq_s16(v, vdupq_n_s16(0xf));
                av = vorrq_s16(vshlq_n_s16(av, 4), av);
                out[0] = out[1] = out[2] = iv; out[3] = av;
            } else {
                all4(v, out);
            }
        } else {
            if (fmt == FORMAT_IA) {
                int16x8_t iv = vandq_s16(nibv, vdupq_n_s16(0xe));
                iv = vorrq_s16(vorrq_s16(vshlq_n_s16(iv, 4), vshlq_n_s16(iv, 1)), vshrq_n_s16(iv, 2));
                out[0] = out[1] = out[2] = iv;
                out[3] = vandq_s16(vreinterpretq_s16_u16(vtstq_s16(nibv, vdupq_n_s16(1))), vdupq_n_s16(0xff));
            } else if (fmt == FORMAT_CI) {
                all4(p, out);
            } else {                         /* RGBA4 / I4: replicate the nibble */
                all4(vorrq_s16(nibv, vshlq_n_s16(nibv, 4)), out);
            }
        }
        return;
    }

    uint16x8_t v = gather16(tm, ad);
    if (tlut) {
        uint16_t pa[8] ALIGN16;
        vst1q_u16(pa, vaddq_u16(vdupq_n_u16(0x400), vshlq_n_u16(vshrq_n_u16(v, 8), 2)));
        uint16x8_t pv = gather16(tm, pa);
        if (!sp->tlt) decode5551(pv, out); else decode_ia16(pv, out);
        return;
    }
    switch (fmt) {
    case FORMAT_RGBA:
        if (size == PIXEL_SIZE_16BIT) {
            decode5551(v, out);
        } else {
            uint16_t ad2[8] ALIGN16;
            vst1q_u16(ad2, vorrq_u16(vld1q_u16(ad), vdupq_n_u16(0x400)));
            uint16x8_t hi = gather16(tm, ad2);
            out[0] = vreinterpretq_s16_u16(vshrq_n_u16(v, 8));
            out[1] = vreinterpretq_s16_u16(vandq_u16(v, vdupq_n_u16(0xff)));
            out[2] = vreinterpretq_s16_u16(vshrq_n_u16(hi, 8));
            out[3] = vreinterpretq_s16_u16(vandq_u16(hi, vdupq_n_u16(0xff)));
        }
        break;
    case FORMAT_IA:
        if (size == PIXEL_SIZE_16BIT) {
            decode_ia16(v, out);
        } else {                                      /* IA32 */
            decode_16_swapped(v, out);
        }
        break;
    default:                                          /* CI, I and invalid formats */
        decode_16_swapped(v, out);
        break;
    }
}

FOIR_INLINE int16x8_t narrow16(int32x4_t a, int32x4_t b) { return vcombine_s16(vmovn_s32(a), vmovn_s32(b)); }

struct tctx {
    const struct tile *tl;
    struct axis as, at;
    struct tsp sp;
    int mid;
};

FOIR_INLINE void tctx_init(struct tctx *c, const struct rdp *r, const struct tile *tl, const struct tsp *sp)
{
    c->tl = tl;
    c->sp = *sp;
    c->mid = sp->mid == TSP_MID_RT ? r->om.mid_texel : sp->mid;
    axis_init(&c->as, tl, 0);
    axis_init(&c->at, tl, 1);
}

FOIR_INLINE void fetch_taps(const struct rdp *r, const struct tile *tl, const struct tsp *sp, const struct rowt R[2], const int32x4_t S[2],
                            int16x8_t out[4])
{
    uint16_t ad[8] ALIGN16, nib[8] ALIGN16 = { 0 };
    for (int h = 0; h < 2; h++)
        vst1_u16(ad + 4 * h, vmovn_u32(vreinterpretq_u32_s32(tap_addr4(sp, R[h], S[h], nib + 4 * h))));
    fetch_decode(r, tl, sp, ad, nib, out);
}

/* MID_TEXEL lanes are rare: such samples go to tex_cycle_mid(), an out-of-line copy with the four-tap blend compiled in, so the common path is identical to a variant built without MID_TEXEL */
static void tex_cycle_mid(const struct rdp *r, const struct tctx *cx, const struct tsp *sp, const int32x4_t sss[2], const int32x4_t sst[2],
                          int16x8_t out[4]);

FOIR_INLINE void tex_cycle_impl(const struct rdp *r, const struct tctx *cx, const struct tsp *sp, const int32x4_t sss[2], const int32x4_t sst[2],
                                int16x8_t out[4], const int midmode)
{
    const struct tile *tl = cx->tl;

    if (!(sp->smp || sp->tlut)) {
        int32x4_t S[2];
        struct rowt R[2];
        for (int h = 0; h < 2; h++) { S[h] = axis_point(&cx->as, sss[h]); R[h] = row_terms(tl, sp, axis_point(&cx->at, sst[h])); }
        fetch_taps(r, tl, sp, R, S, out);
        if (sp->bilerp) {
            for (int c = 0; c < 4; c++) out[c] = vandq_s16(out[c], vdupq_n_s16(0x1ff));
            return;
        }
        /* BI_LERP = 0 on a point sample: the texel goes through the colour-conversion matrix
         *   r = b + (k0*g + 0x80 >> 8)   g = b + (k1*r + k2*g + 0x80 >> 8)   b = b + (k3*r + 0x80 >> 8)   a = b */
        int16x8_t tr = out[0], tg = out[1], tb = out[2];
        const int16_t k0 = (int16_t)r->k0_tf, k1 = (int16_t)r->k1_tf, k2 = (int16_t)r->k2_tf, k3 = (int16_t)r->k3_tf;
        const int32x4_t rnd = vdupq_n_s32(0x80);
        int16x8_t cr_ = vcombine_s16(vshrn_n_s32(vmlal_n_s16(rnd, vget_low_s16(tg), k0), 8),
                                     vshrn_n_s32(vmlal_n_s16(rnd, vget_high_s16(tg), k0), 8));
        int32x4_t g_lo = vmlal_n_s16(vmlal_n_s16(rnd, vget_low_s16(tr), k1), vget_low_s16(tg), k2);
        int32x4_t g_hi = vmlal_n_s16(vmlal_n_s16(rnd, vget_high_s16(tr), k1), vget_high_s16(tg), k2);
        int16x8_t cg_ = vcombine_s16(vshrn_n_s32(g_lo, 8), vshrn_n_s32(g_hi, 8));
        int16x8_t cb_ = vcombine_s16(vshrn_n_s32(vmlal_n_s16(rnd, vget_low_s16(tr), k3), 8),
                                     vshrn_n_s32(vmlal_n_s16(rnd, vget_high_s16(tr), k3), 8));
        out[0] = vandq_s16(vaddq_s16(tb, cr_), vdupq_n_s16(0x1ff));
        out[1] = vandq_s16(vaddq_s16(tb, cg_), vdupq_n_s16(0x1ff));
        out[2] = vandq_s16(vaddq_s16(tb, cb_), vdupq_n_s16(0x1ff));
        out[3] = vandq_s16(tb, vdupq_n_s16(0x1ff));
        return;
    }

    int32x4_t S[2], T[2], SF[2], TF[2], SD[2], TD[2];
    for (int h = 0; h < 2; h++) {
        axis_bilinear(&cx->as, sss[h], &S[h], &SF[h], &SD[h]);
        axis_bilinear(&cx->at, sst[h], &T[h], &TF[h], &TD[h]);
    }
    struct rowt R0[2], R1[2];
    for (int h = 0; h < 2; h++) R0[h] = row_terms(tl, sp, T[h]);
    if (!sp->smp) {
        fetch_taps(r, tl, sp, R0, S, out);
        for (int c = 0; c < 4; c++) out[c] = vandq_s16(out[c], vdupq_n_s16(0x1ff));
        return;
    }
    int32x4_t S1[2];
    for (int h = 0; h < 2; h++) {
        S1[h] = vaddq_s32(S[h], SD[h]);
        R1[h] = row_terms(tl, sp, vaddq_s32(vandq_s32(T[h], S32(0xff)), TD[h]));
    }

    int16x8_t sf = narrow16(SF[0], SF[1]), tf = narrow16(TF[0], TF[1]);
    uint16x8_t upper = vtstq_s16(vaddq_s16(sf, tf), vdupq_n_s16(0x20));
    uint16x8_t center = vdupq_n_u16(0);
    int any_center = 0;
    if (midmode) {
        center = vandq_u16(vceqq_s16(sf, vdupq_n_s16(0x10)), vceqq_s16(tf, vdupq_n_s16(0x10)));
        any_center = vmaxvq_u16(center) != 0;
    } else if (cx->mid) {
        center = vandq_u16(vceqq_s16(sf, vdupq_n_s16(0x10)), vceqq_s16(tf, vdupq_n_s16(0x10)));
        if (vmaxvq_u16(center) != 0) {
            struct tctx cxc = *cx;
            struct tsp spc = *sp;
            int32x4_t s2[2] = { sss[0], sss[1] }, t2[2] = { sst[0], sst[1] };
            int16x8_t o2[4];
            tex_cycle_mid(r, &cxc, &spc, s2, t2, o2);
            for (int c = 0; c < 4; c++) out[c] = o2[c];
            return;
        }
    }

    int16x8_t t1[4], t2[4], ta[4];
    fetch_taps(r, tl, sp, R0, S1, t1);
    fetch_taps(r, tl, sp, R1, S, t2);
    if (!any_center) {
        /* lower triangle uses t0, upper triangle t3: fetch whichever each lane needs */
        int32x4_t A[2];
        struct rowt RA[2];
        uint16_t ad[8] ALIGN16, nib[8] ALIGN16 = { 0 };
        int32x4_t um[2] = { vmovl_s16(vget_low_s16(vreinterpretq_s16_u16(upper))), vmovl_s16(vget_high_s16(vreinterpretq_s16_u16(upper))) };
        for (int h = 0; h < 2; h++) {
            RA[h].base = vbslq_s32(vreinterpretq_u32_s32(um[h]), R1[h].base, R0[h].base);
            RA[h].swp = vbslq_s32(vreinterpretq_u32_s32(um[h]), R1[h].swp, R0[h].swp);
            A[h] = vbslq_s32(vreinterpretq_u32_s32(um[h]), S1[h], S[h]);
            vst1_u16(ad + 4 * h, vmovn_u32(vreinterpretq_u32_s32(tap_addr4(sp, RA[h], A[h], nib + 4 * h))));
        }
        fetch_decode(r, tl, sp, ad, nib, ta);
        int16x8_t isf = vsubq_s16(vdupq_n_s16(0x20), sf), itf = vsubq_s16(vdupq_n_s16(0x20), tf);
        int16x8_t w1 = vbslq_s16(upper, itf, sf), w2 = vbslq_s16(upper, isf, tf);
        const int16x8_t rnd = vdupq_n_s16(0x10);
        for (int c = 0; c < 4; c++) {
            int16x8_t acc = vmlaq_s16(vmlaq_s16(rnd, w1, vsubq_s16(t1[c], ta[c])), w2, vsubq_s16(t2[c], ta[c]));
            out[c] = vandq_s16(vaddq_s16(ta[c], vshrq_n_s16(acc, 5)), vdupq_n_s16(0x1ff));
        }
        return;
    }

    if (!midmode) return;
    int16x8_t t0[4], t3[4];
    fetch_taps(r, tl, sp, R0, S, t0);
    fetch_taps(r, tl, sp, R1, S1, t3);
    int16x8_t isf = vsubq_s16(vdupq_n_s16(0x20), sf), itf = vsubq_s16(vdupq_n_s16(0x20), tf);
    const int16x8_t rnd = vdupq_n_s16(0x10);
    for (int c = 0; c < 4; c++) {
        int16x8_t a0 = t0[c], a1 = t1[c], a2 = t2[c], a3 = t3[c];
        int16x8_t lo = vaddq_s16(a0, vshrq_n_s16(vaddq_s16(vaddq_s16(vmulq_s16(sf, vsubq_s16(a1, a0)),
                                                                     vmulq_s16(tf, vsubq_s16(a2, a0))), rnd), 5));
        int16x8_t up = vaddq_s16(a3, vshrq_n_s16(vaddq_s16(vaddq_s16(vmulq_s16(isf, vsubq_s16(a2, a3)),
                                                                     vmulq_s16(itf, vsubq_s16(a1, a3))), rnd), 5));
        int16x8_t res = vbslq_s16(upper, up, lo);
        /* t3 + (((t1 + t2) << 6) - (t3 << 7) + ((~t3 + t0) << 6) + 0xc0) >> 8, in 32 bits */
        int32x4_t l0 = vmovl_s16(vget_low_s16(a0)), l1 = vmovl_s16(vget_low_s16(a1));
        int32x4_t l2 = vmovl_s16(vget_low_s16(a2)), l3 = vmovl_s16(vget_low_s16(a3));
        int32x4_t h0 = vmovl_high_s16(a0), h1 = vmovl_high_s16(a1), h2 = vmovl_high_s16(a2), h3 = vmovl_high_s16(a3);
        int32x4_t cl = vaddq_s32(l3, vshrq_n_s32(vaddq_s32(vaddq_s32(vshlq_n_s32(vaddq_s32(l1, l2), 6), vnegq_s32(vshlq_n_s32(l3, 7))),
                                 vaddq_s32(vshlq_n_s32(vaddq_s32(vmvnq_s32(l3), l0), 6), S32(0xc0))), 8));
        int32x4_t ch = vaddq_s32(h3, vshrq_n_s32(vaddq_s32(vaddq_s32(vshlq_n_s32(vaddq_s32(h1, h2), 6), vnegq_s32(vshlq_n_s32(h3, 7))),
                                 vaddq_s32(vshlq_n_s32(vaddq_s32(vmvnq_s32(h3), h0), 6), S32(0xc0))), 8));
        res = vbslq_s16(center, narrow16(cl, ch), res);
        out[c] = vandq_s16(res, vdupq_n_s16(0x1ff));
    }
}

static __attribute__((noinline)) void tex_cycle_mid(const struct rdp *r, const struct tctx *cx, const struct tsp *sp, const int32x4_t sss[2], const int32x4_t sst[2],
                                                  int16x8_t out[4])
{
    tex_cycle_impl(r, cx, sp, sss, sst, out, 1);
}

static void tex_cycle(const struct rdp *r, const struct tctx *cx, const struct tsp *sp, const int32x4_t sss[2], const int32x4_t sst[2],
                      int16x8_t out[4])
{
    tex_cycle_impl(r, cx, sp, sss, sst, out, 0);
}

/* LOD from three divided coordinate pairs: own, next pixel (s + dsinc ...), pixel one row down (s + dsdy ...). The next-pixel divide is taken
 * from the neighbouring lane when it holds exactly s + dsinc, t + dtinc, w + dwinc (exact: the divide is a function of its inputs). */
#define LOD_PAD 8
struct lodbuf {
    ALIGN16 int32_t ls_[FOIR_LANES + 2 * LOD_PAD], lt_[FOIR_LANES + 2 * LOD_PAD], lw_[FOIR_LANES + 2 * LOD_PAD];
    ALIGN16 int32_t rs_[FOIR_LANES + 2 * LOD_PAD], rt_[FOIR_LANES + 2 * LOD_PAD];
    ALIGN16 uint8_t t1[FOIR_LANES], t2[FOIR_LANES];
};
#define LB(a) ((a) + LOD_PAD)

FOIR_INLINE void lod_pass_impl(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo,
                               struct lodbuf *lb, const int persp, const int tiles)
{
    const struct prim_ctx *pc = &r->pc;
    const struct other_modes *om = &r->om;
    int32_t *ls = LB(lb->ls_), *lt = LB(lb->lt_), *lw = LB(lb->lw_), *rs = LB(lb->rs_), *rt = LB(lb->rt_);
    const int n = ng * 8;
    const int32x4_t add_s = S32(add[0]), add_t = S32(add[1]), add_w = S32(add[2]);

    for (int i = 0; i < n; i += 4) {
        int32x4_t sv = vaddq_s32(vld1q_s32(s + i), add_s), tv = vaddq_s32(vld1q_s32(t + i), add_t), wv = vaddq_s32(vld1q_s32(w + i), add_w);
        vst1q_s32(ls + i, sv); vst1q_s32(lt + i, tv); vst1q_s32(lw + i, wv);
        int32x4_t a, b;
        tc_divide4(persp, sv, tv, wv, &a, &b);
        vst1q_s32(rs + i, a); vst1q_s32(rt + i, b);
    }
    /* the lane before the first / after the last one must never look like a neighbour */
    ls[-1] = ls[0] + pc->dsinc + 1; ls[n] = ls[n - 1] + pc->dsinc + 1;
    lt[-1] = lw[-1] = lt[n] = lw[n] = 0;
    for (int k = 1; k <= 4; k++) { rs[-k] = 0; rt[-k] = 0; rs[n + k - 1] = 0; rt[n + k - 1] = 0; }
    const int dir = pc->flip ? 1 : -1;

    const int32x4_t dsi = S32(pc->dsinc), dti = S32(pc->dtinc), dwi = S32(pc->dwinc);
    const int32x4_t dsy = S32(pc->dsdy), dty = S32(pc->dtdy), dwy = S32(pc->dwdy);
    const int32_t min_level = r->min_level, max_level = pc->max_level;
    const int sharpen = om->sharpen_tex_en, detail = om->detail_tex_en;
    const int plain = !sharpen && !detail;
    const uint16x8_t k4000 = vdupq_n_u16(0x4000), kff = vdupq_n_u16(0xff);

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        uint32x4_t dm[2], cl[2];
        for (int h = 0; h < 2; h++) {
            const int i = o + 4 * h;
            int32x4_t is = vld1q_s32(rs + i), it = vld1q_s32(rt + i);
            int32x4_t sv = vld1q_s32(ls + i), tv = vld1q_s32(lt + i), wv = vld1q_s32(lw + i);
            int32x4_t ns, nt, nys, nyt;
            if (persp) {
                uint32x4_t ok = vandq_u32(vandq_u32(vceqq_s32(vld1q_s32(ls + i + dir), vaddq_s32(sv, dsi)),
                                                    vceqq_s32(vld1q_s32(lt + i + dir), vaddq_s32(tv, dti))),
                                          vceqq_s32(vld1q_s32(lw + i + dir), vaddq_s32(wv, dwi)));
                ns = vld1q_s32(rs + i + dir); nt = vld1q_s32(rt + i + dir);
#ifdef FOIR_STATS
                __atomic_fetch_add(&lst_xhalves, 1, __ATOMIC_RELAXED);
#endif
                if (vminvq_u32(ok) == 0) {
#ifdef FOIR_STATS
                    __atomic_fetch_add(&lst_xslow, 1, __ATOMIC_RELAXED);
#endif
                    int32x4_t xs, xt;
                    tc_divide4(persp, vaddq_s32(sv, dsi), vaddq_s32(tv, dti), vaddq_s32(wv, dwi), &xs, &xt);
                    ns = vbslq_s32(ok, ns, xs); nt = vbslq_s32(ok, nt, xt);
                }
            } else {
                tc_divide4(0, vaddq_s32(sv, dsi), vaddq_s32(tv, dti), vaddq_s32(wv, dwi), &ns, &nt);
            }
            tc_divide4(persp, vaddq_s32(sv, dsy), vaddq_s32(tv, dty), vaddq_s32(wv, dwy), &nys, &nyt);
            /* lodclamp = any of the six values has bit 17 or 18 set */
            cl[h] = vtstq_s32(vorrq_s32(vorrq_s32(vorrq_s32(is, it), vorrq_s32(ns, nt)), vorrq_s32(nys, nyt)), S32(0x60000));
            /* |next - cur| of the 17-bit sign/magnitude coordinates: a negative difference d becomes ~d (the RDP's flip), i.e. v ^ (v >> 31), which only fills the low 14 bits and keeps the order */
            int32x4_t cs = vshlq_n_s32(is, 15), ct = vshlq_n_s32(it, 15);
            int32x4_t xs = vhsubq_s32(vshlq_n_s32(ns, 15), cs), xt = vhsubq_s32(vshlq_n_s32(nt, 15), ct);
            int32x4_t ys = vhsubq_s32(vshlq_n_s32(nys, 15), cs), yt = vhsubq_s32(vshlq_n_s32(nyt, 15), ct);
            xs = veorq_s32(xs, vshrq_n_s32(xs, 31)); xt = veorq_s32(xt, vshrq_n_s32(xt, 31));
            ys = veorq_s32(ys, vshrq_n_s32(ys, 31)); yt = veorq_s32(yt, vshrq_n_s32(yt, 31));
            dm[h] = vreinterpretq_u32_s32(vmaxq_s32(vmaxq_s32(xs, xt), vmaxq_s32(ys, yt)));
        }
        /* the RDP's folded LOD is the largest difference below 0x4000, else bit 14 set (read as overflow later); a saturated 16-bit value keeps exactly that */
        uint16x8_t lod = vminq_u16(vcombine_u16(vqshrn_n_u32(dm[0], 14), vqshrn_n_u32(dm[1], 14)), k4000);
        uint16x8_t clamp = vcombine_u16(vmovn_u32(cl[0]), vmovn_u32(cl[1]));

        uint16x8_t c1 = vorrq_u16(vceqq_u16(lod, k4000), clamp);
        uint16x8_t c2 = vbicq_u16(vcltq_u16(lod, vdupq_n_u16((uint16_t)min_level)), c1);
        uint16x8_t c3 = vbicq_u16(vbicq_u16(vcltq_u16(lod, vdupq_n_u16(32)), c1), c2);
        uint16x8_t c4 = vmvnq_u16(vorrq_u16(vorrq_u16(c1, c2), c3));
        uint16x8_t ltil = vqsubq_u16(vdupq_n_u16(15), vclzq_u16(vandq_u16(vshrq_n_u16(lod, 5), kff)));     /* floor(log2(.)), 0 for 0 */
        ltil = vandq_u16(ltil, c4);
        uint16x8_t mag = vorrq_u16(c2, c3);
        uint16x8_t dis;
        if (max_level) {
            dis = vorrq_u16(vtstq_u16(lod, vdupq_n_u16(0x6000)), vcgeq_u16(ltil, vdupq_n_u16((uint16_t)max_level)));
            dis = vandq_u16(dis, c4);
        } else {
            dis = vdupq_n_u16(0xffff);
        }
        dis = vorrq_u16(dis, c1);
        uint16x8_t lod3 = vshlq_n_u16(lod, 3);
        uint16x8_t lf_mid, lf_min, lf_far = vandq_u16(vshlq_u16(lod3, vnegq_s16(vreinterpretq_s16_u16(ltil))), kff);
        if (plain) {
            uint16x8_t d = vandq_u16(dis, kff);
            lf_mid = d; lf_min = d; lf_far = vbslq_u16(dis, kff, lf_far);
        } else {
            lf_mid = vorrq_u16(lod3, vdupq_n_u16(sharpen ? 0x100 : 0));
            lf_min = vdupq_n_u16((uint16_t)((min_level << 3) | (sharpen ? 0x100 : 0)));
        }
        uint16x8_t lf = vbslq_u16(c1, kff, vbslq_u16(c2, lf_min, vbslq_u16(c3, lf_mid, lf_far)));
        vst1q_s16(lfo + o, vreinterpretq_s16_u16(lf));

        if (tiles) {
            uint16x8_t ltile = vbslq_u16(dis, vdupq_n_u16((uint16_t)max_level), ltil);
            uint16x8_t base = vaddq_u16(vdupq_n_u16((uint16_t)pc->tilenum), ltile);
            uint16x8_t k7 = vdupq_n_u16(7), one = vdupq_n_u16(1);
            uint16x8_t t1v, t2v;
            if (!detail) {
                uint16x8_t same = vorrq_u16(dis, sharpen ? vdupq_n_u16(0) : mag);
                t1v = vandq_u16(base, k7);
                t2v = vbslq_u16(same, t1v, vandq_u16(vaddq_u16(t1v, one), k7));
            } else {
                t1v = vandq_u16(vaddq_u16(base, vbicq_u16(one, mag)), k7);
                uint16x8_t far2 = vbicq_u16(vmvnq_u16(dis), mag);
                t2v = vandq_u16(vaddq_u16(base, vbslq_u16(far2, vdupq_n_u16(2), one)), k7);
            }
            vst1_u8(lb->t1 + o, vmovn_u16(t1v));
            vst1_u8(lb->t2 + o, vmovn_u16(t2v));
        }
    }
}

static __attribute__((noinline)) void lod_pass_p1t(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo, struct lodbuf *lb)
{ lod_pass_impl(r, s, t, w, add, ng, lfo, lb, 1, 1); }
static __attribute__((noinline)) void lod_pass_p1(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo, struct lodbuf *lb)
{ lod_pass_impl(r, s, t, w, add, ng, lfo, lb, 1, 0); }
static __attribute__((noinline)) void lod_pass_p2t(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo, struct lodbuf *lb)
{ lod_pass_impl(r, s, t, w, add, ng, lfo, lb, 2, 1); }
static __attribute__((noinline)) void lod_pass_p2(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo, struct lodbuf *lb)
{ lod_pass_impl(r, s, t, w, add, ng, lfo, lb, 2, 0); }
static __attribute__((noinline)) void lod_pass_p0t(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo, struct lodbuf *lb)
{ lod_pass_impl(r, s, t, w, add, ng, lfo, lb, 0, 1); }
static __attribute__((noinline)) void lod_pass_p0(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo, struct lodbuf *lb)
{ lod_pass_impl(r, s, t, w, add, ng, lfo, lb, 0, 0); }

FOIR_INLINE void lod_pass(const struct rdp *r, const int32_t *s, const int32_t *t, const int32_t *w, const int32_t add[3], int ng, int16_t *lfo,
                          struct lodbuf *lb, int persp, int tiles)
{
    if (persp == 2) { if (tiles) lod_pass_p2t(r, s, t, w, add, ng, lfo, lb); else lod_pass_p2(r, s, t, w, add, ng, lfo, lb); }
    else if (persp) { if (tiles) lod_pass_p1t(r, s, t, w, add, ng, lfo, lb); else lod_pass_p1(r, s, t, w, add, ng, lfo, lb); }
    else       { if (tiles) lod_pass_p0t(r, s, t, w, add, ng, lfo, lb); else lod_pass_p0(r, s, t, w, add, ng, lfo, lb); }
}

static __attribute__((noinline)) void tex_cycle_multi(const struct rdp *r, struct tctx *cxs, uint32_t *have, const struct tsp *base, uint8x8_t ids,
                                                     const int32x4_t sss[2], const int32x4_t sst[2], int16x8_t out[4])
{
    uint8_t idv[8] ALIGN16;
    vst1_u8(idv, ids);
    unsigned done = 0;
    for (int c = 0; c < 4; c++) out[c] = vdupq_n_s16(0);
    while (done != 0xff) {     /* (not a counted loop: the compiler would unroll it and inline the cycle code eight times) */
        int k = __builtin_ctz(~done & 0xff);
        int v = idv[k] & 7;
        uint8x8_t eq = vceq_u8(ids, vdup_n_u8((uint8_t)v));
        uint16x8_t m16 = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(eq)));
        for (int l = 0; l < 8; l++) if ((idv[l] & 7) == v) done |= 1u << l;
        if (!(*have & (1u << v))) {
            const struct tile *tv = &r->tile[v];
            const struct tsp sp = { tv->size, tv->format, base->tlut, r->om.tlut_type, base->smp, base->mid, base->bilerp };
            tctx_init(&cxs[v], r, tv, &sp);
            *have |= 1u << v;
        }
        int16x8_t t[4];
        tex_cycle(r, &cxs[v], &cxs[v].sp, sss, sst, t);
        for (int c = 0; c < 4; c++) out[c] = vbslq_s16(m16, t[c], out[c]);
    }
}

/* fixed: the format description a specialised variant was built for (all tiles of the chain); NULL: take it from the tile */
FOIR_INLINE void tex_cycle_tiles(const struct rdp *r, struct tctx *cxs, uint32_t *have, const struct tsp *base, const struct tsp *fixed, uint8x8_t ids,
                                 const int32x4_t sss[2], const int32x4_t sst[2], int16x8_t out[4])
{
    const uint64_t same = vget_lane_u64(vreinterpret_u64_u8(vceq_u8(ids, vdup_lane_u8(ids, 0))), 0);
    if (same != ~0ull) { tex_cycle_multi(r, cxs, have, base, ids, sss, sst, out); return; }
    const int v = vget_lane_u8(ids, 0) & 7;
    if (!(*have & (1u << v))) {
        const struct tile *tv = &r->tile[v];
        const struct tsp sp = fixed ? *fixed : (struct tsp){ tv->size, tv->format, base->tlut, r->om.tlut_type, base->smp, base->mid, base->bilerp };
        tctx_init(&cxs[v], r, tv, &sp);
        *have |= 1u << v;
    }
    tex_cycle(r, &cxs[v], fixed ? fixed : &cxs[v].sp, sss, sst, out);
}

struct tspec {
    int two;
    int t0, t1, nx;
    int dolod, lodtiles;
    int lodfix;              /* all tiles the LOD can select have the format c0 / c1 describe */
    int persp;
    struct tsp c0, c1;
    int anyfmt;              /* size, format and TLUT type come from the render state */
};

FOIR_INLINE void texture_body(struct rdp *r, struct px_batch *b, const struct tspec *S)
{
    const struct other_modes *om = &r->om;
    const int prim_tile = r->pc.tilenum;
    const struct tile *tl0 = &r->tile[prim_tile];
    const struct tile *tl1 = &r->tile[(prim_tile + 1) & 7];
    const int two = S->two;
    NEON_HANDLED(0);

    const struct prim_ctx *pc = &r->pc;
    struct tctx cx0, cx1 = { 0 };
    struct tctx cxs0[8], cxs1[8];
    uint32_t have0 = 0, have1 = 0, haven = 0;
    struct tctx cxsn[8];
    tctx_init(&cx0, r, tl0, &S->c0);
    if (two) tctx_init(&cx1, r, tl1, &S->c1);
    if (S->anyfmt) {
        cx0.sp.size = tl0->size; cx0.sp.fmt = tl0->format; cx0.sp.tlt = om->tlut_type;
        cx1.sp.size = tl1->size; cx1.sp.fmt = tl1->format; cx1.sp.tlt = om->tlut_type;
    }
    const int ng = NGROUPS(b->n);
    const int32x4_t dsi = S32(pc->dsinc), dti = S32(pc->dtinc), dwi = S32(pc->dwinc);
    const int lodtiles = S->lodtiles;
    const int persp = S->persp && r->cfg.fast_persp ? 2 : S->persp;
    (void)om;
#ifdef FOIR_STATS
    {
        int mi = S->two | (S->persp << 1) | (S->c0.smp << 2) | (S->c0.tlut << 3) | (lodtiles << 4) | (S->dolod << 5) | (om->detail_tex_en << 6) |
                 (om->sharpen_tex_en << 7) | (S->anyfmt << 8) | (S->nx << 9) | (S->t1 << 10);
        LST_ADD(lst_batches[mi], 1); LST_ADD(lst_pixels[mi], b->n); LST_ADD(lst_groups[mi], ng);
        if (lodtiles) LST_ADD(lst_fmt[tl0->format & 7][tl0->size & 3], 1);
    }
#endif

    struct lodbuf lb, lbn;
    if (S->dolod) {
        static const int32_t zero3[3] = { 0, 0, 0 };
        lod_pass(r, b->s, b->t, b->w, zero3, ng, b->lodfrac, &lb, persp, lodtiles);
    }
    if (S->nx && lodtiles) {
        /* the next pixel is evaluated as a pixel of its own */
        const int32_t add[3] = { pc->dsinc, pc->dtinc, pc->dwinc };
        int16_t lfn[FOIR_LANES] ALIGN16;
        lod_pass(r, b->s, b->t, b->w, add, ng, lfn, &lbn, persp, 1);
    }
    const int32_t *lrs = LB(lb.rs_), *lrt = LB(lb.rt_), *nrs = LB(lbn.rs_), *nrt = LB(lbn.rt_);
#ifdef FOIR_STATS
    if (S->dolod && lodtiles)
        for (int g = 0; g < ng; g++) {
            unsigned m1 = lst_distinct(vld1_u8(lb.t1 + 8 * g)), m2 = lst_distinct(vld1_u8(lb.t2 + 8 * g));
            LST_ADD(lst_dist[0][__builtin_popcount(m1)], 1); LST_ADD(lst_dist[1][__builtin_popcount(m2)], 1);
            LST_ADD(lst_distu[__builtin_popcount(m1 | m2)], 1);
            if (vminv_u8(vceq_u8(vld1_u8(lb.t1 + 8 * g), vld1_u8(lb.t2 + 8 * g)))) LST_ADD(lst_same12, 1);
            if (S->nx) LST_ADD(lst_dist[2][__builtin_popcount(lst_distinct(vld1_u8(lbn.t1 + 8 * g)))], 1);
        }
#endif

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        int32x4_t sss[2], sst[2];
        int32x4_t nsss[2], nsst[2];
        for (int h = 0; h < 2; h++) {
            int32x4_t a, bb;
            if (S->dolod) {
                a = vld1q_s32(lrs + o + 4 * h); bb = vld1q_s32(lrt + o + 4 * h);
                sss[h] = clamp17v(a); sst[h] = clamp17v(bb);
            } else {
                tc_divclamp4(persp, vld1q_s32(&b->s[o + 4 * h]), vld1q_s32(&b->t[o + 4 * h]), vld1q_s32(&b->w[o + 4 * h]), &sss[h], &sst[h]);
            }
            if (S->nx) {
                if (lodtiles) {
                    a = vld1q_s32(nrs + o + 4 * h); bb = vld1q_s32(nrt + o + 4 * h);
                    nsss[h] = clamp17v(a); nsst[h] = clamp17v(bb);
                } else {
                    int32x4_t s = vld1q_s32(&b->s[o + 4 * h]), t = vld1q_s32(&b->t[o + 4 * h]), w = vld1q_s32(&b->w[o + 4 * h]);
                    tc_divclamp4(persp, vaddq_s32(s, dsi), vaddq_s32(t, dti), vaddq_s32(w, dwi), &nsss[h], &nsst[h]);
                }
            }
        }
        if (S->t0) {
            int16x8_t t0[4];
            if (lodtiles) tex_cycle_tiles(r, cxs0, &have0, &S->c0, S->lodfix ? &S->c0 : NULL, vld1_u8(lb.t1 + o), sss, sst, t0);
            else tex_cycle(r, &cx0, &cx0.sp, sss, sst, t0);
            for (int c = 0; c < 4; c++) vst1q_s16(&b->tex0[c][o], t0[c]);
        }
        if (two && S->t1) {
            int16x8_t t1[4];
            if (lodtiles) tex_cycle_tiles(r, cxs1, &have1, &S->c1, S->lodfix ? &S->c1 : NULL, vld1_u8(lb.t2 + o), sss, sst, t1);
            else tex_cycle(r, &cx1, &cx1.sp, sss, sst, t1);
            for (int c = 0; c < 4; c++) vst1q_s16(&b->tex1[c][o], t1[c]);
        }
        if (S->nx) {
            int16x8_t tn[4];
            if (lodtiles) tex_cycle_tiles(r, cxsn, &haven, &S->c0, S->lodfix ? &S->c0 : NULL, vld1_u8(lbn.t1 + o), nsss, nsst, tn);
            else tex_cycle(r, &cx0, &cx0.sp, nsss, nsst, tn);
            for (int c = 0; c < 4; c++) {
                vst1q_s16(&b->tex0n[c][o], tn[c]);
                if (!two) vst1q_s16(&b->tex1[c][o], tn[c]);
            }
        }
    }
}

/* LOD selects tile tilenum + 0 .. max_level for cycle 0 and 1; detail textures shift the chain up by one */
FOIR_INLINE int lod_chain_last(const struct rdp *r) { return r->pc.max_level + (r->om.detail_tex_en ? 1 : 0); }

void neon_texture(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    const int two = om->cycle_type == CYCLE_TYPE_2;
    const int prim_tile = r->pc.tilenum;
    const struct tile *tl0 = &r->tile[prim_tile];
    const struct tile *tl1 = &r->tile[(prim_tile + 1) & 7];

    const int pointplain = !(om->sample_type || om->en_tlut);
    int unsupp = tl0->format == FORMAT_YUV || (!om->bi_lerp0 && !pointplain);
    if (two) unsupp |= tl1->format == FORMAT_YUV || (!om->bi_lerp1 && !pointplain) || om->convert_one;
    if (om->tex_lod_en)
        for (int k = 0; k <= lod_chain_last(r) && k < 8; k++)
            unsupp |= r->tile[(prim_tile + k) & 7].format == FORMAT_YUV;
#ifdef FOIR_STATS
    if (unsupp) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "texture fallback: dolod=%d tex_lod_en=%d yuv0=%d bilerp0=%d pointplain=%d two=%d bilerp1=%d conv=%d tlut=%d sample=%d\n", om->dolod, om->tex_lod_en,
                    tl0->format == FORMAT_YUV, om->bi_lerp0, pointplain, two, om->bi_lerp1, om->convert_one, om->en_tlut, om->sample_type);
    }
#endif
    if (unsupp) { NEON_FALLBACK(0); foir_kernels_scalar.texture(r, b); return; }
    if (!(r->pf.need_t0 || r->pf.need_t1 || r->pf.next_texel || om->dolod)) return;

    const struct tspec S = {
        .two = two, .t0 = r->pf.need_t0, .t1 = r->pf.need_t1, .nx = r->pf.next_texel,
        .dolod = om->dolod, .lodtiles = om->tex_lod_en, .persp = om->persp_tex_en,
        .c0 = { tl0->size, tl0->format, om->en_tlut, om->tlut_type, om->sample_type, TSP_MID_RT, om->bi_lerp0 },
        .c1 = { tl1->size, tl1->format, om->en_tlut, om->tlut_type, om->sample_type, TSP_MID_RT, om->bi_lerp1 },
    };
    texture_body(r, b, &S);
}

/* COPY: address arithmetic runs in 16-bit lanes (only the low 11-12 bits of a TMEM address are used and every operation is exact modulo 2^16);
 * windows start at an even framebuffer pixel so each is one vector load / store of the byte-swapped RDRAM */

#define COPY_MAXG 272          /* groups of four pixels per span (<= 1024 pixels) */

/* byte indices (vqtbl1q_u8 on four int16 group values) and in-group pixel numbers of a window whose first lane has phase ph = span offset & 3 */
#define CPT(a, b, c, d, e, f, g, h) { 2 * a, 2 * a + 1, 2 * b, 2 * b + 1, 2 * c, 2 * c + 1, 2 * d, 2 * d + 1, \
                                      2 * e, 2 * e + 1, 2 * f, 2 * f + 1, 2 * g, 2 * g + 1, 2 * h, 2 * h + 1 }
static const uint8_t copy_gtbl[4][16] = {
    CPT(0, 0, 0, 0, 1, 1, 1, 1), CPT(0, 0, 0, 1, 1, 1, 1, 2), CPT(0, 0, 1, 1, 1, 1, 2, 2), CPT(0, 1, 1, 1, 1, 2, 2, 2) };
static const int16_t copy_qtbl[4][8] = {
    { 0, 1, 2, 3, 0, 1, 2, 3 }, { 1, 2, 3, 0, 1, 2, 3, 0 }, { 2, 3, 0, 1, 2, 3, 0, 1 }, { 3, 0, 1, 2, 3, 0, 1, 2 } };
#undef CPT

enum { CK_RAW16, CK_TLUT8, CK_TLUT4, CK_TLUT16, CK_GENERIC };

FOIR_INLINE uint16x8_t copy_fetch(const struct rdp *r, const struct tile *tl, const int kind, int16x8_t ss, int16x8_t tt)
{
    const uint8_t *tm = r->tmem;
    int16x8_t tbase = vmlaq_n_s16(vdupq_n_s16((int16_t)tl->tmem), vandq_s16(tt, vdupq_n_s16(0xff)), (int16_t)tl->line);
    int16x8_t odd = vandq_s16(tt, vdupq_n_s16(1));
    uint16_t ad[8] ALIGN16;
    switch (kind) {
    case CK_RAW16:
    case CK_TLUT16: {
        int16x8_t i = veorq_s16(vaddq_s16(vshlq_n_s16(tbase, 2), ss), vshlq_n_s16(odd, 1));
        vst1q_u16(ad, vreinterpretq_u16_s16(i));
        uint16x8_t v = gather16(tm, ad);
        if (kind == CK_RAW16) return v;
        vst1q_u16(ad, vaddq_u16(vdupq_n_u16(0x400), vshlq_n_u16(vshrq_n_u16(v, 8), 2)));
        return gather16(tm, ad);
    }
    case CK_TLUT8: {
        int16x8_t a = veorq_s16(vaddq_s16(vshlq_n_s16(tbase, 3), ss), vshlq_n_s16(odd, 2));
        vst1q_u16(ad, vreinterpretq_u16_s16(a));
        uint16x8_t idx = vmovl_u8(gather8(tm, ad));
        vst1q_u16(ad, vaddq_u16(vdupq_n_u16(0x400), vshlq_n_u16(idx, 2)));
        return gather16(tm, ad);
    }
    default: {
        int16x8_t x = vaddq_s16(vshlq_n_s16(tbase, 4), ss);
        int16x8_t a = veorq_s16(vreinterpretq_s16_u16(vshrq_n_u16(vreinterpretq_u16_s16(x), 1)), vshlq_n_s16(odd, 2));
        vst1q_u16(ad, vreinterpretq_u16_s16(a));
        int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(gather8(tm, ad)));
        int16x8_t nib = vbslq_s16(vtstq_s16(x, vdupq_n_s16(1)), vandq_s16(v, vdupq_n_s16(0xf)), vshrq_n_s16(v, 4));
        int16x8_t idx = vandq_s16(vorrq_s16(nib, vdupq_n_s16((int16_t)(tl->palette << 4))), vdupq_n_s16(0xff));
        vst1q_u16(ad, vaddq_u16(vdupq_n_u16(0x400), vshlq_n_u16(vreinterpretq_u16_s16(idx), 2)));
        return gather16(tm, ad);
    }
    }
}

FOIR_INLINE void copy_body(struct rdp *r, int y, const struct span *sp, const int kind)
{
    const struct prim_ctx *pc = &r->pc;
    const struct tile *tl = &r->tile[pc->tilenum];
    const int length = sp->lx - sp->rx;                  /* flip: the span runs from rx up to lx */
    const int xs = sp->rx;
    int n = length + 1;
    if (xs + n > r->fb_width) n = r->fb_width - xs;
    if (n <= 0) return;
    const uint32_t rs = (r->fb_address >> 1) + (uint32_t)(r->fb_width * y);
    const uint32_t i0 = rs + (uint32_t)xs;
    const int head = (int)(i0 & 1);

    int16_t gsb[COPY_MAXG + 16] ALIGN16, gtb[COPY_MAXG + 16] ALIGN16;
    int16_t *GS = gsb + 4, *GT = gtb + 4;
    memset(gsb, 0, 8); memset(gtb, 0, 8);
    const int ng = (n + 3) >> 2;
    {
        static const int32_t lane[4] = { 0, 1, 2, 3 };
        const int32x4_t iota = vld1q_s32(lane);
        const int persp = r->om.persp_tex_en;
        const int shs = tl->shift_s, sht = tl->shift_t;
        const int32x4_t slr = S32(tl->sl << 3), tlr = S32(tl->tl << 3);
        for (int g0 = 0; g0 < ng; g0 += 4) {
            int32x4_t gi = vaddq_s32(S32(g0), iota);
            int32x4_t s = vmlaq_n_s32(S32(sp->s), gi, pc->ds), t = vmlaq_n_s32(S32(sp->t), gi, pc->dt);
            int32x4_t w = vmlaq_n_s32(S32(sp->w), gi, pc->dw);
            int32x4_t a, b;
            tc_divide4(persp, s, t, w, &a, &b);
            int32x4_t cs = sign16(clamp17v(a)), ct = sign16(clamp17v(b));
            cs = shs < 11 ? vshlq_s32(cs, S32(-shs)) : sign16(vshlq_s32(cs, S32(16 - shs)));
            ct = sht < 11 ? vshlq_s32(ct, S32(-sht)) : sign16(vshlq_s32(ct, S32(16 - sht)));
            vst1_s16(GS + g0, vmovn_s32(vshrq_n_s32(vsubq_s32(cs, slr), 5)));
            vst1_s16(GT + g0, vmovn_s32(vshrq_n_s32(vsubq_s32(ct, tlr), 5)));
        }
        memset(GS + ng, 0, 8); memset(GT + ng, 0, 8);
    }

    const int16x8_t smask = vdupq_n_s16(tl->mask_s ? (int16_t)((1 << (tl->mask_s > 10 ? 10 : tl->mask_s)) - 1) : -1);
    const int16x8_t tmask = vdupq_n_s16(tl->mask_t ? (int16_t)((1 << (tl->mask_t > 10 ? 10 : tl->mask_t)) - 1) : -1);
    static const int16_t lane16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const int16x8_t iota16 = vld1q_s16(lane16);
    const int16x8_t nvec = vdupq_n_s16((int16_t)n);
    const int acmp = r->om.alpha_compare_en;
    const int nwin = (n + head + 7) >> 3;
    const uint32_t rowend = rs + (uint32_t)r->fb_width;
    uint16_t *const h16 = (uint16_t *)r->rdram;
    struct tsp gsp = { tl->size, tl->format, 0, 0, 0, 0, 1 };
    (void)gsp;

    for (int m = 0; m < nwin; m++) {
        const int jw = 8 * m - head;
        const int g0 = jw >> 2, ph = jw & 3;
        int16x8_t jv = vaddq_s16(vdupq_n_s16((int16_t)jw), iota16);
        uint16x8_t mask = vandq_u16(vcgeq_s16(jv, vdupq_n_s16(0)), vcltq_s16(jv, nvec));
        uint8x16_t tix = vld1q_u8(copy_gtbl[ph]);
        int16x8_t ss = vaddq_s16(vreinterpretq_s16_u8(vqtbl1q_u8(vreinterpretq_u8_s16(vcombine_s16(vld1_s16(GS + g0), vdup_n_s16(0))), tix)),
                                 vld1q_s16(copy_qtbl[ph]));
        int16x8_t tt = vreinterpretq_s16_u8(vqtbl1q_u8(vreinterpretq_u8_s16(vcombine_s16(vld1_s16(GT + g0), vdup_n_s16(0))), tix));
        if (tl->mask_s) ss = vandq_s16(ss, smask);
        if (tl->mask_t) tt = vandq_s16(tt, tmask);

        uint16x8_t px;
        if (kind == CK_GENERIC) {
            int32x4_t S[2] = { vmovl_s16(vget_low_s16(ss)), vmovl_s16(vget_high_s16(ss)) };
            int32x4_t T[2] = { vmovl_s16(vget_low_s16(tt)), vmovl_s16(vget_high_s16(tt)) };
            struct rowt R[2] = { row_terms(tl, &gsp, T[0]), row_terms(tl, &gsp, T[1]) };
            int16x8_t c[4];
            fetch_taps(r, tl, &gsp, R, S, c);
            int16x8_t pr = vorrq_s16(vorrq_s16(vshlq_n_s16(vandq_s16(c[0], vdupq_n_s16(0xf8)), 8), vshlq_n_s16(vandq_s16(c[1], vdupq_n_s16(0xf8)), 3)),
                                     vshrq_n_s16(vandq_s16(c[2], vdupq_n_s16(0xf8)), 2));
            px = vreinterpretq_u16_s16(vorrq_s16(pr, vandq_s16(vreinterpretq_s16_u16(vcgeq_s16(c[3], vdupq_n_s16(0x80))), vdupq_n_s16(1))));
        } else {
            px = copy_fetch(r, tl, kind, ss, tt);
        }
        if (acmp) mask = vandq_u16(mask, vtstq_u16(px, vdupq_n_u16(1)));
        if (vmaxvq_u16(mask) == 0) continue;

        const uint32_t W = i0 - (uint32_t)head + 8u * (uint32_t)m;
        if (W >= rs && W + 8 <= rowend) {
            uint8x8_t hn = vorr_u8(vdup_n_u8(8), vmul_u8(vand_u8(vmovn_u16(px), vdup_n_u8(1)), vdup_n_u8(7)));
            if (vminvq_u16(mask) == 0xffff) {
                vst1q_u16(h16 + W, vrev32q_u16(px));
                vst1_u8(r->hidden + W, hn);
            } else {
                uint16x8_t old = vrev32q_u16(vld1q_u16(h16 + W));
                vst1q_u16(h16 + W, vrev32q_u16(vbslq_u16(mask, px, old)));
                vst1_u8(r->hidden + W, vbsl_u8(vmovn_u16(mask), hn, vld1_u8(r->hidden + W)));
            }
        } else {
            /* the window leaves the row (the neighbouring rows may belong to another worker): per pixel */
            uint16_t pa[8] ALIGN16, ma[8] ALIGN16;
            vst1q_u16(pa, px); vst1q_u16(ma, mask);
            for (int l = 0; l < 8; l++)
                if (ma[l]) rdram_wr_pair16(r, W + (uint32_t)l, pa[l], (uint8_t)((pa[l] & 1) ? 3 : 0));
        }
    }
}

void neon_copy(struct rdp *r, int y, const struct span *sp, int flip)
{
    const struct tile *tl = &r->tile[r->pc.tilenum];
    int kind;
    if (r->om.en_tlut) kind = tl->size == PIXEL_SIZE_8BIT ? CK_TLUT8 : tl->size == PIXEL_SIZE_4BIT ? CK_TLUT4 : CK_TLUT16;
    else kind = tl->size == PIXEL_SIZE_16BIT ? CK_RAW16 : tl->format == FORMAT_YUV ? -1 : CK_GENERIC;
    const int length = sp->lx - sp->rx;
    const int xs = sp->rx;
    if (kind < 0 || !flip || r->fb_size != PIXEL_SIZE_16BIT || xs < 0 || length >= 1024) {
        NEON_FALLBACK(8);
        foir_kernels_scalar.copy(r, y, sp, flip);
        return;
    }
    if (length < 0) return;
    {
        int n = length + 1;
        if (xs + n > r->fb_width) n = r->fb_width - xs;
        const uint32_t i0 = (r->fb_address >> 1) + (uint32_t)(r->fb_width * y) + (uint32_t)xs;
        if (n <= 0) return;
        if (i0 + (uint32_t)n + 16 >= (RDRAM_MASK >> 1) || ((i0 + (uint32_t)n + 8) << 1) > r->rdram_size) {
            NEON_FALLBACK(8);
            foir_kernels_scalar.copy(r, y, sp, flip);
            return;
        }
    }
    NEON_HANDLED(8);
    switch (kind) {
    case CK_RAW16:  copy_body(r, y, sp, CK_RAW16); break;
    case CK_TLUT8:  copy_body(r, y, sp, CK_TLUT8); break;
    case CK_TLUT4:  copy_body(r, y, sp, CK_TLUT4); break;
    case CK_TLUT16: copy_body(r, y, sp, CK_TLUT16); break;
    default:        copy_body(r, y, sp, CK_GENERIC); break;
    }
}

#include "kspec_tex.inc"
