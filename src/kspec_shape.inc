/*
 * mupen64plus-rdp-foir - kspec_shape.inc (included by kneon_comb.c)
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* Only the structure of both cycles is compiled in (per input: zero, 1.0, COMBINED or variable). A variable operand is read through the
 * per-draw table r->sk (foir_spec_select()) as a byte offset into the px_batch: the same load sequence whatever the operand.
 * Constants, noise and COMBINED alpha are stored 9-bit extended; the per-pixel arrays are 0..255 unless the key says `ext`, in which case
 * every variable operand is extended on load (idempotent for the stored-extended ones). A, B, D use k_ext9v, the multiply input k_sext9. */
struct ss {
    int two;
    int ext;
    uint8_t sh[2][2];             /* [cycle][rgb, alpha]: A | B << 2 | M << 4 | D << 6, letters SKL_* */
    uint8_t cm[2][2];             /* constant variable operands (bit per position) */
};

#define SKL(sh, pos) (((sh) >> ((pos) * 2)) & 3)

FOIR_INLINE int16x8_t sk_load(const struct px_batch *b, unsigned off, int o)
{
    return vld1q_s16((const int16_t *)((const char *)b + off) + o);
}

/* isc (FOIR_SK_SPLIT): the operand is a constant and the table entry is its extended value instead of an offset */
FOIR_INLINE int16x8_t sk_opnd(int l, int pos, bool ext, bool isc, const struct px_batch *b, unsigned off, int o, int16x8_t c0)
{
    switch (l) {
    case SKL_Z: return vdupq_n_s16(0);
    case SKL_1: return vdupq_n_s16(0x100);
    case SKL_C: return pos == 2 ? k_sext9(c0) : k_ext9v(c0);
    default: {
        if (isc) return vdupq_n_s16((int16_t)off);
        int16x8_t v = sk_load(b, off, o);
        if (ext) v = pos == 2 ? k_sext9(v) : k_ext9v(v);
        return v;
    }
    }
}

FOIR_INLINE int16x8_t sk_eval(unsigned sh, unsigned cm, bool ext, const struct px_batch *b, int o, int16x8_t c0,
                              unsigned oa, unsigned ob, unsigned om_, unsigned od)
{
    const int la = SKL(sh, 0), lb = SKL(sh, 1), lm = SKL(sh, 2), ld = SKL(sh, 3);
    if (la == SKL_Z && lb == SKL_Z && lm == SKL_Z) {
        switch (ld) {
        case SKL_Z: return vdupq_n_s16(0);
        case SKL_1: return vdupq_n_s16(0x100);
        case SKL_C: return vandq_s16(c0, vdupq_n_s16(0x1ff));
        default:    return vandq_s16(FOIR_SK_SPLIT && (cm & 8) ? vdupq_n_s16((int16_t)od) : sk_load(b, od, o), vdupq_n_s16(0x1ff));
        }
    }
    int16x8_t c = sk_opnd(lm, 2, ext, FOIR_SK_SPLIT && (cm & 4), b, om_, o, c0);
    int32x4_t acc_lo, acc_hi;
    const int32x4_t rnd = vdupq_n_s32(0x80);
    if (ld != SKL_Z) {
        int16x8_t d = sk_opnd(ld, 3, ext, FOIR_SK_SPLIT && (cm & 8), b, od, o, c0);
        acc_lo = vorrq_s32(vshll_n_s16(vget_low_s16(d), 8), rnd);
        acc_hi = vorrq_s32(vshll_high_n_s16(d, 8), rnd);
    } else {
        acc_lo = acc_hi = rnd;
    }
    int32x4_t lo, hi;
    if (la == SKL_Z) {
        int16x8_t bv = sk_opnd(lb, 1, ext, FOIR_SK_SPLIT && (cm & 2), b, ob, o, c0);
        lo = vmlsl_s16(acc_lo, vget_low_s16(bv), vget_low_s16(c));
        hi = vmlsl_high_s16(acc_hi, bv, c);
    } else {
        int16x8_t a = sk_opnd(la, 0, ext, FOIR_SK_SPLIT && (cm & 1), b, oa, o, c0);
        int16x8_t ab = a;
        if (lb != SKL_Z) ab = vsubq_s16(a, sk_opnd(lb, 1, ext, FOIR_SK_SPLIT && (cm & 2), b, ob, o, c0));
        lo = vmlal_s16(acc_lo, vget_low_s16(ab), vget_low_s16(c));
        hi = vmlal_high_s16(acc_hi, ab, c);
    }
    return vandq_s16(vcombine_s16(vshrn_n_s32(lo, 8), vshrn_n_s32(hi, 8)), vdupq_n_s16(0x1ff));
}

