/*
 * mupen64plus-rdp-foir - kspec_comb.inc (included by kneon_comb.c)
 * Specialised colour combiner variants. comb_body() is the generic kernel's algorithm with the combiner
 * selectors, the dither / alpha options and the shade needs as compile-time constants: operands are
 * resolved to a load, a splatted constant or a register at build time, the equation shapes are chosen at
 * build time, shade and the cycle-0 result never touch memory and there is no per-batch table setup.
 * The variants are listed in spec_comb.def.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "combreg.h"
#include "spec.h"

struct cs {
    int two;
    uint8_t sel[16];
    int rgbd, ad, acmp, acs, cta, shrgb, sha, wide;
};

/* splatted per-draw constants (the generic kernel splats the used ones into per-batch arrays) */
struct cconst {
    int16x8_t prim[4], env[4], keyc[3], keys[3], k4, k5, plodf;
};

FOIR_INLINE int16x8_t cs_opnd(int idx, int tsel, const struct px_batch *b, int o, const int16x8_t sh[4],
                              const int16x8_t c0[4], int16x8_t noise, const struct cconst *K)
{
    if (idx >= REG_COMB && idx < REG_COMB + 4) return c0[idx - REG_COMB];
    if (idx >= REG_TEX0 && idx < REG_TEX0 + 4) return vld1q_s16((tsel ? b->tex1[idx - REG_TEX0] : b->tex0[idx - REG_TEX0]) + o);
    if (idx >= REG_TEX1 && idx < REG_TEX1 + 4) return vld1q_s16((tsel ? b->tex0n[idx - REG_TEX1] : b->tex1[idx - REG_TEX1]) + o);
    if (idx >= REG_PRIM && idx < REG_PRIM + 4) return K->prim[idx - REG_PRIM];
    if (idx >= REG_SHADE && idx < REG_SHADE + 4) return sh[idx - REG_SHADE];
    if (idx >= REG_ENV && idx < REG_ENV + 4) return K->env[idx - REG_ENV];
    if (idx >= REG_KEYC && idx < REG_KEYC + 3) return K->keyc[idx - REG_KEYC];
    if (idx >= REG_KEYS && idx < REG_KEYS + 3) return K->keys[idx - REG_KEYS];
    switch (idx) {
    case REG_ONE:   return vdupq_n_s16(0x100);
    case REG_NOISE: return noise;
    case REG_K4:    return K->k4;
    case REG_K5:    return K->k5;
    case REG_PLODF: return K->plodf;
    case REG_LODF:  return vld1q_s16(b->lodfrac + o);
    default:        return vdupq_n_s16(0);
    }
}

/* one equation (((a - b) * c + (d << 8) + 0x80) >> 8) & 0x1ff, classified at build time like cop_make() */
FOIR_INLINE int16x8_t cs_eval(int ia, int ib, int im, int id, bool tw, int tsel, const struct px_batch *b, int o,
                              const int16x8_t sh[4], const int16x8_t c0[4], int16x8_t noise, const struct cconst *K)
{
    const bool ea = reg_wide(ia, tw), eb = reg_wide(ib, tw), ec = reg_gt255(im, tw), ed = reg_wide(id, tw);
    int mode;
    if (im == REG_ZERO || ia == ib) mode = M_D;
    else if (ib == REG_ZERO && id == REG_ZERO) mode = M_AC;
    else if (id == REG_ZERO) mode = M_ABC;
    else if (ib == REG_ZERO) mode = M_ACD;
    else mode = M_FULL;

    if (mode == M_D)
        return vandq_s16(cs_opnd(id, tsel, b, o, sh, c0, noise, K), vdupq_n_s16(0x1ff));
    int16x8_t a = cs_opnd(ia, tsel, b, o, sh, c0, noise, K);
    int16x8_t c = cs_opnd(im, tsel, b, o, sh, c0, noise, K);
    if (ea) a = k_ext9v(a);
    if (ec) c = k_sext9(c);
    int16x8_t ab = a;
    if (mode == M_ABC || mode == M_FULL) {
        int16x8_t bv = cs_opnd(ib, tsel, b, o, sh, c0, noise, K);
        if (eb) bv = k_ext9v(bv);
        ab = vsubq_s16(a, bv);
    }
    int32x4_t acc_lo, acc_hi;
    const int32x4_t rnd = vdupq_n_s32(0x80);
    if (mode == M_ACD || mode == M_FULL) {
        int16x8_t d = cs_opnd(id, tsel, b, o, sh, c0, noise, K);
        if (ed) d = k_ext9v(d);
        acc_lo = vorrq_s32(vshll_n_s16(vget_low_s16(d), 8), rnd);
        acc_hi = vorrq_s32(vshll_high_n_s16(d, 8), rnd);
    } else {
        acc_lo = acc_hi = rnd;
    }
    int32x4_t lo = vmlal_s16(acc_lo, vget_low_s16(ab), vget_low_s16(c));
    int32x4_t hi = vmlal_high_s16(acc_hi, ab, c);
    return vandq_s16(vcombine_s16(vshrn_n_s32(lo, 8), vshrn_n_s32(hi, 8)), vdupq_n_s16(0x1ff));
}

