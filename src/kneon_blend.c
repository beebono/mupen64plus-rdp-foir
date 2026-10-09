/*
 * mupen64plus-rdp-foir - kneon_blend.c
 * NEON blender: alpha compare, coverage test, the blend equation (with the
 * exact division table for the final cycle), colour dithering. Random alpha
 * compare dithering falls back to the scalar kernel.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kneon.h"
#include "spec.h"

FOIR_INLINE int16x8_t colsel(int sel, int16x8_t pix, int16x8_t mem, int16_t bl, int16_t fog)
{
    switch (sel & 3) {
    case 0: return pix;
    case 1: return mem;
    case 2: return vdupq_n_s16(bl);
    default: return vdupq_n_s16(fog);
    }
}

struct bl_eq {
    int16x8_t out[3];      /* result when the equation does not use the division table */
    uint16x8_t idx[3];     /* division table index when it does */
};

/* The blender state the kernel depends on. The generic kernel fills it from the render state at run time, a specialised
 * variant (kspec_blend.inc) passes a literal one, so the same body serves both. */
struct bs {
    int two;
    int m1a[2], m1b[2], m2a[2], m2b[2];
    int acmp, dal, aa, force, cvgcol, rgbd;
};

/* the blend equation; pix[3] = alpha used for the m1b = pixel alpha / inverse selections */
FOIR_INLINE void blend_eq(const struct rdp *r, const struct bs *S, int cyc, const int16x8_t pix[4], const int16x8_t mem[4],
                          int16x8_t shade_a, int16x8_t sha_shift, int16x8_t shb_shift, struct bl_eq *e)
{
    int16x8_t m1b;
    switch (S->m1b[cyc] & 3) {
    case 0: m1b = pix[3]; break;
    case 1: m1b = vdupq_n_s16((int16_t)r->fog_color.a); break;
    case 2: m1b = shade_a; break;
    default: m1b = vdupq_n_s16(0); break;
    }
    int16x8_t m2b;
    switch (S->m2b[cyc] & 3) {
    case 0: m2b = vandq_s16(vmvnq_s16(m1b), vdupq_n_s16(0xff)); break;
    case 1: m2b = mem[3]; break;
    case 2: m2b = vdupq_n_s16(0xff); break;
    default: m2b = vdupq_n_s16(0); break;
    }
    int16x8_t b1a = vshrq_n_s16(m1b, 3), b2a = vshrq_n_s16(m2b, 3);
    if (S->m2b[cyc] == 1) {
        b1a = vandq_s16(vshlq_s16(b1a, vnegq_s16(sha_shift)), vdupq_n_s16(0x3c));
        b2a = vorrq_s16(vshlq_s16(b2a, vnegq_s16(shb_shift)), vdupq_n_s16(3));
    }
    int16x8_t mulb = vaddq_s16(b2a, vdupq_n_s16(1));
    uint16x8_t sum = vreinterpretq_u16_s16(vshlq_n_s16(
        vaddq_s16(vaddq_s16(vandq_s16(b1a, vdupq_n_s16(~3)), vandq_s16(b2a, vdupq_n_s16(~3))), vdupq_n_s16(4)), 9));
    for (int ch = 0; ch < 3; ch++) {
        int16x8_t c1 = colsel(S->m1a[cyc], pix[ch], mem[ch], (int16_t)(&r->blend_color.r)[ch], (int16_t)(&r->fog_color.r)[ch]);
        int16x8_t c2 = colsel(S->m2a[cyc], pix[ch], mem[ch], (int16_t)(&r->blend_color.r)[ch], (int16_t)(&r->fog_color.r)[ch]);
        int16x8_t bl = vaddq_s16(vmulq_s16(c1, b1a), vmulq_s16(c2, mulb));
        e->out[ch] = vandq_s16(vshrq_n_s16(bl, 5), vdupq_n_s16(0xff));
        e->idx[ch] = vorrq_u16(sum, vreinterpretq_u16_s16(vandq_s16(vshrq_n_s16(bl, 2), vdupq_n_s16(0x7ff))));
    }
}

FOIR_INLINE int16x8_t rgb_dither8(int16x8_t c, int16x8_t dith)
{
    int16x8_t lo = vandq_s16(c, vdupq_n_s16(7));
    int16x8_t up = vbslq_s16(vcgtq_s16(c, vdupq_n_s16(247)), vdupq_n_s16(255),
                             vaddq_s16(vandq_s16(c, vdupq_n_s16(0xf8)), vdupq_n_s16(8)));
    return vbslq_s16(vcltq_s16(dith, lo), up, c);
}

FOIR_INLINE int16x8_t rand_thr(const struct px_batch *b, int o, uint32_t seed)
{
    uint32x4_t t0 = vandq_u32(k_prand4(vld1q_u32(&b->fbidx[o]), seed, 3), vdupq_n_u32(0xff));
    uint32x4_t t1 = vandq_u32(k_prand4(vld1q_u32(&b->fbidx[o + 4]), seed, 3), vdupq_n_u32(0xff));
    return vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(t0), vmovn_u32(t1)));
}

