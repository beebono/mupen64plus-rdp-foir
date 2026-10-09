/*
 * mupen64plus-rdp-foir - kneon_tex.c
 * NEON texture unit: perspective divide, tile coordinate processing (shift,
 * clamp, mask, mirror), TMEM texel fetch for every non-YUV format (with TLUT)
 * and point / bilinear / mid-texel filtering, eight pixels per step.
 *
 * Coordinate math runs in int32x4 halves (the intermediate products need the
 * headroom), TMEM is read with per-lane gathers (the address arithmetic is
 * vectorised), and the filtering runs on 16-bit lanes.
 *
 * Not handled here (the scalar kernel runs instead): LOD / detail / sharpen,
 * YUV tiles, the colour-convert (YUV) cycle modes.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kneon.h"
#include "spec.h"
#ifdef FOIR_STATS
#include <stdio.h>
#endif

#define S32(x) vdupq_n_s32(x)

FOIR_INLINE int32x4_t sign16(int32x4_t x) { return vshrq_n_s32(vshlq_n_s32(x, 16), 16); }

/* ---- perspective divide + clamp17 ------------------------------------------------------- */

FOIR_INLINE void tc_divide4(int persp, int32x4_t ss, int32x4_t st, int32x4_t sw,
                            int32x4_t *sss, int32x4_t *sst)
{
    if (!persp) {
        *sss = vandq_s32(sign16(ss), S32(0x1ffff));
        *sst = vandq_s32(sign16(st), S32(0x1ffff));
        return;
    }
    uint32x4_t wcarry = vcleq_s32(sign16(sw), S32(0));
    int32_t idx[4] ALIGN16, tab[4] ALIGN16;
    vst1q_s32(idx, vandq_s32(sw, S32(0x7fff)));
    tab[0] = k_tcdiv[idx[0]]; tab[1] = k_tcdiv[idx[1]]; tab[2] = k_tcdiv[idx[2]]; tab[3] = k_tcdiv[idx[3]];
    int32x4_t tv = vld1q_s32(tab);
    int32x4_t shift = vandq_s32(tv, S32(0xf));
    int32x4_t rcp = vshrq_n_s32(tv, 4);

    int32x4_t sprod = vmulq_s32(sign16(ss), rcp), tprod = vmulq_s32(sign16(st), rcp);
    int32x4_t tempmask = vandq_s32(vnegq_s32(vshlq_s32(S32(1 << 29), vnegq_s32(shift))), S32((1 << 30) - 1));
    int32x4_t oob_s = vandq_s32(sprod, tempmask), oob_t = vandq_s32(tprod, tempmask);
    int32x4_t cnt = vsubq_s32(shift, S32(13));           /* right shift by 13 - shift (left by 1 when shift == 14) */
    int32x4_t temps = vshlq_s32(sprod, cnt), tempt = vshlq_s32(tprod, cnt);
    uint32x4_t is_e = vceqq_s32(shift, S32(0xe));
    int32x4_t chk_s = vbslq_s32(is_e, sprod, temps), chk_t = vbslq_s32(is_e, tprod, tempt);

    uint32x4_t cs = vandq_u32(vmvnq_u32(vceqq_s32(oob_s, tempmask)), vmvnq_u32(vceqq_s32(oob_s, S32(0))));
    uint32x4_t ct = vandq_u32(vmvnq_u32(vceqq_s32(oob_t, tempmask)), vmvnq_u32(vceqq_s32(oob_t, S32(0))));
    int32x4_t ous = vbslq_s32(vtstq_s32(chk_s, S32(1 << 29)), S32(1 << 17), S32(2 << 17));
    int32x4_t out = vbslq_s32(vtstq_s32(chk_t, S32(1 << 29)), S32(1 << 17), S32(2 << 17));
    ous = vandq_s32(ous, vreinterpretq_s32_u32(cs));
    out = vandq_s32(out, vreinterpretq_s32_u32(ct));
    int32x4_t wc = vandq_s32(S32(2 << 17), vreinterpretq_s32_u32(wcarry));
    ous = vorrq_s32(ous, wc);
    out = vorrq_s32(out, wc);
    *sss = vorrq_s32(vandq_s32(temps, S32(0x1ffff)), ous);
    *sst = vorrq_s32(vandq_s32(tempt, S32(0x1ffff)), out);
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

/* ---- per-axis tile coordinate processing ------------------------------------------------------ */

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

/* bilinear / TLUT flavour: texel coordinate, fraction and the "next texel" step (tcclamp_cycle + tcmask_coupled) */
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

/* point flavour (tcclamp_cycle_light + tcmask) */
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

/* ---- TMEM gathers + format decode ------------------------------------------------------------- */

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

FOIR_INLINE int16x8_t expand5(uint16x8_t x)    /* 5-bit -> 8-bit replicate */
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

#define TSP_MID_RT 2       /* tsp.mid: MID_TEXEL comes from the render state (generic kernel, "any format" kernels) */

/* What one texture cycle depends on besides the tile's coordinate parameters (shift / clamp / mask, which stay run time
 * data): the texel format and size, TLUT use and the filter. The generic kernel builds it from the render state, a
 * specialised variant (kspec_tex.inc) passes a literal one and has everything folded. */
struct tsp {
    int size, fmt;        /* texel size (PIXEL_SIZE_*) and format of the tile */
    int tlut, tlt;        /* EN_TLUT, TLUT_TYPE (IA) */
    int smp, mid;         /* SAMPLE_TYPE (bilinear), MID_TEXEL (TSP_MID_RT: read from the render state) */
    int bilerp;           /* BI_LERP of this cycle; 0 selects the colour-conversion arithmetic */
};

/* Per-row address terms for 4 lanes: base = TMEM row start in address units, swp = odd-row word swap */
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

/* TMEM addresses (halfword index for 16/32-bit texels, byte address for 4/8-bit) of 4 lanes */
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

/* gather + decode 8 lanes whose addresses are in ad[] (nib[] holds the nibble selector for 4-bit texels) */
static void fetch_decode(const struct rdp *r, const struct tile *tl, const struct tsp *sp, const uint16_t *ad, const uint16_t *nib,
                         int16x8_t out[4])
{
    const uint8_t *tm = r->tmem;
    const int size = sp->size, fmt = sp->fmt;
    const bool tlut = sp->tlut;

    /* raw texel -> either an 8-bit palette index (TLUT) or colour channels */
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

/* ---- one texture cycle on 8 lanes ------------------------------------------------------------------ */

FOIR_INLINE int16x8_t narrow16(int32x4_t a, int32x4_t b) { return vcombine_s16(vmovn_s32(a), vmovn_s32(b)); }

struct tctx {
    const struct tile *tl;
    struct axis as, at;
    struct tsp sp;
    int mid;                      /* MID_TEXEL of this context (a literal in the exact variants; only used with bilinear sampling) */
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

/* MID_TEXEL (bilinear sampling, lanes that sit at the centre of their texel use all four taps) is read from the state in every
 * variant, but it is rare: a sample in which some lane is at a texel centre is handed to tex_cycle_mid(), an out-of-line copy of
 * this function in which the four-tap blend is compiled in (midmode = 1), so the code of the common path is exactly that of a
 * variant that was built without MID_TEXEL. */
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
    if (!sp->smp) {                  /* TLUT with point sampling: a single texel */
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
            /* (copies, so that the state of the caller does not have to live in memory) */
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
    fetch_taps(r, tl, sp, R0, S1, t1);                 /* (s + sdiff, t)      */
    fetch_taps(r, tl, sp, R1, S, t2);                  /* (s, t + tdiff)      */
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

    /* mid-texel lanes present (midmode only): all four taps */
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

/* ---- level of detail ------------------------------------------------------------------------------------ */

FOIR_INLINE int32x4_t sign17(int32x4_t x) { return vshrq_n_s32(vshlq_n_s32(x, 15), 15); }

/* |next - cur| in the RDP's 17-bit sign/magnitude flavour, then the larger of s and t and of the previous value */
FOIR_INLINE int32x4_t lod_delta(int32x4_t scur, int32x4_t snext, int32x4_t tcur, int32x4_t tnext, int32x4_t previous)
{
    int32x4_t ds = vsubq_s32(sign17(snext), sign17(scur)), dt = vsubq_s32(sign17(tnext), sign17(tcur));
    ds = vbslq_s32(vtstq_s32(ds, S32(0x20000)), vandq_s32(vmvnq_s32(ds), S32(0x1ffff)), ds);
    dt = vbslq_s32(vtstq_s32(dt, S32(0x20000)), vandq_s32(vmvnq_s32(dt), S32(0x1ffff)), dt);
    return vmaxq_s32(vmaxq_s32(ds, dt), previous);
}

/* lod_4x17_to_15: keep 15 bits and flag overflow of the three bits above with bit 14 */
FOIR_INLINE int32x4_t lod_fold(int32x4_t d)
{
    return vorrq_s32(vandq_s32(d, S32(0x7fff)), vbslq_s32(vtstq_s32(d, S32(0x1c000)), S32(0x4000), S32(0)));
}

struct lodres {
    int16x8_t lf;           /* LOD fraction (0..0x1ff) */
    uint8x8_t t1, t2;       /* tiles of the first / second texture cycle */
};

/* Port of lod_select() for eight pixels. s,t,w are the raw interpolants; is/it the divided coordinates of the
 * pixel itself (before the 16-bit clamp, with the over/underflow flags). */
static __attribute__((noinline)) void lod_group(const struct rdp *r, const int32x4_t s[2], const int32x4_t t[2], const int32x4_t w[2],
                      const int32x4_t is[2], const int32x4_t it[2], struct lodres *res)
{
    const struct prim_ctx *pc = &r->pc;
    const struct other_modes *om = &r->om;
    const int32_t min_level = r->min_level, max_level = pc->max_level;
    int16x4_t lf16[2];
    int32x4_t tl1[2], tl2[2];

    for (int h = 0; h < 2; h++) {
        int32x4_t ns, nt, nys, nyt;
        tc_divide4(om->persp_tex_en, vshrq_n_s32(vaddq_s32(s[h], S32(pc->dsinc)), 16), vshrq_n_s32(vaddq_s32(t[h], S32(pc->dtinc)), 16),
                   vshrq_n_s32(vaddq_s32(w[h], S32(pc->dwinc)), 16), &ns, &nt);
        tc_divide4(om->persp_tex_en, vshrq_n_s32(vaddq_s32(s[h], S32(pc->dsdy)), 16), vshrq_n_s32(vaddq_s32(t[h], S32(pc->dtdy)), 16),
                   vshrq_n_s32(vaddq_s32(w[h], S32(pc->dwdy)), 16), &nys, &nyt);
        int32x4_t flags = vorrq_s32(vorrq_s32(vorrq_s32(is[h], it[h]), vorrq_s32(ns, nt)), vorrq_s32(nys, nyt));
        /* lodclamp = any of the six values has bit 17 or 18 set */
        uint32x4_t clampm = vtstq_s32(flags, S32(0x60000));
        int32x4_t d1 = lod_delta(is[h], ns, it[h], nt, S32(0));
        int32x4_t lod1 = lod_fold(d1);
        int32x4_t d2 = lod_delta(is[h], nys, it[h], nyt, lod1);
        int32x4_t lod = lod_fold(d2);
        lod = vbicq_s32(lod, vreinterpretq_s32_u32(clampm));      /* lodclamp -> lod = 0 */

        /* lodfrac_lodtile */
        uint32x4_t c1 = vorrq_u32(vtstq_s32(lod, S32(0x4000)), clampm);
        uint32x4_t c2 = vandq_u32(vmvnq_u32(c1), vcltq_s32(lod, S32(min_level)));
        uint32x4_t c3 = vandq_u32(vandq_u32(vmvnq_u32(c1), vmvnq_u32(c2)), vcltq_s32(lod, S32(32)));
        uint32x4_t c4 = vmvnq_u32(vorrq_u32(vorrq_u32(c1, c2), c3));
        int32x4_t lead = vreinterpretq_s32_u32(vclzq_u32(vreinterpretq_u32_s32(vandq_s32(vshrq_n_s32(lod, 5), S32(0xff)))));
        int32x4_t ltil4 = vmaxq_s32(vsubq_s32(S32(31), lead), S32(0));
        ltil4 = vbicq_s32(ltil4, vreinterpretq_s32_u32(vmvnq_u32(c4)));          /* only the minification case */
        uint32x4_t mag = vorrq_u32(c2, c3);
        uint32x4_t dis;
        if (max_level) {
            dis = vorrq_u32(vtstq_s32(lod, S32(0x6000)), vcgeq_s32(ltil4, S32(max_level)));
            dis = vbslq_u32(c4, dis, vbslq_u32(c1, vdupq_n_u32(~0u), vdupq_n_u32(0)));
        } else {
            dis = vdupq_n_u32(~0u);
        }
        dis = vbslq_u32(c1, vdupq_n_u32(~0u), dis);
        int32x4_t lf;
        const int plain = !om->sharpen_tex_en && !om->detail_tex_en;
        int32x4_t lf_mid = plain ? vbslq_s32(dis, S32(0xff), S32(0))
                                 : vorrq_s32(vshlq_n_s32(lod, 3), S32(om->sharpen_tex_en ? 0x100 : 0));
        int32x4_t lf_min = plain ? vbslq_s32(dis, S32(0xff), S32(0))
                                 : S32((min_level << 3) | (om->sharpen_tex_en ? 0x100 : 0));
        int32x4_t lf_far = vandq_s32(vshlq_s32(vshlq_n_s32(lod, 3), vnegq_s32(ltil4)), S32(0xff));
        if (plain) lf_far = vbslq_s32(dis, S32(0xff), lf_far);
        lf = vbslq_s32(c1, S32(0xff), vbslq_s32(c2, lf_min, vbslq_s32(c3, lf_mid, lf_far)));
        lf16[h] = vmovn_s32(lf);

        /* tile selection (only meaningful with TEX_LOD_EN) */
        int32x4_t ltile = vbslq_s32(dis, S32(max_level), ltil4);
        int32x4_t base = vaddq_s32(S32(pc->tilenum), ltile);
        uint32x4_t sameTile = vorrq_u32(dis, vandq_u32(mag, vdupq_n_u32(om->sharpen_tex_en ? 0 : ~0u)));
        if (!om->detail_tex_en) {
            int32x4_t t1v = vandq_s32(base, S32(7));
            int32x4_t t2v = vbslq_s32(sameTile, t1v, vandq_s32(vaddq_s32(t1v, S32(1)), S32(7)));
            tl1[h] = t1v; tl2[h] = t2v;
        } else {
            int32x4_t t1v = vandq_s32(vaddq_s32(base, vbslq_s32(mag, S32(0), S32(1))), S32(7));
            uint32x4_t far2 = vandq_u32(vmvnq_u32(dis), vmvnq_u32(mag));
            int32x4_t t2v = vandq_s32(vaddq_s32(base, vbslq_s32(far2, S32(2), S32(1))), S32(7));
            tl1[h] = t1v; tl2[h] = t2v;
        }
    }
    res->lf = vcombine_s16(lf16[0], lf16[1]);
    if (om->tex_lod_en) {
        res->t1 = vmovn_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(tl1[0])), vmovn_u32(vreinterpretq_u32_s32(tl1[1]))));
        res->t2 = vmovn_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(tl2[0])), vmovn_u32(vreinterpretq_u32_s32(tl2[1]))));
    } else {
        res->t1 = vdup_n_u8((uint8_t)pc->tilenum);
        res->t2 = vdup_n_u8((uint8_t)((pc->tilenum + 1) & 7));
    }
}