FOIR_INLINE int16x8_t cs_rgb(const struct cs *S, int cy, int ch, int tsel, bool tw, const struct px_batch *b, int o,
                             const int16x8_t sh[4], const int16x8_t c0[4], int16x8_t noise, const struct cconst *K)
{
    return cs_eval(comb_reg_rgb(0, S->sel[cy * 8 + 0], ch), comb_reg_rgb(1, S->sel[cy * 8 + 1], ch),
                   comb_reg_rgb(2, S->sel[cy * 8 + 2], ch), comb_reg_rgb(3, S->sel[cy * 8 + 3], ch),
                   tw, tsel, b, o, sh, c0, noise, K);
}

FOIR_INLINE int16x8_t cs_alpha(const struct cs *S, int cy, int tsel, bool tw, const struct px_batch *b, int o,
                               const int16x8_t sh[4], const int16x8_t c0[4], int16x8_t noise, const struct cconst *K)
{
    return cs_eval(comb_reg_alpha(0, S->sel[cy * 8 + 4]), comb_reg_alpha(1, S->sel[cy * 8 + 5]),
                   comb_reg_alpha(2, S->sel[cy * 8 + 6]), comb_reg_alpha(3, S->sel[cy * 8 + 7]),
                   tw, tsel, b, o, sh, c0, noise, K);
}