FOIR_INLINE void blend_body(struct rdp *r, struct px_batch *b, const struct bs S)
{
    NEON_HANDLED(3);
    const uint32_t seed = r->pc.seed;
    const int two = S.two;
    const int cyc = two ? 1 : 0;
    const int16x8_t thr = vdupq_n_s16((int16_t)r->blend_color.a);
    const int do_dither = S.rgbd != 3;
    const int partialreject = S.m2b[cyc] == 0 && S.m1b[cyc] == 0;
    const int ng = NGROUPS(b->n);

    for (int g = 0; g < ng; g++) {
        const int o = g * 8;
        uint16x8_t wen = k_widen_mask8(vld1_u8(&b->wen[o]));
        if (vmaxvq_u16(wen) == 0) continue;

        int16x8_t pix[4], mem[4];
        for (int c = 0; c < 4; c++) { pix[c] = vld1q_s16(&b->pixel[c][o]); mem[c] = vld1q_s16(&b->mem[c][o]); }
        const int16x8_t shade_a = vld1q_s16(&b->shade_alpha[o]);
        const int16x8_t sha_shift = vmovl_s8(vld1_s8(&b->blshifta[o]));
        const int16x8_t shb_shift = vmovl_s8(vld1_s8(&b->blshiftb[o]));

        /* coverage test */
        int16x8_t cv = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(S.aa ? &b->cvg[o] : &b->cvbit[o])));
        wen = vandq_u16(wen, vcgtq_s16(cv, vdupq_n_s16(0)));

        int16x8_t cur[4] = { pix[0], pix[1], pix[2], pix[3] };
        if (!two) {
            if (S.acmp) wen = vandq_u16(wen, vcgeq_s16(pix[3], S.dal ? rand_thr(b, o, seed) : thr));
        } else {
            struct bl_eq e0;
            blend_eq(r, &S, 0, pix, mem, shade_a, sha_shift, shb_shift, &e0);
            if (S.acmp) wen = vandq_u16(wen, vcgeq_s16(vld1q_s16(&b->acalpha[o]), S.dal ? rand_thr(b, o, seed) : thr));
            cur[0] = e0.out[0]; cur[1] = e0.out[1]; cur[2] = e0.out[2];
        }

        /* final colour */
        uint16x8_t pathA;       /* lanes that go through the m1a / equation branch */
        if (!S.cvgcol) pathA = vdupq_n_u16(0xffff);
        else pathA = k_widen_mask8(vld1_u8(&b->prewrap[o]));

        uint16x8_t blendm = k_widen_mask8(vld1_u8(&b->blend_en[o]));
        if (partialreject)
            blendm = vandq_u16(blendm, vcltq_s16(pix[3], vdupq_n_s16(0xff)));
        uint16x8_t eqm = vandq_u16(vandq_u16(blendm, pathA), wen);

        int16x8_t res[3];
        for (int c = 0; c < 3; c++) {
            int16x8_t m1 = colsel(S.m1a[cyc], cur[c], mem[c], (int16_t)(&r->blend_color.r)[c], (int16_t)(&r->fog_color.r)[c]);
            int16x8_t m2 = colsel(S.m2a[cyc], cur[c], mem[c], (int16_t)(&r->blend_color.r)[c], (int16_t)(&r->fog_color.r)[c]);
            res[c] = vbslq_s16(pathA, m1, m2);
        }
        if (vmaxvq_u16(eqm) != 0) {
            struct bl_eq e;
            int16x8_t pc[4] = { cur[0], cur[1], cur[2], pix[3] };
            blend_eq(r, &S, cyc, pc, mem, shade_a, sha_shift, shb_shift, &e);
            if (S.force) {
                for (int c = 0; c < 3; c++) res[c] = vbslq_s16(eqm, e.out[c], res[c]);
            } else {
                /* exact division table: per lane lookups */
                uint16_t idx[3][8] ALIGN16;
                int16_t outv[3][8] ALIGN16;
                uint16_t em[8] ALIGN16;
                for (int c = 0; c < 3; c++) {
                    vst1q_u16(idx[c], e.idx[c]);
                    vst1q_s16(outv[c], res[c]);
                }
                vst1q_u16(em, eqm);
                for (int l = 0; l < 8; l++)
                    if (em[l])
                        for (int c = 0; c < 3; c++) outv[c][l] = k_bldiv[idx[c][l] & 0x7fff];
                for (int c = 0; c < 3; c++) res[c] = vld1q_s16(outv[c]);
            }
        }
        if (do_dither) {
            int16x8_t dith = vld1q_s16(&b->cdith[o]);
            if (S.rgbd == 2) {
                res[0] = rgb_dither8(res[0], vandq_s16(dith, vdupq_n_s16(7)));
                res[1] = rgb_dither8(res[1], vandq_s16(vshrq_n_s16(dith, 3), vdupq_n_s16(7)));
                res[2] = rgb_dither8(res[2], vandq_s16(vshrq_n_s16(dith, 6), vdupq_n_s16(7)));
            } else {
                for (int c = 0; c < 3; c++) res[c] = rgb_dither8(res[c], dith);
            }
        }
        for (int c = 0; c < 3; c++) vst1q_s16(&b->fcol[c][o], res[c]);
        vst1_u8(&b->wen[o], vmovn_u16(vandq_u16(wen, vdupq_n_u16(1))));
    }
}

void neon_blend(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    struct bs S = { .two = om->cycle_type == CYCLE_TYPE_2, .acmp = om->alpha_compare_en, .dal = om->dither_alpha_en,
                    .aa = om->antialias_en, .force = om->force_blend, .cvgcol = om->color_on_cvg, .rgbd = om->rgb_dither_sel };
    for (int c = 0; c < 2; c++) {
        S.m1a[c] = om->blend_m1a[c]; S.m1b[c] = om->blend_m1b[c]; S.m2a[c] = om->blend_m2a[c]; S.m2b[c] = om->blend_m2b[c];
    }
    blend_body(r, b, S);
}

#include "kspec_blend.inc"