/* One texture cycle with a per-lane tile: every distinct tile of the group gets a pass, results are merged */
static void tex_cycle_tiles(const struct rdp *r, struct tctx *cxs, uint32_t *have, const struct tsp *base, const struct tsp *fixed, uint8x8_t ids,
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
            /* a specialised variant knows that every tile of the mip chain has the format it was built for (checked per batch) */
            const struct tsp sp = fixed ? *fixed : (struct tsp){ tv->size, tv->format, base->tlut, r->om.tlut_type, base->smp, base->mid, base->bilerp };
            tctx_init(&cxs[v], r, tv, &sp);
            *have |= 1u << v;
        }
        int16x8_t t[4];
        tex_cycle(r, &cxs[v], fixed ? fixed : &cxs[v].sp, sss, sst, t);
        for (int c = 0; c < 4; c++) out[c] = vbslq_s16(m16, t[c], out[c]);
    }
}

/* ---- kernel ---------------------------------------------------------------------------------------------- */

/* What the texture stage depends on besides the tiles' coordinate parameters (see struct tsp). */
struct tspec {
    int two;                 /* two-cycle: a second texture cycle on tile + 1 */
    int t0, t1, nx;          /* TEXEL0, second cycle, next-pixel TEXEL1 needed */
    int dolod, lodtiles;     /* LOD fraction needed, LOD selects the tile (TEX_LOD_EN) */
    int lodfix;              /* variants: all tiles the LOD can select have the format c0 / c1 describe */
    int persp;
    struct tsp c0, c1;
    int anyfmt;              /* "any format" variants: size, format and TLUT type of the tiles come from the render state */
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
    (void)om;

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        int32x4_t sss[2], sst[2], rs[2], rt[2], S_[2], T[2], W[2];
        int32x4_t nsss[2], nsst[2];
        for (int h = 0; h < 2; h++) {
            int32x4_t s = vld1q_s32(&b->s[o + 4 * h]), t = vld1q_s32(&b->t[o + 4 * h]), w = vld1q_s32(&b->w[o + 4 * h]);
            S_[h] = s; T[h] = t; W[h] = w;
            int32x4_t a, bb;
            tc_divide4(S->persp, vshrq_n_s32(s, 16), vshrq_n_s32(t, 16), vshrq_n_s32(w, 16), &a, &bb);
            rs[h] = a; rt[h] = bb;
            sss[h] = clamp17v(a); sst[h] = clamp17v(bb);
            if (S->nx) {
                tc_divide4(S->persp, vshrq_n_s32(vaddq_s32(s, dsi), 16), vshrq_n_s32(vaddq_s32(t, dti), 16),
                           vshrq_n_s32(vaddq_s32(w, dwi), 16), &a, &bb);
                nsss[h] = clamp17v(a); nsst[h] = clamp17v(bb);
            }
        }
        struct lodres lr = { vdupq_n_s16(0), vdup_n_u8(0), vdup_n_u8(0) };
        if (S->dolod) {
            lod_group(r, S_, T, W, rs, rt, &lr);
            vst1q_s16(&b->lodfrac[o], lr.lf);
        }
        if (S->t0) {
            int16x8_t t0[4];
            if (lodtiles) tex_cycle_tiles(r, cxs0, &have0, &S->c0, S->lodfix ? &S->c0 : NULL, lr.t1, sss, sst, t0);
            else tex_cycle(r, &cx0, &cx0.sp, sss, sst, t0);
            for (int c = 0; c < 4; c++) vst1q_s16(&b->tex0[c][o], t0[c]);
        }
        if (two && S->t1) {
            int16x8_t t1[4];
            if (lodtiles) tex_cycle_tiles(r, cxs1, &have1, &S->c1, S->lodfix ? &S->c1 : NULL, lr.t2, sss, sst, t1);
            else tex_cycle(r, &cx1, &cx1.sp, sss, sst, t1);
            for (int c = 0; c < 4; c++) vst1q_s16(&b->tex1[c][o], t1[c]);
        }
        if (S->nx) {
            int16x8_t tn[4];
            if (lodtiles) {
                /* the next pixel is evaluated as a pixel of its own: its LOD comes from its own neighbours */
                int32x4_t S2[2], T2[2], W2[2];
                for (int h = 0; h < 2; h++) { S2[h] = vaddq_s32(S_[h], dsi); T2[h] = vaddq_s32(T[h], dti); W2[h] = vaddq_s32(W[h], dwi); }
                int32x4_t a, bb, ns[2], nt[2];
                for (int h = 0; h < 2; h++) {
                    tc_divide4(S->persp, vshrq_n_s32(S2[h], 16), vshrq_n_s32(T2[h], 16), vshrq_n_s32(W2[h], 16), &a, &bb);
                    ns[h] = a; nt[h] = bb;
                }
                struct lodres ln;
                lod_group(r, S2, T2, W2, ns, nt, &ln);
                tex_cycle_tiles(r, cxsn, &haven, &S->c0, S->lodfix ? &S->c0 : NULL, ln.t1, nsss, nsst, tn);
            } else {
                tex_cycle(r, &cx0, &cx0.sp, nsss, nsst, tn);
            }
            for (int c = 0; c < 4; c++) {
                vst1q_s16(&b->tex0n[c][o], tn[c]);
                if (!two) vst1q_s16(&b->tex1[c][o], tn[c]);
            }
        }
    }
}

