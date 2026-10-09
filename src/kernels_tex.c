/*
 * mupen64plus-rdp-foir - kernels_tex.c
 * Scalar reference texture kernel: perspective divide, LOD, tile selection,
 * point / bilinear / mid-texel filtering, TLUT and YUV conversion.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kernels_priv.h"

static const int32_t norm_point_table[64] = {
    0x4000, 0x3f04, 0x3e10, 0x3d22, 0x3c3c, 0x3b5d, 0x3a83, 0x39b1,
    0x38e4, 0x381c, 0x375a, 0x369d, 0x35e5, 0x3532, 0x3483, 0x33d9,
    0x3333, 0x3291, 0x31f4, 0x3159, 0x30c3, 0x3030, 0x2fa1, 0x2f15,
    0x2e8c, 0x2e06, 0x2d83, 0x2d03, 0x2c86, 0x2c0b, 0x2b93, 0x2b1e,
    0x2aab, 0x2a3a, 0x29cc, 0x2960, 0x28f6, 0x288e, 0x2828, 0x27c4,
    0x2762, 0x2702, 0x26a4, 0x2648, 0x25ed, 0x2594, 0x253d, 0x24e7,
    0x2492, 0x243f, 0x23ee, 0x239e, 0x234f, 0x2302, 0x22b6, 0x226c,
    0x2222, 0x21da, 0x2193, 0x214d, 0x2108, 0x20c5, 0x2082, 0x2041
};

static const int32_t norm_slope_table[64] = {
    0xf03, 0xf0b, 0xf11, 0xf19, 0xf20, 0xf25, 0xf2d, 0xf32,
    0xf37, 0xf3d, 0xf42, 0xf47, 0xf4c, 0xf50, 0xf55, 0xf59,
    0xf5d, 0xf62, 0xf64, 0xf69, 0xf6c, 0xf70, 0xf73, 0xf76,
    0xf79, 0xf7c, 0xf7f, 0xf82, 0xf84, 0xf87, 0xf8a, 0xf8c,
    0xf8e, 0xf91, 0xf93, 0xf95, 0xf97, 0xf99, 0xf9b, 0xf9d,
    0xf9f, 0xfa1, 0xfa3, 0xfa4, 0xfa6, 0xfa8, 0xfa9, 0xfaa,
    0xfac, 0xfae, 0xfaf, 0xfb0, 0xfb2, 0xfb3, 0xfb5, 0xfb5,
    0xfb7, 0xfb8, 0xfb9, 0xfba, 0xfbc, 0xfbc, 0xfbe, 0xfbe
};

int32_t k_maskbits[16];
static int32_t log2table[256];
int32_t k_tcdiv[0x8000];

void kernels_tex_init_tables(void)
{
    static bool done;
    if (done) return;
    log2table[0] = log2table[1] = 0;
    for (int i = 2; i < 256; i++)
        for (int k = 7; k > 0; k--)
            if ((i >> k) & 1) { log2table[i] = k; break; }

    for (int i = 0; i < 0x8000; i++) {
        int k;
        for (k = 1; k <= 14 && !((i << k) & 0x8000); k++) ;
        int shift = k - 1;
        int normout = (i << shift) & 0x3fff;
        int wnorm = (normout & 0xff) << 2;
        normout >>= 8;
        int temppoint = norm_point_table[normout];
        int tempslope = norm_slope_table[normout];
        tempslope = (tempslope | ~0x3ff) + 1;
        int tlu_rcp = (((tempslope * wnorm) >> 10) + temppoint) & 0x7fff;
        k_tcdiv[i] = shift | (tlu_rcp << 4);
    }
    k_maskbits[0] = 0x3ff;
    for (int i = 1; i < 16; i++) k_maskbits[i] = ((uint16_t)0xffff >> (16 - i)) & 0x3ff;
    done = true;
}

/* ---- perspective divide -------------------------------------------------- */