FOIR_INLINE void comb_body(struct rdp *r, struct px_batch *b, const struct cs S)
{
    const struct prim_ctx *pc = &r->pc;
    const bool two = S.two, tw = S.wide;
    const int rgbsel = S.rgbd, asel = S.ad;
    const int ng = NGROUPS(b->n);

    struct cconst K;
    K.prim[0] = vdupq_n_s16((int16_t)r->prim_color.r); K.prim[1] = vdupq_n_s16((int16_t)r->prim_color.g);
    K.prim[2] = vdupq_n_s16((int16_t)r->prim_color.b); K.prim[3] = vdupq_n_s16((int16_t)r->prim_color.a);
    K.env[0] = vdupq_n_s16((int16_t)r->env_color.r); K.env[1] = vdupq_n_s16((int16_t)r->env_color.g);
    K.env[2] = vdupq_n_s16((int16_t)r->env_color.b); K.env[3] = vdupq_n_s16((int16_t)r->env_color.a);
    K.keyc[0] = vdupq_n_s16((int16_t)r->key_center.r); K.keyc[1] = vdupq_n_s16((int16_t)r->key_center.g);
    K.keyc[2] = vdupq_n_s16((int16_t)r->key_center.b);
    K.keys[0] = vdupq_n_s16((int16_t)r->key_scale.r); K.keys[1] = vdupq_n_s16((int16_t)r->key_scale.g);
    K.keys[2] = vdupq_n_s16((int16_t)r->key_scale.b);
    K.k4 = vdupq_n_s16((int16_t)r->k4); K.k5 = vdupq_n_s16((int16_t)r->k5);
    K.plodf = vdupq_n_s16((int16_t)r->primitive_lod_frac);

    const uint8x16_t ptab = vld1q_u8((rgbsel == 0 || rgbsel == 2) ? magic_matrix : bayer_matrix);
    const bool need_pv = !(rgbsel == 2 || rgbsel == 3) || asel == 0 || asel == 1;
    const bool need_noise = asel == 2 || (two && (S.sel[0] & 0xf) == 7) || (S.sel[8] & 0xf) == 7;
    const uint32_t seed = pc->seed;
    const int16x8_t zero4[4] = { vdupq_n_s16(0), vdupq_n_s16(0), vdupq_n_s16(0), vdupq_n_s16(0) };

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        const uint8x8_t cvg8 = vld1_u8(&b->cvg[o]);
        const int16x8_t cvg = vreinterpretq_s16_u16(vmovl_u8(cvg8));

        /* ---- noise / dither ---- */
        uint8x8_t pv = vdup_n_u8(0);
        if (need_pv) pv = vqtbl1_u8(ptab, vld1_u8(&b->ditidx[o]));
        int16x8_t cdv;
        uint8x8_t ad8;
        int16x8_t noise = vdupq_n_s16(0);
        if (need_noise) {
            uint32x4_t i0 = vld1q_u32(&b->fbidx[o]), i1 = vld1q_u32(&b->fbidx[o + 4]);
            uint32x4_t n0 = vandq_u32(k_prand4(i0, seed, 1), vdupq_n_u32(7));
            uint32x4_t n1 = vandq_u32(k_prand4(i1, seed, 1), vdupq_n_u32(7));
            noise = vorrq_s16(vshlq_n_s16(vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(n0), vmovn_u32(n1))), 6), vdupq_n_s16(0x20));
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

        /* ---- shade ---- */
        int16x8_t sh[4] = { vdupq_n_s16(0), vdupq_n_s16(0), vdupq_n_s16(0), vdupq_n_s16(0) };
        if (S.sha) {
            if (vminv_u8(cvg8) == 8) {
                if (S.shrgb) { sh[0] = shade_chan_full(&b->cr[o]); sh[1] = shade_chan_full(&b->cg[o]); sh[2] = shade_chan_full(&b->cb[o]); }
                sh[3] = shade_chan_full(&b->ca[o]);
            } else {
                uint16x8_t ox = vmovl_u8(vld1_u8(&b->offx[o])), oy = vmovl_u8(vld1_u8(&b->offy[o]));
                int32x4_t oxl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(ox)));
                int32x4_t oxh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(ox)));
                int32x4_t oyl = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(oy)));
                int32x4_t oyh = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(oy)));
                if (S.shrgb) {
                    sh[0] = shade_chan_corr(&b->cr[o], oxl, oxh, oyl, oyh, pc->cdr, pc->drdy);
                    sh[1] = shade_chan_corr(&b->cg[o], oxl, oxh, oyl, oyh, pc->cdg, pc->dgdy);
                    sh[2] = shade_chan_corr(&b->cb[o], oxl, oxh, oyl, oyh, pc->cdb, pc->dbdy);
                }
                sh[3] = shade_chan_corr(&b->ca[o], oxl, oxh, oyl, oyh, pc->cda, pc->dady);
            }
        }
        const int16x8_t shade_a = sh[3];

        /* ---- cycles ---- */
        int16x8_t acalpha = vdupq_n_s16(0);
        int16x8_t c0r[4] = { vdupq_n_s16(0), vdupq_n_s16(0), vdupq_n_s16(0), vdupq_n_s16(0) };
        if (two) {
            for (int c = 0; c < 3; c++) c0r[c] = cs_rgb(&S, 0, c, 0, tw, b, o, sh, zero4, noise, &K);
            c0r[3] = cs_alpha(&S, 0, 0, tw, b, o, sh, zero4, noise, &K);
            if (S.acmp) {
                int16x8_t pre = k_clamp9v(c0r[3]);
                pre = vbslq_s16(vceqq_s16(pre, vdupq_n_s16(0xff)), vdupq_n_s16(0x100), pre);
                if (!S.acs) {
                    pre = vaddq_s16(pre, adith);
                    pre = vbslq_s16(vtstq_s16(pre, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), pre);
                } else {
                    if (S.cta) pre = vshrq_n_s16(vaddq_s16(vmulq_s16(pre, cvg), vdupq_n_s16(4)), 3);
                    else pre = vshlq_n_s16(cvg, 5);
                    pre = vminq_s16(pre, vdupq_n_s16(0xff));
                }
                acalpha = pre;
            }
        }
        int16x8_t cf[4];
        const int16x8_t *cprev = two ? c0r : zero4;
        for (int c = 0; c < 3; c++) cf[c] = cs_rgb(&S, 1, c, two, tw, b, o, sh, cprev, noise, &K);
        cf[3] = cs_alpha(&S, 1, two, tw, b, o, sh, cprev, noise, &K);

        /* ---- finish ---- */
        int16x8_t pa = k_clamp9v(cf[3]);
        pa = vbslq_s16(vceqq_s16(pa, vdupq_n_s16(0xff)), vdupq_n_s16(0x100), pa);
        int16x8_t newcvg = cvg, temp = vdupq_n_s16(0);
        if (S.cta) {
            temp = vshrq_n_s16(vaddq_s16(vmulq_s16(pa, cvg), vdupq_n_s16(4)), 3);
            newcvg = vandq_s16(vshrq_n_s16(temp, 5), vdupq_n_s16(0xf));
            vst1_u8(&b->cvg[o], vmovn_u16(vreinterpretq_u16_s16(newcvg)));
        }
        if (!S.acs) {
            pa = vaddq_s16(pa, adith);
            pa = vbslq_s16(vtstq_s16(pa, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), pa);
        } else {
            pa = S.cta ? temp : vshlq_n_s16(newcvg, 5);
            pa = vminq_s16(pa, vdupq_n_s16(0xff));
        }
        vst1q_s16(&b->pixel[CH_R][o], k_clamp9v(cf[0]));
        vst1q_s16(&b->pixel[CH_G][o], k_clamp9v(cf[1]));
        vst1q_s16(&b->pixel[CH_B][o], k_clamp9v(cf[2]));
        vst1q_s16(&b->pixel[CH_A][o], pa);
        vst1q_s16(&b->acalpha[o], two ? acalpha : pa);

        int16x8_t sha = vaddq_s16(shade_a, adith);
        sha = vbslq_s16(vtstq_s16(sha, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), sha);
        vst1q_s16(&b->shade_alpha[o], sha);
    }
}

