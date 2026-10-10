/*
 * mupen64plus-rdp-foir - kneon_comb.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kneon.h"
#include "spec.h"

static const uint8_t bayer_matrix[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };
static const uint8_t magic_matrix[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };

enum { M_D = 0, M_AC, M_ABC, M_ACD, M_FULL };

struct cop {
    const int16_t *pa, *pb, *pc, *pd;
    uint8_t mode, ea, eb, ec, ed;
};

/* value can be outside 0..383 */
FOIR_INLINE bool reg_wide(int idx, bool tex_wide)
{
    if (idx >= REG_TEX0 && idx < REG_TEX1 + 4) return tex_wide;
    return idx == REG_NOISE || idx == REG_LODF || idx == REG_K4 || idx == REG_K5 || (idx >= REG_COMB && idx < REG_COMB + 4);
}
/* not in 0..255: the C input is sign-extended from 9 bits */
FOIR_INLINE bool reg_gt255(int idx, bool tex_wide) { return reg_wide(idx, tex_wide) || idx == REG_ONE; }

static void cop_make(struct cop *op, const int16_t *const *src, bool tw, int ia, int ib, int im, int id)
{
    op->pd = src[id]; op->ed = reg_wide(id, tw);
    op->pa = src[ia]; op->pb = src[ib]; op->pc = src[im];
    op->ea = reg_wide(ia, tw); op->eb = reg_wide(ib, tw); op->ec = reg_gt255(im, tw);
    if (im == REG_ZERO || ia == ib) op->mode = M_D;
    else if (ib == REG_ZERO && id == REG_ZERO) op->mode = M_AC;
    else if (id == REG_ZERO) op->mode = M_ABC;
    else if (ib == REG_ZERO) op->mode = M_ACD;
    else op->mode = M_FULL;
}

FOIR_INLINE int16x8_t cop_eval(const struct cop *op, int o)
{
    if (op->mode == M_D)                 /* (d << 8 + 0x80) >> 8 in 9 bits is just d & 0x1ff */
        return vandq_s16(vld1q_s16(op->pd + o), vdupq_n_s16(0x1ff));
    int16x8_t a = vld1q_s16(op->pa + o);
    int16x8_t c = vld1q_s16(op->pc + o);
    if (op->ea) a = k_ext9v(a);
    if (op->ec) c = k_sext9(c);
    int16x8_t ab = a;
    if (op->mode == M_ABC || op->mode == M_FULL) {
        int16x8_t bv = vld1q_s16(op->pb + o);
        if (op->eb) bv = k_ext9v(bv);
        ab = vsubq_s16(a, bv);
    }
    int32x4_t acc_lo, acc_hi;
    const int32x4_t rnd = vdupq_n_s32(0x80);
    if (op->mode == M_ACD || op->mode == M_FULL) {
        int16x8_t d = vld1q_s16(op->pd + o);
        if (op->ed) d = k_ext9v(d);
        acc_lo = vorrq_s32(vshll_n_s16(vget_low_s16(d), 8), rnd);
        acc_hi = vorrq_s32(vshll_high_n_s16(d, 8), rnd);
    } else {
        acc_lo = acc_hi = rnd;
    }
    int32x4_t lo = vmlal_s16(acc_lo, vget_low_s16(ab), vget_low_s16(c));
    int32x4_t hi = vmlal_high_s16(acc_hi, ab, c);
    return vandq_s16(vcombine_s16(vshrn_n_s32(lo, 8), vshrn_n_s32(hi, 8)), vdupq_n_s16(0x1ff));
}

FOIR_INLINE int16x8_t shade_chan_full(const int32_t *c)
{
    int32x4_t lo = vld1q_s32(c), hi = vld1q_s32(c + 4);
    return k_clamp9v(vcombine_s16(vshrn_n_s32(lo, 16), vshrn_n_s32(hi, 16)));
}

FOIR_INLINE int16x8_t shade_chan_corr(const int32_t *c, int32x4_t offx_lo, int32x4_t offx_hi,
                                      int32x4_t offy_lo, int32x4_t offy_hi, int32_t cdx, int32_t cdy)
{
    int32x4_t lo = vshlq_n_s32(vshrq_n_s32(vld1q_s32(c), 14), 2);
    int32x4_t hi = vshlq_n_s32(vshrq_n_s32(vld1q_s32(c + 4), 14), 2);
    lo = vmlaq_n_s32(vmlaq_n_s32(lo, offx_lo, cdx), offy_lo, cdy);
    hi = vmlaq_n_s32(vmlaq_n_s32(hi, offx_hi, cdx), offy_hi, cdy);
    return k_clamp9v(vcombine_s16(vmovn_s32(vshrq_n_s32(lo, 4)), vmovn_s32(vshrq_n_s32(hi, 4))));
}