void tc_divide(const struct rdp *r, int32_t ss, int32_t st, int32_t sw, int32_t *sss, int32_t *sst)
{
    if (!r->om.persp_tex_en) {
        *sss = SIGN16(ss) & 0x1ffff;
        *sst = SIGN16(st) & 0x1ffff;
        return;
    }
    int w_carry = 0;
    if (SIGN16(sw) <= 0) w_carry = 1;
    sw &= 0x7fff;

    int shift = k_tcdiv[sw];
    int tlu_rcp = shift >> 4;
    shift &= 0xf;

    int sprod = SIGN16(ss) * tlu_rcp;
    int tprod = SIGN16(st) * tlu_rcp;

    int tempmask = ((1 << 30) - 1) & -((1 << 29) >> shift);
    int outofbounds_s = sprod & tempmask;
    int outofbounds_t = tprod & tempmask;

    int32_t temps, tempt;
    if (shift != 0xe) {
        int shift_value = 13 - shift;
        temps = sprod = (sprod >> shift_value);
        tempt = tprod = (tprod >> shift_value);
    } else {
        temps = sprod << 1;
        tempt = tprod << 1;
    }

    int overunder_s = 0, overunder_t = 0;
    if (outofbounds_s != tempmask && outofbounds_s != 0)
        overunder_s = !(sprod & (1 << 29)) ? (2 << 17) : (1 << 17);
    if (outofbounds_t != tempmask && outofbounds_t != 0)
        overunder_t = !(tprod & (1 << 29)) ? (2 << 17) : (1 << 17);
    if (w_carry) {
        overunder_s |= (2 << 17);
        overunder_t |= (2 << 17);
    }
    *sss = (temps & 0x1ffff) | overunder_s;
    *sst = (tempt & 0x1ffff) | overunder_t;
}

static void clamp17_one(int32_t *v)
{
    int32_t x = *v, anded;
    if (!(x & 0x40000)) {
        if (!(x & 0x20000)) {
            anded = x & 0x18000;
            if (anded != 0x8000) {
                if (anded != 0x10000) *v &= 0xffff;
                else *v = 0x8000;
            } else {
                *v = 0x7fff;
            }
        } else {
            *v = 0x8000;
        }
    } else {
        *v = 0x7fff;
    }
}

void tc_clamp17(int32_t *sss, int32_t *sst)
{
    clamp17_one(sss);
    clamp17_one(sst);
}

/* ---- LOD ------------------------------------------------------------------- */

static void lod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t *lod)
{
    int dels = SIGN(snext, 17) - SIGN(scurr, 17);
    if (dels & 0x20000) dels = ~dels & 0x1ffff;
    int delt = SIGN(tnext, 17) - SIGN(tcurr, 17);
    if (delt & 0x20000) delt = ~delt & 0x1ffff;
    dels = (dels > delt) ? dels : delt;
    dels = (previous > dels) ? previous : dels;
    *lod = dels & 0x7fff;
    if (dels & 0x1c000) *lod |= 0x4000;
}

static void lodfrac_lodtile(const struct rdp *r, int lodclamp, int32_t lod,
                            uint32_t *l_tile, uint32_t *magnify, uint32_t *distant, int32_t *lfdst)
{
    uint32_t ltil, dis, mag;
    int32_t lf;
    const struct other_modes *om = &r->om;

    if ((lod & 0x4000) || lodclamp) {
        mag = 0; ltil = 0; dis = 1; lf = 0xff;
    } else if (lod < r->min_level) {
        mag = 1; ltil = 0;
        dis = r->pc.max_level == 0;
        if (!om->sharpen_tex_en && !om->detail_tex_en) {
            lf = dis ? 0xff : 0;
        } else {
            lf = r->min_level << 3;
            if (om->sharpen_tex_en) lf |= 0x100;
        }
    } else if (lod < 32) {
        mag = 1; ltil = 0;
        dis = r->pc.max_level == 0;
        if (!om->sharpen_tex_en && !om->detail_tex_en) {
            lf = dis ? 0xff : 0;
        } else {
            lf = lod << 3;
            if (om->sharpen_tex_en) lf |= 0x100;
        }
    } else {
        mag = 0;
        ltil = log2table[(lod >> 5) & 0xff];
        if (r->pc.max_level)
            dis = ((lod & 0x6000) || ((int32_t)ltil >= r->pc.max_level)) != 0;
        else
            dis = 1;
        if (!om->sharpen_tex_en && !om->detail_tex_en && dis)
            lf = 0xff;
        else
            lf = ((lod << 3) >> ltil) & 0xff;
    }
    *distant = dis; *l_tile = ltil; *magnify = mag; *lfdst = lf;
}