void neon_texture(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    const int two = om->cycle_type == CYCLE_TYPE_2;
    const int prim_tile = r->pc.tilenum;
    const struct tile *tl0 = &r->tile[prim_tile];
    const struct tile *tl1 = &r->tile[(prim_tile + 1) & 7];

    /* unsupported: YUV tiles, the conversion arithmetic on filtered / TLUT texels, the convert-one feedback */
    const int pointplain = !(om->sample_type || om->en_tlut);
    int unsupp = tl0->format == FORMAT_YUV || (!om->bi_lerp0 && !pointplain);
    if (two) unsupp |= tl1->format == FORMAT_YUV || (!om->bi_lerp1 && !pointplain) || om->convert_one;
    if (om->tex_lod_en)         /* any tile the LOD can select */
        for (int k = 0; k <= r->pc.max_level + 2 && k < 8; k++)
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

/* ---- COPY cycle (texture rectangles in COPY mode) --------------------------------------------------- */

/* The scalar reference (scalar_copy) works in groups of four pixels: the texture coordinates of a group's first pixel are
 * divided / clamped / shifted once, the pixel q of the group reads texel (s + q, t), the tile's mask is applied per texel
 * (no clamp, no mirror), the texel is the raw 16-bit RGBA5551 word (TMEM as is, or the TLUT entry of an 8 / 4-bit colour
 * index) and, with alpha compare, pixels whose low bit is clear are skipped. The vector version below does the same for
 * 16-bit colour buffers and flip != 0 (texture rectangles; the other cases keep the scalar kernel): phase 1 computes the
 * (s, t) of every group of the span, four groups per vector; phase 2 walks the span in windows of eight pixels that start
 * at an even framebuffer pixel (so a window is one vector load / store of the byte-swapped RDRAM), expands the group
 * values to the pixels of the window with a table lookup and gathers the texels. Address arithmetic runs in 16-bit lanes:
 * only the low 11-12 bits of a TMEM address are used and every operation on the way is exact modulo 2^16. */

#define COPY_MAXG 272          /* groups of four pixels per span (a span has at most 1024 pixels) */

/* byte indices (for vqtbl1q_u8 on four int16 group values) and in-group pixel numbers of the lanes of a window whose
 * first lane has the phase ph = (offset of the lane in the span) & 3 */
#define CPT(a, b, c, d, e, f, g, h) { 2 * a, 2 * a + 1, 2 * b, 2 * b + 1, 2 * c, 2 * c + 1, 2 * d, 2 * d + 1, \
                                      2 * e, 2 * e + 1, 2 * f, 2 * f + 1, 2 * g, 2 * g + 1, 2 * h, 2 * h + 1 }