FOIR_INLINE void shape_body(struct rdp *r, struct px_batch *b, const struct ss S)
{
    const struct prim_ctx *pc = &r->pc;
    const struct other_modes *om = &r->om;
    const bool two = S.two, ext = S.ext;
    const int rgbsel = om->rgb_dither_sel, asel = om->alpha_dither_sel;
    const bool acmp = two && om->alpha_compare_en, acs = om->alpha_cvg_select, cta = om->cvg_times_alpha;
    const bool sh_rgb = r->sk.sh_rgb, sha = r->sk.sh_a, st_noise = r->sk.noise, st_c0a = r->sk.c0a;
    const int ng = NGROUPS(b->n);
    uint16_t off[2][4][4];                                       /* a local copy: the stores below could alias the table */
    memcpy(off, r->sk.off, sizeof off);

    const uint8x16_t ptab = vld1q_u8((rgbsel == 0 || rgbsel == 2) ? magic_matrix : bayer_matrix);
    const bool need_pv = !(rgbsel == 2 || rgbsel == 3) || asel == 0 || asel == 1;
    const bool need_noise = st_noise || asel == 2;
    const uint32_t seed = pc->seed;
    const int16x8_t zero = vdupq_n_s16(0);

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        const uint8x8_t cvg8 = vld1_u8(&b->cvg[o]);
        const int16x8_t cvg = vreinterpretq_s16_u16(vmovl_u8(cvg8));

        uint8x8_t pv = vdup_n_u8(0);
        if (need_pv) pv = vqtbl1_u8(ptab, vld1_u8(&b->ditidx[o]));
        int16x8_t cdv;
        uint8x8_t ad8;
        int16x8_t noise = zero;
        if (need_noise) {
            uint32x4_t i0 = vld1q_u32(&b->fbidx[o]), i1 = vld1q_u32(&b->fbidx[o + 4]);
            uint32x4_t n0 = vandq_u32(k_prand4(i0, seed, 1), vdupq_n_u32(7));
            uint32x4_t n1 = vandq_u32(k_prand4(i1, seed, 1), vdupq_n_u32(7));
            noise = vorrq_s16(vshlq_n_s16(vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(n0), vmovn_u32(n1))), 6), vdupq_n_s16(0x20));
            if (st_noise) vst1q_s16(&b->noise_s[o], k_ext9v(noise));
        }
        if (rgbsel == 3) cdv = vdupq_n_s16(7);
        else if (rgbsel == 2) {
            uint32x4_t i0 = vld1q_u32(&b->fbidx[o]), i1 = vld1q_u32(&b->fbidx[o + 4]);
            uint32x4_t c0 = vandq_u32(k_prand4(i0, seed, 2), vdupq_n_u32(0x7fff));
            uint32x4_t c1 = vandq_u32(k_prand4(i1, seed, 2), vdupq_n_u32(0x7fff));
            cdv = vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(c0), vmovn_u32(c1)));
        } else cdv = vreinterpretq_s16_u16(vmovl_u8(pv));
        switch (asel) {
        case 0:  ad8 = pv; break;
        case 1:  ad8 = vand_u8(vmvn_u8(pv), vdup_n_u8(7)); break;
        case 2:  ad8 = vmovn_u16(vreinterpretq_u16_s16(vandq_s16(vshrq_n_s16(noise, 6), vdupq_n_s16(7)))); break;
        default: ad8 = vdup_n_u8(0); break;
        }
        const int16x8_t adith = vreinterpretq_s16_u16(vmovl_u8(ad8));
        vst1q_s16(&b->cdith[o], cdv);
        vst1_s8(&b->adith[o], vreinterpret_s8_u8(ad8));

        /* stored: the operand table can point at it */
        int16x8_t shade_a = zero;
        if (sha) {
            int16x8_t sh[4];
            if (vminv_u8(cvg8) == 8) {
                if (sh_rgb) { sh[0] = shade_chan_full(&b->cr[o]); sh[1] = shade_chan_full(&b->cg[o]); sh[2] = shade_chan_full(&b->cb[o]); }
                sh[3] = shade_chan_full(&b->ca[o]);
            } else {
                uint16x8_t ox = vmovl_u8(vld1_u8(&b->offx[o])), oy = vmovl_u8(vld1_u8(&b->offy[o]));
                int32x4_t oxl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(ox)));
                int32x4_t oxh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(ox)));
                int32x4_t oyl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(oy)));
                int32x4_t oyh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(oy)));
                if (sh_rgb) {
                    sh[0] = shade_chan_corr(&b->cr[o], oxl, oxh, oyl, oyh, pc->cdr, pc->drdy);
                    sh[1] = shade_chan_corr(&b->cg[o], oxl, oxh, oyl, oyh, pc->cdg, pc->dgdy);
                    sh[2] = shade_chan_corr(&b->cb[o], oxl, oxh, oyl, oyh, pc->cdb, pc->dbdy);
                }
                sh[3] = shade_chan_corr(&b->ca[o], oxl, oxh, oyl, oyh, pc->cda, pc->dady);
            }
            if (sh_rgb) for (int c = 0; c < 3; c++) vst1q_s16(&b->shade[c][o], sh[c]);
            vst1q_s16(&b->shade[3][o], sh[3]);
            shade_a = sh[3];
        }

        int16x8_t acalpha = zero;
        int16x8_t c0r[4] = { zero, zero, zero, zero };
        if (two) {
            for (int c = 0; c < 3; c++)
                c0r[c] = sk_eval(S.sh[0][0], S.cm[0][0], ext, b, o, zero, off[0][0][c], off[0][1][c], off[0][2][c], off[0][3][c]);
            c0r[3] = sk_eval(S.sh[0][1], S.cm[0][1], ext, b, o, zero, off[0][0][3], off[0][1][3], off[0][2][3], off[0][3][3]);
            if (st_c0a) vst1q_s16(&b->c0a[o], k_sext9(c0r[3]));
            if (acmp) {
                int16x8_t pre = k_clamp9v(c0r[3]);
                pre = vbslq_s16(vceqq_s16(pre, vdupq_n_s16(0xff)), vdupq_n_s16(0x100), pre);
                if (!acs) {
                    pre = vaddq_s16(pre, adith);
                    pre = vbslq_s16(vtstq_s16(pre, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), pre);
                } else {
                    if (cta) pre = vshrq_n_s16(vaddq_s16(vmulq_s16(pre, cvg), vdupq_n_s16(4)), 3);
                    else pre = vshlq_n_s16(cvg, 5);
                    pre = vminq_s16(pre, vdupq_n_s16(0xff));
                }
                acalpha = pre;
            }
        }
        int16x8_t cf[4];
        for (int c = 0; c < 3; c++)
            cf[c] = sk_eval(S.sh[1][0], S.cm[1][0], ext, b, o, c0r[c], off[1][0][c], off[1][1][c], off[1][2][c], off[1][3][c]);
        cf[3] = sk_eval(S.sh[1][1], S.cm[1][1], ext, b, o, c0r[3], off[1][0][3], off[1][1][3], off[1][2][3], off[1][3][3]);

        int16x8_t pa = k_clamp9v(cf[3]);
        pa = vbslq_s16(vceqq_s16(pa, vdupq_n_s16(0xff)), vdupq_n_s16(0x100), pa);
        int16x8_t newcvg = cvg, temp = zero;
        if (cta) {
            temp = vshrq_n_s16(vaddq_s16(vmulq_s16(pa, cvg), vdupq_n_s16(4)), 3);
            newcvg = vandq_s16(vshrq_n_s16(temp, 5), vdupq_n_s16(0xf));
            vst1_u8(&b->cvg[o], vmovn_u16(vreinterpretq_u16_s16(newcvg)));
        }
        if (!acs) {
            pa = vaddq_s16(pa, adith);
            pa = vbslq_s16(vtstq_s16(pa, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), pa);
        } else {
            pa = cta ? temp : vshlq_n_s16(newcvg, 5);
            pa = vminq_s16(pa, vdupq_n_s16(0xff));
        }
        vst1q_s16(&b->pixel[CH_R][o], k_clamp9v(cf[0]));
        vst1q_s16(&b->pixel[CH_G][o], k_clamp9v(cf[1]));
        vst1q_s16(&b->pixel[CH_B][o], k_clamp9v(cf[2]));
        vst1q_s16(&b->pixel[CH_A][o], pa);
        vst1q_s16(&b->acalpha[o], two ? acalpha : pa);

        int16x8_t sha_v = vaddq_s16(shade_a, adith);
        sha_v = vbslq_s16(vtstq_s16(sha_v, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), sha_v);
        vst1q_s16(&b->shade_alpha[o], sha_v);
    }
}