/* ---- the variants -------------------------------------------------------------------------------------- */

/* selector fields of SET_COMBINE (w0 = low 24 bits of the first word, w1 = second word); the cycle-0 fields of a
 * one-cycle state are zero (as in the key) */
#define CMB_SELS(two, w0, w1) { \
    (two) ? ((w0) >> 20) & 0xf : 0, (two) ? ((w1) >> 28) & 0xf : 0, (two) ? ((w0) >> 15) & 0x1f : 0, (two) ? ((w1) >> 15) & 7 : 0, \
    (two) ? ((w0) >> 12) & 7 : 0, (two) ? ((w1) >> 12) & 7 : 0, (two) ? ((w0) >> 9) & 7 : 0, (two) ? ((w1) >> 9) & 7 : 0, \
    ((w0) >> 5) & 0xf, ((w1) >> 24) & 0xf, (w0) & 0x1f, ((w1) >> 6) & 7, \
    ((w1) >> 21) & 7, ((w1) >> 3) & 7, ((w1) >> 18) & 7, (w1) & 7 }

/* COMB: every option literal. COMBX: the "any option" form of the same selectors: dither, alpha compare, coverage
 * options and the blender's shade alpha need are read from the render state (cheap: loop invariants and well predicted
 * branches), so one variant serves every state with these selectors. A state looks for COMB first, then COMBX. */
#define COMB(two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide) \
    COMB_NAMED(comb_##two##_##w0##_##w1##_##rgbd##ad##acmp##acs##cta##shrgb##sha##wide, two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide)
#define COMB_NAMED(nm, two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide) \
    static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { comb_body(r, b, (const struct cs){ two, CMB_SELS(two, w0, w1), rgbd, ad, acmp, acs, cta, shrgb, sha, wide }); }
#define COMBX(two, w0, w1, shrgb, wide) COMBX_NAMED(combx_##two##_##w0##_##w1##_##shrgb##wide, two, w0, w1, shrgb, wide)
#define COMBX_NAMED(nm, two, w0, w1, shrgb, wide) \
    static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { comb_body(r, b, (const struct cs){ two, CMB_SELS(two, w0, w1), r->om.rgb_dither_sel, r->om.alpha_dither_sel, \
                                         (two) && r->om.alpha_compare_en, r->om.alpha_cvg_select, r->om.cvg_times_alpha, \
                                         shrgb, r->pf.shade_a || r->pf.shade_rgb, wide }); }
#include "spec_comb.def"
#undef COMB
#undef COMB_NAMED
#undef COMBX
#undef COMBX_NAMED

#define COMB(two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide) \
    COMB_NAMED(comb_##two##_##w0##_##w1##_##rgbd##ad##acmp##acs##cta##shrgb##sha##wide, two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide)
#define COMB_NAMED(nm, two_, w0, w1, rgbd_, ad_, acmp_, acs_, cta_, shrgb_, sha_, wide_) \
    { .k.comb = { .sel = CMB_SELS(two_, w0, w1), .two = two_, .rgbd = rgbd_, .ad = ad_, .acmp = acmp_, .acs = acs_, \
                  .cta = cta_, .shrgb = shrgb_, .sha = sha_, .wide = wide_ }, .fn = spec_##nm, .name = #nm },
#define COMBX(two, w0, w1, shrgb, wide) COMBX_NAMED(combx_##two##_##w0##_##w1##_##shrgb##wide, two, w0, w1, shrgb, wide)
#define COMBX_NAMED(nm, two_, w0, w1, shrgb_, wide_) \
    { .k.comb = { .sel = CMB_SELS(two_, w0, w1), .two = two_, .shrgb = shrgb_, .wide = wide_, .any = 1 }, .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_comb[] = {
#include "spec_comb.def"
};
const int foir_spec_n_comb = (int)(sizeof(foir_spec_ents_comb) / sizeof(foir_spec_ents_comb[0]));
#undef COMB
#undef COMB_NAMED
#undef COMBX
#undef COMBX_NAMED