/* Compute LOD fraction and the two tiles used by cycle 0 / cycle 1. */
static void lod_select(const struct rdp *r, int32_t s, int32_t t, int32_t w,
                       int32_t inits, int32_t initt, int prim_tile,
                       int *t1, int *t2, int32_t *lf)
{
    const struct prim_ctx *pc = &r->pc;
    *t1 = prim_tile;
    *t2 = (prim_tile + 1) & 7;
    *lf = 0;
    if (!r->om.dolod) return;

    int32_t nextsw = (w + pc->dwinc) >> 16, nexts = (s + pc->dsinc) >> 16, nextt = (t + pc->dtinc) >> 16;
    int32_t nextys = (s + pc->dsdy) >> 16, nextyt = (t + pc->dtdy) >> 16, nextysw = (w + pc->dwdy) >> 16;
    tc_divide(r, nexts, nextt, nextsw, &nexts, &nextt);
    tc_divide(r, nextys, nextyt, nextysw, &nextys, &nextyt);

    int lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) ||
                   (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);
    int32_t lod = 0;
    if (!lodclamp) {
        lod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
        lod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
    }
    uint32_t l_tile, magnify, distant;
    lodfrac_lodtile(r, lodclamp, lod, &l_tile, &magnify, &distant, lf);

    if (r->om.tex_lod_en) {
        if (distant) l_tile = (uint32_t)r->pc.max_level;
        if (!r->om.detail_tex_en) {
            *t1 = (prim_tile + l_tile) & 7;
            if (!(distant || (!r->om.sharpen_tex_en && magnify)))
                *t2 = (*t1 + 1) & 7;
            else
                *t2 = *t1;
        } else {
            if (!magnify) *t1 = (prim_tile + l_tile + 1);
            else *t1 = (prim_tile + l_tile);
            *t1 &= 7;
            if (!distant && !magnify) *t2 = (prim_tile + l_tile + 2) & 7;
            else *t2 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

/* ---- tile coordinate processing -------------------------------------------- */

static void tcshift_cycle(const struct tile *tile, int32_t *S, int32_t *T, int32_t *maxs, int32_t *maxt)
{
    int32_t coord = *S;
    int32_t shifter = tile->shift_s;
    if (shifter < 11) { coord = SIGN16(coord); coord >>= shifter; }
    else { coord <<= (16 - shifter); coord = SIGN16(coord); }
    *S = coord;
    *maxs = ((coord >> 3) >= tile->sh);

    coord = *T;
    shifter = tile->shift_t;
    if (shifter < 11) { coord = SIGN16(coord); coord >>= shifter; }
    else { coord <<= (16 - shifter); coord = SIGN16(coord); }
    *T = coord;
    *maxt = ((coord >> 3) >= tile->th);
}

static void tcclamp_cycle(const struct tile *tile, int32_t *S, int32_t *T, int32_t *SFRAC, int32_t *TFRAC,
                          int32_t maxs, int32_t maxt)
{
    int32_t locs = *S, loct = *T;
    if (tile->clampens) {
        if (maxs) { *S = tile->clampdiffs; *SFRAC = 0; }
        else if (!(locs & 0x10000)) *S = locs >> 5;
        else { *S = 0; *SFRAC = 0; }
    } else {
        *S = locs >> 5;
    }
    if (tile->clampent) {
        if (maxt) { *T = tile->clampdifft; *TFRAC = 0; }
        else if (!(loct & 0x10000)) *T = loct >> 5;
        else { *T = 0; *TFRAC = 0; }
    } else {
        *T = loct >> 5;
    }
}

static void tcclamp_cycle_light(const struct tile *tile, int32_t *S, int32_t *T, int32_t maxs, int32_t maxt)
{
    int32_t locs = *S, loct = *T;
    if (tile->clampens) {
        if (maxs) *S = tile->clampdiffs;
        else if (!(locs & 0x10000)) *S = locs >> 5;
        else *S = 0;
    } else {
        *S = locs >> 5;
    }
    if (tile->clampent) {
        if (maxt) *T = tile->clampdifft;
        else if (!(loct & 0x10000)) *T = loct >> 5;
        else *T = 0;
    } else {
        *T = loct >> 5;
    }
}

static void tcmask(const struct tile *tile, int32_t *S, int32_t *T)
{
    if (tile->mask_s) {
        if (tile->ms) {
            int32_t wrap = (*S >> tile->masksclamped) & 1;
            *S ^= (-wrap);
        }
        *S &= k_maskbits[tile->mask_s];
    }
    if (tile->mask_t) {
        if (tile->mt) {
            int32_t wrap = (*T >> tile->masktclamped) & 1;
            *T ^= (-wrap);
        }
        *T &= k_maskbits[tile->mask_t];
    }
}

static void tcmask_coupled(const struct tile *tile, int32_t *S, int32_t *sdiff, int32_t *T, int32_t *tdiff)
{
    if (tile->mask_s) {
        int32_t maskbits = k_maskbits[tile->mask_s];
        if (tile->ms) {
            int32_t wrap = (*S >> tile->masksclamped) & 1;
            *S ^= (-wrap);
            *S &= maskbits;
            if (((*S - wrap) & maskbits) == maskbits) *sdiff = 0;
            else *sdiff = 1 - (wrap << 1);
        } else {
            *S &= maskbits;
            if (*S == maskbits) *sdiff = -(*S);
            else *sdiff = 1;
        }
    } else {
        *sdiff = 1;
    }
    if (tile->mask_t) {
        int32_t maskbits = k_maskbits[tile->mask_t];
        if (tile->mt) {
            int32_t wrap = (*T >> tile->masktclamped) & 1;
            *T ^= (-wrap);
            *T &= maskbits;
            if (((*T - wrap) & maskbits) == maskbits) *tdiff = 0;
            else *tdiff = 1 - (wrap << 1);
        } else {
            *T &= maskbits;
            if (*T == maskbits) *tdiff = -(*T & 0xff);
            else *tdiff = 1;
        }
    } else {
        *tdiff = 1;
    }
}

/* ---- texel fetch with optional TLUT ----------------------------------------- */

static uint32_t tlut_index(const struct rdp *r, const struct tile *tl, int s, int tt)
{
    struct color c;
    if (tl->size == PIXEL_SIZE_4BIT) {
        /* nibble from tmem, palette in the upper bits */
        struct tile tmp = *tl;
        tmp.format = FORMAT_CI;
        tmem_fetch_texel(r, &tmp, s, tt, &c);
        return (uint32_t)c.r & 0xff;
    }
    if (tl->size == PIXEL_SIZE_8BIT) {
        struct tile tmp = *tl;
        tmp.format = FORMAT_CI;
        tmem_fetch_texel(r, &tmp, s, tt, &c);
        return (uint32_t)c.r & 0xff;
    }
    uint16_t raw = tmem_raw16(r, tl, s, tt);
    return raw >> 8;
}

static void fetch_one(const struct rdp *r, const struct tile *tl, int s, int tt, struct color *c)
{
    if (r->om.en_tlut) {
        tmem_fetch_tlut(r, tlut_index(r, tl, s, tt), c);
    } else {
        tmem_fetch_texel(r, tl, s, tt, c);
    }
}

static void fetch_quad(const struct rdp *r, const struct tile *tl, int s0, int sdiff, int t0, int tdiff,
                       struct color *c0, struct color *c1, struct color *c2, struct color *c3)
{
    int t1 = (t0 & 0xff) + tdiff;
    int s1 = s0 + sdiff;
    fetch_one(r, tl, s0, t0, c0);
    fetch_one(r, tl, s1, t0, c1);
    fetch_one(r, tl, s0, t1, c2);
    fetch_one(r, tl, s1, t1, c3);
}

/* ---- the texture pipeline stage -------------------------------------------- */

static void texture_cycle(const struct rdp *r, struct color *TEX, const struct color *prev,
                          int32_t SSS, int32_t SST, int tilenum, int cycle)
{
    const struct tile *tl = &r->tile[tilenum];
    const struct other_modes *om = &r->om;
    int bilerp = cycle ? om->bi_lerp1 : om->bi_lerp0;
    int convert = om->convert_one && cycle;
    struct color t0 = {0}, t1 = {0}, t2 = {0}, t3 = {0};
    int32_t sss1 = SSS, sst1 = SST, sdiff, tdiff, maxs, maxt;
    struct color out;

    tcshift_cycle(tl, &sss1, &sst1, &maxs, &maxt);
    sss1 = TRELATIVE(sss1, tl->sl);
    sst1 = TRELATIVE(sst1, tl->tl);

    if (om->sample_type || om->en_tlut) {
        int32_t sfrac = sss1 & 0x1f, tfrac = sst1 & 0x1f;
        int32_t sfracrg;
        int upper, upperrg, center, centerrg;

        tcclamp_cycle(tl, &sss1, &sst1, &sfrac, &tfrac, maxs, maxt);
        tcmask_coupled(tl, &sss1, &sdiff, &sst1, &tdiff);

        upper = (sfrac + tfrac) & 0x20;
        if (tl->format == FORMAT_YUV) {
            sfracrg = (sfrac >> 1) | ((sss1 & 1) << 4);
            upperrg = (sfracrg + tfrac) & 0x20;
        } else {
            upperrg = upper;
            sfracrg = sfrac;
        }

        if (bilerp) {
            if (!om->sample_type) {
                /* point sampled through the TLUT */
                fetch_one(r, tl, sss1, sst1, &t0);
                t1 = t2 = t3 = t0;
            } else {
                fetch_quad(r, tl, sss1, sdiff, sst1, tdiff, &t0, &t1, &t2, &t3);
            }

            if (!om->mid_texel) {
                center = centerrg = 0;
            } else {
                center = (sfrac == 0x10 && tfrac == 0x10);
                centerrg = (sfracrg == 0x10 && tfrac == 0x10);
            }

            if (!convert) {
                int32_t invtf = 0x20 - tfrac, invsf, invsfrg;
                if (!centerrg) {
                    if (upperrg) {
                        invsfrg = 0x20 - sfracrg;
                        out.r = t3.r + ((invsfrg * (t2.r - t3.r) + invtf * (t1.r - t3.r) + 0x10) >> 5);
                        out.g = t3.g + ((invsfrg * (t2.g - t3.g) + invtf * (t1.g - t3.g) + 0x10) >> 5);
                    } else {
                        out.r = t0.r + ((sfracrg * (t1.r - t0.r) + tfrac * (t2.r - t0.r) + 0x10) >> 5);
                        out.g = t0.g + ((sfracrg * (t1.g - t0.g) + tfrac * (t2.g - t0.g) + 0x10) >> 5);
                    }
                } else {
                    int32_t invt3r = ~t3.r, invt3g = ~t3.g;
                    out.r = t3.r + ((((t1.r + t2.r) << 6) - (t3.r << 7) + ((invt3r + t0.r) << 6) + 0xc0) >> 8);
                    out.g = t3.g + ((((t1.g + t2.g) << 6) - (t3.g << 7) + ((invt3g + t0.g) << 6) + 0xc0) >> 8);
                }
                if (!center) {
                    if (upper) {
                        invsf = 0x20 - sfrac;
                        out.b = t3.b + ((invsf * (t2.b - t3.b) + invtf * (t1.b - t3.b) + 0x10) >> 5);
                        out.a = t3.a + ((invsf * (t2.a - t3.a) + invtf * (t1.a - t3.a) + 0x10) >> 5);
                    } else {
                        out.b = t0.b + ((sfrac * (t1.b - t0.b) + tfrac * (t2.b - t0.b) + 0x10) >> 5);
                        out.a = t0.a + ((sfrac * (t1.a - t0.a) + tfrac * (t2.a - t0.a) + 0x10) >> 5);
                    }
                } else {
                    int32_t invt3b = ~t3.b, invt3a = ~t3.a;
                    out.b = t3.b + ((((t1.b + t2.b) << 6) - (t3.b << 7) + ((invt3b + t0.b) << 6) + 0xc0) >> 8);
                    out.a = t3.a + ((((t1.a + t2.a) << 6) - (t3.a << 7) + ((invt3a + t0.a) << 6) + 0xc0) >> 8);
                }
            } else {
                int32_t prevr = SIGN(prev->r, 9), prevg = SIGN(prev->g, 9), prevb = SIGN(prev->b, 9);
                if (!centerrg) {
                    if (upperrg) {
                        out.r = prevb + ((prevr * (t2.r - t3.r) + prevg * (t1.r - t3.r) + 0x80) >> 8);
                        out.g = prevb + ((prevr * (t2.g - t3.g) + prevg * (t1.g - t3.g) + 0x80) >> 8);
                    } else {
                        out.r = prevb + ((prevr * (t1.r - t0.r) + prevg * (t2.r - t0.r) + 0x80) >> 8);
                        out.g = prevb + ((prevr * (t1.g - t0.g) + prevg * (t2.g - t0.g) + 0x80) >> 8);
                    }
                } else {
                    int32_t invt3r = ~t3.r, invt3g = ~t3.g;
                    out.r = prevb + ((prevr * (t2.r - t3.r) + prevg * (t1.r - t3.r) + ((invt3r + t0.r) << 6) + 0xc0) >> 8);
                    out.g = prevb + ((prevr * (t2.g - t3.g) + prevg * (t1.g - t3.g) + ((invt3g + t0.g) << 6) + 0xc0) >> 8);
                }
                if (!center) {
                    if (upper) {
                        out.b = prevb + ((prevr * (t2.b - t3.b) + prevg * (t1.b - t3.b) + 0x80) >> 8);
                        out.a = prevb + ((prevr * (t2.a - t3.a) + prevg * (t1.a - t3.a) + 0x80) >> 8);
                    } else {
                        out.b = prevb + ((prevr * (t1.b - t0.b) + prevg * (t2.b - t0.b) + 0x80) >> 8);
                        out.a = prevb + ((prevr * (t1.a - t0.a) + prevg * (t2.a - t0.a) + 0x80) >> 8);
                    }
                } else {
                    int32_t invt3b = ~t3.b, invt3a = ~t3.a;
                    out.b = prevb + ((prevr * (t2.b - t3.b) + prevg * (t1.b - t3.b) + ((invt3b + t0.b) << 6) + 0xc0) >> 8);
                    out.a = prevb + ((prevr * (t2.a - t3.a) + prevg * (t1.a - t3.a) + ((invt3a + t0.a) << 6) + 0xc0) >> 8);
                }
            }
        } else {
            /* bi_lerp == 0: YUV -> RGB conversion on a point sample */
            if (convert) {
                t0 = t3 = *prev;
                t0.r = SIGN(t0.r, 9); t0.g = SIGN(t0.g, 9); t0.b = SIGN(t0.b, 9);
                t3.r = SIGN(t3.r, 9); t3.g = SIGN(t3.g, 9); t3.b = SIGN(t3.b, 9);
            } else if (!om->sample_type) {
                fetch_one(r, tl, sss1, sst1, &t0);
                t1 = t2 = t3 = t0;
            } else {
                fetch_quad(r, tl, sss1, sdiff, sst1, tdiff, &t0, &t1, &t2, &t3);
            }
            if (upperrg) {
                if (upper) {
                    out.r = t3.b + ((r->k0_tf * t3.g + 0x80) >> 8);
                    out.g = t3.b + ((r->k1_tf * t3.r + r->k2_tf * t3.g + 0x80) >> 8);
                    out.b = t3.b + ((r->k3_tf * t3.r + 0x80) >> 8);
                    out.a = t3.b;
                } else {
                    out.r = t0.b + ((r->k0_tf * t3.g + 0x80) >> 8);
                    out.g = t0.b + ((r->k1_tf * t3.r + r->k2_tf * t3.g + 0x80) >> 8);
                    out.b = t0.b + ((r->k3_tf * t3.r + 0x80) >> 8);
                    out.a = t0.b;
                }
            } else {
                if (upper) {
                    out.r = t3.b + ((r->k0_tf * t0.g + 0x80) >> 8);
                    out.g = t3.b + ((r->k1_tf * t0.r + r->k2_tf * t0.g + 0x80) >> 8);
                    out.b = t3.b + ((r->k3_tf * t0.r + 0x80) >> 8);
                    out.a = t3.b;
                } else {
                    out.r = t0.b + ((r->k0_tf * t0.g + 0x80) >> 8);
                    out.g = t0.b + ((r->k1_tf * t0.r + r->k2_tf * t0.g + 0x80) >> 8);
                    out.b = t0.b + ((r->k3_tf * t0.r + 0x80) >> 8);
                    out.a = t0.b;
                }
            }
        }
        out.r &= 0x1ff; out.g &= 0x1ff; out.b &= 0x1ff; out.a &= 0x1ff;
    } else {
        /* point sampling without TLUT */
        tcclamp_cycle_light(tl, &sss1, &sst1, maxs, maxt);
        tcmask(tl, &sss1, &sst1);
        if (bilerp) {
            if (!convert) {
                tmem_fetch_texel(r, tl, sss1, sst1, &t0);
                out.r = t0.r & 0x1ff; out.g = t0.g & 0x1ff; out.b = t0.b; out.a = t0.a;
            } else {
                out.r = out.g = out.b = out.a = prev->b;
            }
        } else {
            if (convert) {
                t0 = *prev;
                t0.r = SIGN(t0.r, 9); t0.g = SIGN(t0.g, 9); t0.b = SIGN(t0.b, 9);
            } else {
                tmem_fetch_texel(r, tl, sss1, sst1, &t0);
            }
            out.r = t0.b + ((r->k0_tf * t0.g + 0x80) >> 8);
            out.g = t0.b + ((r->k1_tf * t0.r + r->k2_tf * t0.g + 0x80) >> 8);
            out.b = t0.b + ((r->k3_tf * t0.r + 0x80) >> 8);
            out.a = t0.b & 0x1ff;
            out.r &= 0x1ff; out.g &= 0x1ff; out.b &= 0x1ff;
        }
    }
    *TEX = out;
}

/* ---- kernel entry -------------------------------------------------------------- */

/* Evaluate the texture unit at one pixel position. tex1 is only produced in two-cycle mode. */
static void tex_pixel(const struct rdp *r, int32_t s, int32_t t, int32_t w, int two,
                      struct color *tex0, struct color *tex1, int32_t *lodfrac)
{
    int prim_tile = r->pc.tilenum;
    int32_t sss, sst;
    tc_divide(r, s >> 16, t >> 16, w >> 16, &sss, &sst);
    int32_t inits = sss, initt = sst;
    int t1, t2;
    int32_t lf;
    tc_clamp17(&sss, &sst);
    lod_select(r, s, t, w, inits, initt, prim_tile, &t1, &t2, &lf);
    if (lodfrac) *lodfrac = lf;
    texture_cycle(r, tex0, tex0, sss, sst, t1, 0);
    if (two && tex1) texture_cycle(r, tex1, tex0, sss, sst, t2, 1);
}

void scalar_texture(struct rdp *r, struct px_batch *b)
{
    int n = b->n;
    int two = (r->om.cycle_type == CYCLE_TYPE_2);
    const struct prim_ctx *pc = &r->pc;

    for (int i = 0; i < n; i++) {
        struct color tex0 = {0}, tex1 = {0};
        int32_t lf = 0;
        tex_pixel(r, b->s[i], b->t[i], b->w[i], two, &tex0, &tex1, &lf);
        b->lodfrac[i] = (int16_t)lf;
        b->tex0[CH_R][i] = (int16_t)tex0.r; b->tex0[CH_G][i] = (int16_t)tex0.g;
        b->tex0[CH_B][i] = (int16_t)tex0.b; b->tex0[CH_A][i] = (int16_t)tex0.a;
        if (two) {
            b->tex1[CH_R][i] = (int16_t)tex1.r; b->tex1[CH_G][i] = (int16_t)tex1.g;
            b->tex1[CH_B][i] = (int16_t)tex1.b; b->tex1[CH_A][i] = (int16_t)tex1.a;
        }
        if (r->pf.next_texel) {
            struct color n0 = {0};
            tex_pixel(r, b->s[i] + pc->dsinc, b->t[i] + pc->dtinc, b->w[i] + pc->dwinc, 0, &n0, NULL, NULL);
            b->tex0n[CH_R][i] = (int16_t)n0.r; b->tex0n[CH_G][i] = (int16_t)n0.g;
            b->tex0n[CH_B][i] = (int16_t)n0.b; b->tex0n[CH_A][i] = (int16_t)n0.a;
            if (!two) {
                for (int c = 0; c < 4; c++) b->tex1[c][i] = b->tex0n[c][i];
            }
        }
    }
}