static const uint8_t copy_gtbl[4][16] = {
    CPT(0, 0, 0, 0, 1, 1, 1, 1), CPT(0, 0, 0, 1, 1, 1, 1, 2), CPT(0, 0, 1, 1, 1, 1, 2, 2), CPT(0, 1, 1, 1, 1, 2, 2, 2) };
static const int16_t copy_qtbl[4][8] = {
    { 0, 1, 2, 3, 0, 1, 2, 3 }, { 1, 2, 3, 0, 1, 2, 3, 0 }, { 2, 3, 0, 1, 2, 3, 0, 1 }, { 3, 0, 1, 2, 3, 0, 1, 2 } };
#undef CPT

enum { CK_RAW16, CK_TLUT8, CK_TLUT4, CK_TLUT16, CK_GENERIC };

/* texel (raw RGBA5551 word) of eight pixels from the 16-bit-lane texel coordinates; kind selects the fetch */
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
    default: {                                     /* CK_TLUT4 */
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
    if (xs + n > r->fb_width) n = r->fb_width - xs;      /* pixels past the row end are dropped */
    if (n <= 0) return;
    const uint32_t rs = (r->fb_address >> 1) + (uint32_t)(r->fb_width * y);
    const uint32_t i0 = rs + (uint32_t)xs;
    const int head = (int)(i0 & 1);

    /* phase 1: texture coordinates of every group of four pixels (lanes = groups) */
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
            tc_divide4(persp, vshrq_n_s32(s, 16), vshrq_n_s32(t, 16), vshrq_n_s32(w, 16), &a, &b);
            int32x4_t cs = sign16(clamp17v(a)), ct = sign16(clamp17v(b));
            cs = shs < 11 ? vshlq_s32(cs, S32(-shs)) : sign16(vshlq_s32(cs, S32(16 - shs)));
            ct = sht < 11 ? vshlq_s32(ct, S32(-sht)) : sign16(vshlq_s32(ct, S32(16 - sht)));
            vst1_s16(GS + g0, vmovn_s32(vshrq_n_s32(vsubq_s32(cs, slr), 5)));
            vst1_s16(GT + g0, vmovn_s32(vshrq_n_s32(vsubq_s32(ct, tlr), 5)));
        }
        memset(GS + ng, 0, 8); memset(GT + ng, 0, 8);
    }

    /* phase 2: windows of eight pixels, the first one starting at the even pixel i0 - head */
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
        const int jw = 8 * m - head;                     /* span offset of lane 0 (-1 .. n) */
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
            /* anything else (4 / 8 / 32-bit texels without TLUT): decode to colour, then pack as the scalar kernel does */
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
            if (vminvq_u16(mask) == 0xffff) {            /* every pixel of the window is written: no read-modify-write */
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
    /* the vector path covers texture rectangles into 16-bit colour buffers that lie inside RDRAM; the rest (8-bit colour
     * buffer, flip = 0 triangles, YUV texels without TLUT, spans outside the row or the memory) takes the scalar kernel */
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