/* the run-time options are loop invariant: keep one loop (GCC would unswitch it into up to a dozen copies) */
#if defined(__GNUC__) && !defined(__clang__)
#define SK_NOUNSWITCH __attribute__((optimize("no-unswitch-loops")))
#else
#define SK_NOUNSWITCH
#endif

#ifndef FOIR_SHAPE_DEF
#define FOIR_SHAPE_DEF "spec_shape.def"      /* -DFOIR_SHAPE_DEF=\"other.def\": experiments with other kernel lists */
#endif

#if FOIR_SK_SPLIT
/* SHAPEC: also which variable operands are constants (c0r, c0a, c1r, c1a: bit per position A, B, M, D) */
#define SHAPE_DEF_FILE "spec_shape_cm.def"
#define SHAPEC(two, r0, a0, r1, a1, ext, k0r, k0a, k1r, k1a) \
    SHAPE_NAMED(shp_##two##_##r0##_##a0##_##r1##_##a1##_##ext##_##k0r##k0a##k1r##k1a, two, r0, a0, r1, a1, ext, k0r, k0a, k1r, k1a)
#define SHAPE_NAMED(nm, two, r0, a0, r1, a1, ext, k0r, k0a, k1r, k1a) \
    SK_NOUNSWITCH static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { shape_body(r, b, (const struct ss){ two, ext, { { r0, a0 }, { r1, a1 } }, { { k0r, k0a }, { k1r, k1a } } }); }
#include SHAPE_DEF_FILE
#undef SHAPEC
#undef SHAPE_NAMED

#define SHAPEC(two, r0, a0, r1, a1, ext, k0r, k0a, k1r, k1a) \
    SHAPE_NAMED(shp_##two##_##r0##_##a0##_##r1##_##a1##_##ext##_##k0r##k0a##k1r##k1a, two, r0, a0, r1, a1, ext, k0r, k0a, k1r, k1a)
#define SHAPE_NAMED(nm, two_, r0, a0, r1, a1, ext_, k0r, k0a, k1r, k1a) \
    { .k.shape = { .two = two_, .ext = ext_, .sh = { { r0, a0 }, { r1, a1 } }, .cm = { { k0r, k0a }, { k1r, k1a } } }, .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_shape[] = {
#include SHAPE_DEF_FILE
};
#undef SHAPEC
#undef SHAPE_NAMED
#else
#define SHAPE(two, r0, a0, r1, a1, ext) SHAPE_NAMED(shp_##two##_##r0##_##a0##_##r1##_##a1##_##ext, two, r0, a0, r1, a1, ext)
#define SHAPE_NAMED(nm, two, r0, a0, r1, a1, ext) \
    SK_NOUNSWITCH static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { shape_body(r, b, (const struct ss){ two, ext, { { r0, a0 }, { r1, a1 } }, { { 0, 0 }, { 0, 0 } } }); }
#include FOIR_SHAPE_DEF
#undef SHAPE
#undef SHAPE_NAMED

#define SHAPE(two, r0, a0, r1, a1, ext) SHAPE_NAMED(shp_##two##_##r0##_##a0##_##r1##_##a1##_##ext, two, r0, a0, r1, a1, ext)
#define SHAPE_NAMED(nm, two_, r0, a0, r1, a1, ext_) \
    { .k.shape = { .two = two_, .ext = ext_, .sh = { { r0, a0 }, { r1, a1 } } }, .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_shape[] = {
#include FOIR_SHAPE_DEF
};
#undef SHAPE
#undef SHAPE_NAMED
#endif
const int foir_spec_n_shape = (int)(sizeof(foir_spec_ents_shape) / sizeof(foir_spec_ents_shape[0]));