void neon_combine(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    if (om->key_en) {
        NEON_FALLBACK(1);
        foir_kernels_scalar.combine(r, b);
        return;
    }
    NEON_HANDLED(1);
    const struct prim_ctx *pc = &r->pc;
    const struct combiner *cb = &r->comb;
    const int two = om->cycle_type == CYCLE_TYPE_2;
    const int sh_rgb = r->pf.shade_rgb, sh_a = r->pf.shade_a || r->pf.shade_rgb;
    const struct tile *tl0 = &r->tile[r->pc.tilenum], *tl1 = &r->tile[(r->pc.tilenum + 1) & 7];
    /* texel values are 0..255 unless YUV conversion is involved */
    bool tex_wide = tl0->format == FORMAT_YUV || !om->bi_lerp0 ||
                    (two && (tl1->format == FORMAT_YUV || !om->bi_lerp1 || om->convert_one));
    if (!tex_wide && om->tex_lod_en)   /* LOD can pick any tile of the mip chain */
        for (int t = 0; t < 8; t++)
            if (r->tile[t].format == FORMAT_YUV) tex_wide = true;

    int16_t shade[4][FOIR_LANES] ALIGN16;
    int16_t comb0[4][FOIR_LANES] ALIGN16;
    int16_t noise_s[FOIR_LANES] ALIGN16;
    int16_t cbuf[REG_COUNT][FOIR_LANES] ALIGN16;
    const int16_t *src0[REG_COUNT], *src1[REG_COUNT];
    uint8_t used[REG_COUNT];
    memset(used, 0, sizeof(used));
    for (int cy = two ? 0 : 1; cy < 2; cy++) {
        for (int ch = 0; ch < 3; ch++)
            for (int i = 0; i < 4; i++) used[cb->rgb[cy][i][ch]] = 1;
        for (int i = 0; i < 4; i++) used[cb->alpha[cy][i]] = 1;
    }
    used[REG_ZERO] = 1;
    memset(comb0, 0, sizeof(comb0));

    int16_t cv[REG_COUNT];
    memset(cv, 0, sizeof(cv));
    cv[REG_ONE] = 0x100;
    cv[REG_K4] = (int16_t)r->k4; cv[REG_K5] = (int16_t)r->k5;
    cv[REG_PLODF] = (int16_t)r->primitive_lod_frac;
    cv[REG_KEYC + 0] = (int16_t)r->key_center.r; cv[REG_KEYC + 1] = (int16_t)r->key_center.g; cv[REG_KEYC + 2] = (int16_t)r->key_center.b;
    cv[REG_KEYS + 0] = (int16_t)r->key_scale.r;  cv[REG_KEYS + 1] = (int16_t)r->key_scale.g;  cv[REG_KEYS + 2] = (int16_t)r->key_scale.b;
    cv[REG_PRIM + 0] = (int16_t)r->prim_color.r; cv[REG_PRIM + 1] = (int16_t)r->prim_color.g;
    cv[REG_PRIM + 2] = (int16_t)r->prim_color.b; cv[REG_PRIM + 3] = (int16_t)r->prim_color.a;
    cv[REG_ENV + 0] = (int16_t)r->env_color.r; cv[REG_ENV + 1] = (int16_t)r->env_color.g;
    cv[REG_ENV + 2] = (int16_t)r->env_color.b; cv[REG_ENV + 3] = (int16_t)r->env_color.a;
    const int ng = NGROUPS(b->n);
    for (int i = 0; i < REG_COUNT; i++) {
        src0[i] = cbuf[i];
        if (used[i] && !(i >= REG_COMB && i < REG_COMB + 4) && !(i >= REG_TEX0 && i < REG_TEX1 + 4) &&
            !(i >= REG_SHADE && i < REG_SHADE + 4) && i != REG_NOISE && i != REG_LODF) {
            int16x8_t v = vdupq_n_s16(cv[i]);
            for (int g = 0; g < ng; g++) vst1q_s16(&cbuf[i][g * 8], v);
        }
    }
    for (int c = 0; c < 4; c++) {
        src0[REG_TEX0 + c] = b->tex0[c]; src0[REG_TEX1 + c] = b->tex1[c];
        src0[REG_SHADE + c] = shade[c]; src0[REG_COMB + c] = comb0[c];
    }
    src0[REG_NOISE] = noise_s; src0[REG_LODF] = b->lodfrac;
    memcpy(src1, src0, sizeof(src0));
    for (int c = 0; c < 4; c++) { src1[REG_TEX0 + c] = b->tex1[c]; src1[REG_TEX1 + c] = b->tex0n[c]; }

    struct cop ops[2][4];
    for (int cy = two ? 0 : 1; cy < 2; cy++) {
        const int16_t *const *s = (two && cy == 1) ? src1 : src0;
        for (int ch = 0; ch < 3; ch++)
            cop_make(&ops[cy][ch], s, tex_wide, cb->rgb[cy][0][ch], cb->rgb[cy][1][ch], cb->rgb[cy][2][ch], cb->rgb[cy][3][ch]);
        cop_make(&ops[cy][3], s, tex_wide, cb->alpha[cy][0], cb->alpha[cy][1], cb->alpha[cy][2], cb->alpha[cy][3]);
    }

    const uint8x16_t ptab = vld1q_u8((om->rgb_dither_sel == 0 || om->rgb_dither_sel == 2) ? magic_matrix : bayer_matrix);
    const int rgbsel = om->rgb_dither_sel, asel = om->alpha_dither_sel;
    const int need_noise = om->uses_noise || asel == 2;
    const uint32_t seed = pc->seed;

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        const uint8x8_t cvg8 = vld1_u8(&b->cvg[o]);
        const int16x8_t cvg = vreinterpretq_s16_u16(vmovl_u8(cvg8));

        uint8x8_t pv = vqtbl1_u8(ptab, vld1_u8(&b->ditidx[o]));
        int16x8_t cdv;
        uint8x8_t ad8;
        int16x8_t noise = vdupq_n_s16(0);
        if (need_noise) {
            uint32x4_t i0 = vld1q_u32(&b->fbidx[o]), i1 = vld1q_u32(&b->fbidx[o + 4]);
            uint32x4_t n0 = vandq_u32(k_prand4(i0, seed, 1), vdupq_n_u32(7));
            uint32x4_t n1 = vandq_u32(k_prand4(i1, seed, 1), vdupq_n_u32(7));
            noise = vorrq_s16(vshlq_n_s16(vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(n0), vmovn_u32(n1))), 6), vdupq_n_s16(0x20));
            vst1q_s16(&noise_s[o], noise);
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
        int16x8_t adith = vreinterpretq_s16_u16(vmovl_u8(ad8));
        vst1q_s16(&b->cdith[o], cdv);
        vst1_s8(&b->adith[o], vreinterpret_s8_u8(ad8));

        int16x8_t shade_a = vdupq_n_s16(0);
        if (sh_rgb || sh_a) {
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
            if (sh_rgb) for (int c = 0; c < 3; c++) vst1q_s16(&shade[c][o], sh[c]);
            vst1q_s16(&shade[3][o], sh[3]);
            shade_a = sh[3];
        }

        int16x8_t acalpha = vdupq_n_s16(0);
        if (two) {
            int16x8_t c0[4];
            for (int c = 0; c < 4; c++) c0[c] = cop_eval(&ops[0][c], o);
            for (int c = 0; c < 4; c++) vst1q_s16(&comb0[c][o], c0[c]);
            if (om->alpha_compare_en) {
                int16x8_t pre = k_clamp9v(c0[3]);
                pre = vbslq_s16(vceqq_s16(pre, vdupq_n_s16(0xff)), vdupq_n_s16(0x100), pre);
                if (!om->alpha_cvg_select) {
                    pre = vaddq_s16(pre, adith);
                    pre = vbslq_s16(vtstq_s16(pre, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), pre);
                } else {
                    if (om->cvg_times_alpha) pre = vshrq_n_s16(vaddq_s16(vmulq_s16(pre, cvg), vdupq_n_s16(4)), 3);
                    else pre = vshlq_n_s16(cvg, 5);
                    pre = vminq_s16(pre, vdupq_n_s16(0xff));
                }
                acalpha = pre;
            }
        }
        int16x8_t cf[4];
        for (int c = 0; c < 4; c++) cf[c] = cop_eval(&ops[1][c], o);

        int16x8_t pa = k_clamp9v(cf[3]);
        pa = vbslq_s16(vceqq_s16(pa, vdupq_n_s16(0xff)), vdupq_n_s16(0x100), pa);
        int16x8_t newcvg = cvg, temp = vdupq_n_s16(0);
        if (om->cvg_times_alpha) {
            temp = vshrq_n_s16(vaddq_s16(vmulq_s16(pa, cvg), vdupq_n_s16(4)), 3);
            newcvg = vandq_s16(vshrq_n_s16(temp, 5), vdupq_n_s16(0xf));
            vst1_u8(&b->cvg[o], vmovn_u16(vreinterpretq_u16_s16(newcvg)));
        }
        if (!om->alpha_cvg_select) {
            pa = vaddq_s16(pa, adith);
            pa = vbslq_s16(vtstq_s16(pa, vdupq_n_s16(0x100)), vdupq_n_s16(0xff), pa);
        } else {
            pa = om->cvg_times_alpha ? temp : vshlq_n_s16(newcvg, 5);
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

#include "kspec_comb.inc"
#include "kspec_shape.inc"
