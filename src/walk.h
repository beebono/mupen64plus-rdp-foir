/*
 * mupen64plus-rdp-foir - walk.h
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_WALK_H
#define FOIR_WALK_H

#include "rdp.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

/* The state the RDP keeps from one subscanline to the next is linear in the subscanline number, so the span of scanline j is a pure function of j
 * and the setup record: edge positions are start + (k - k0) * step in 32-bit wrapping arithmetic, identical to k0 - k repeated additions
 * (unsigned + and * are ring operations modulo 2^32). Only validline is meaningful for an invalid scanline. */
FOIR_INLINE int walk_row_c(const struct prim_walk *pw, int j, struct span *sp)
{
    const int flip = pw->flip;
    const int k0 = j << 2;
    const int32_t clipxlshift = pw->clipxl2, clipxhshift = pw->clipxh2;

    /* edge positions at the first subscanline of the row (k0 - ycur >= 0; the multiplication wraps like the additions) */
    int32_t xright = (int32_t)(pw->xr0 + (uint32_t)(k0 - pw->ycur) * pw->xri);
    int32_t xleft;
    uint32_t xleft_inc;
    if (k0 > pw->ysw) { xleft = (int32_t)(pw->xl0 + (uint32_t)(k0 - pw->ysw) * pw->xli); xleft_inc = pw->xli; }
    else              { xleft = (int32_t)(pw->xm0 + (uint32_t)(k0 - pw->ycur) * pw->xmi); xleft_inc = pw->xmi; }

    int32_t maxxmx = 0, minxmx = 0, maxxhx = 0, minxhx = 0;
    if (flip) { maxxmx = 0; minxhx = 0xfff; }
    else      { maxxhx = 0; minxmx = 0xfff; }
    int allover = 1, allunder = 1, allinval = 1;
    int32_t xld = 0;
    int32_t xrsc, xlsc, stickybit;

    for (int spix = 0; spix < 4; spix++) {
        const int k = k0 + spix;
        if (k == pw->ysw) {
            xleft = (int32_t)pw->xl0;
            xleft_inc = pw->xli;
        }
        int curover, curunder;
        int invaly = k < pw->yhlimit || k >= pw->yllimit;

        stickybit = ((xright >> 1) & 0x1fff) > 0;
        xrsc = ((xright >> 13) & 0x1ffe) | stickybit;
        curunder = ((xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000)));
        xrsc = curunder ? clipxhshift : (((xright >> 13) & 0x3ffe) | stickybit);
        curover = ((xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift);
        xrsc = curover ? clipxlshift : xrsc;
        sp->majorx[spix] = xrsc & 0x1fff;
        allover &= curover;
        allunder &= curunder;

        stickybit = ((xleft >> 1) & 0x1fff) > 0;
        xlsc = ((xleft >> 13) & 0x1ffe) | stickybit;
        curunder = ((xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000)));
        xlsc = curunder ? clipxhshift : (((xleft >> 13) & 0x3ffe) | stickybit);
        curover = ((xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift);
        xlsc = curover ? clipxlshift : xlsc;
        sp->minorx[spix] = xlsc & 0x1fff;
        allover &= curover;
        allunder &= curunder;

        int curcross;
        if (flip)
            curcross = ((xleft ^ (1 << 27)) & (0x3fff << 14)) < ((xright ^ (1 << 27)) & (0x3fff << 14));
        else
            curcross = ((xright ^ (1 << 27)) & (0x3fff << 14)) < ((xleft ^ (1 << 27)) & (0x3fff << 14));

        invaly |= curcross;
        sp->invalyscan[spix] = invaly;
        allinval &= invaly;

        if (!invaly) {
            if (flip) {
                maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
                minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
            } else {
                minxmx = (((xlsc >> 3) & 0xfff) < minxmx) ? (xlsc >> 3) & 0xfff : minxmx;
                maxxhx = (((xrsc >> 3) & 0xfff) > maxxhx) ? (xrsc >> 3) & 0xfff : maxxhx;
            }
        }

        if (spix == pw->ldflag) xld = xright;

        xleft = (int32_t)((uint32_t)xleft + xleft_inc);
        xright = (int32_t)((uint32_t)xright + pw->xri);
    }

    if (flip) { sp->lx = maxxmx; sp->rx = minxhx; }
    else      { sp->lx = minxmx; sp->rx = maxxhx; }
    const int valid = !allinval && !allover && !allunder &&
                      (!pw->scfield || (pw->scfield && !(pw->sckeepodd ^ (j & 1))));
    sp->validline = valid;
    if (!valid) return 0;

    /* the interpolants, at the right edge of the subscanline ldflag */
    sp->unscrx = SIGN(xld >> 16, 12);
    const uint32_t xfrac = (uint32_t)((xld >> 8) & 0xff);
    const uint32_t n = (uint32_t)(j - (pw->ycur >> 2));
    for (int i = 0; i < 8; i++) {
        const uint32_t a = pw->a0[i] + n * pw->de[i];
        sp->att[i] = (int32_t)(((a & ~0x1ffu) + pw->diff[i] - xfrac * pw->dxh[i]) & ~0x3ffu);
    }
    return 1;
}

#if defined(FOIR_NEON)
/* the four subscanlines of the row in the four lanes of a vector */

FOIR_INLINE int32x4_t walk_clip(int32x4_t x, int32x4_t clipxh, int32x4_t clipxl, uint32x4_t *under, uint32x4_t *over)
{
    const int32x4_t m1fff = vdupq_n_s32(0x1fff);
    const uint32x4_t nz = vtstq_s32(vandq_s32(vshrq_n_s32(x, 1), m1fff), m1fff);
    const int32x4_t sticky = vreinterpretq_s32_u32(vshrq_n_u32(nz, 31));
    const int32x4_t sh = vshrq_n_s32(x, 13);
    const int32x4_t lo = vorrq_s32(vandq_s32(sh, vdupq_n_s32(0x1ffe)), sticky);
    const int32x4_t hi = vorrq_s32(vandq_s32(sh, vdupq_n_s32(0x3ffe)), sticky);
    const uint32x4_t u = vorrq_u32(vtstq_s32(x, vdupq_n_s32(0x8000000)),
                                   vandq_u32(vcltq_s32(lo, clipxh), vmvnq_u32(vtstq_s32(x, vdupq_n_s32(0x4000000)))));
    int32x4_t xsc = vbslq_s32(u, clipxh, hi);
    const uint32x4_t o = vorrq_u32(vtstq_s32(xsc, vdupq_n_s32(0x2000)), vcgeq_s32(vandq_s32(xsc, m1fff), clipxl));
    xsc = vbslq_s32(o, clipxl, xsc);
    *under = u;
    *over = o;
    return xsc;
}

FOIR_INLINE int walk_row_neon(const struct prim_walk *pw, int j, struct span *sp)
{
    static const uint32_t io[4] = { 0, 1, 2, 3 };
    const uint32x4_t iota = vld1q_u32(io);
    const int k0 = j << 2;
    const uint32_t n0 = (uint32_t)(k0 - pw->ycur);
    const int32x4_t kv = vreinterpretq_s32_u32(vaddq_u32(vdupq_n_u32((uint32_t)k0), iota));

    const uint32x4_t xr = vmlaq_n_u32(vdupq_n_u32(pw->xr0 + n0 * pw->xri), iota, pw->xri);
    const uint32x4_t xm = vmlaq_n_u32(vdupq_n_u32(pw->xm0 + n0 * pw->xmi), iota, pw->xmi);
    const uint32x4_t xl = vmlaq_n_u32(vdupq_n_u32(pw->xl0 + (uint32_t)(k0 - pw->ysw) * pw->xli), iota, pw->xli);
    const uint32x4_t sw = vcgeq_s32(kv, vdupq_n_s32(pw->ysw));
    const int32x4_t xright = vreinterpretq_s32_u32(xr), xleft = vreinterpretq_s32_u32(vbslq_u32(sw, xl, xm));

    const int32x4_t clipxh = vdupq_n_s32(pw->clipxh2), clipxl = vdupq_n_s32(pw->clipxl2);
    uint32x4_t ur, orr, ul, ol;
    const int32x4_t xrsc = walk_clip(xright, clipxh, clipxl, &ur, &orr);
    const int32x4_t xlsc = walk_clip(xleft, clipxh, clipxl, &ul, &ol);
    const int32x4_t m1fff = vdupq_n_s32(0x1fff);
    vst1q_s32(sp->majorx, vandq_s32(xrsc, m1fff));
    vst1q_s32(sp->minorx, vandq_s32(xlsc, m1fff));

    const int32x4_t flipbit = vdupq_n_s32(1 << 27), cm = vdupq_n_s32(0x3fff << 14);
    const uint32x4_t cl = vreinterpretq_u32_s32(vandq_s32(veorq_s32(xleft, flipbit), cm));
    const uint32x4_t cr = vreinterpretq_u32_s32(vandq_s32(veorq_s32(xright, flipbit), cm));
    const uint32x4_t cross = pw->flip ? vcltq_u32(cl, cr) : vcltq_u32(cr, cl);
    const uint32x4_t inv = vorrq_u32(cross, vorrq_u32(vcltq_s32(kv, vdupq_n_s32(pw->yhlimit)), vcgeq_s32(kv, vdupq_n_s32(pw->yllimit))));
    vst1q_u32((uint32_t *)sp->invalyscan, vshrq_n_u32(inv, 31));

    const int allinval = vminvq_u32(inv) != 0;
    const int allover = vminvq_u32(vandq_u32(orr, ol)) != 0, allunder = vminvq_u32(vandq_u32(ur, ul)) != 0;
    const int valid = !allinval && !allover && !allunder &&
                      (!pw->scfield || (pw->scfield && !(pw->sckeepodd ^ (j & 1))));
    sp->validline = valid;
    if (!valid) return 0;

    const int32x4_t m0fff = vdupq_n_s32(0xfff);
    const int32x4_t lv = vandq_s32(vshrq_n_s32(xlsc, 3), m0fff), rv = vandq_s32(vshrq_n_s32(xrsc, 3), m0fff);
    if (pw->flip) {
        sp->lx = (int)vmaxvq_u32(vbicq_u32(vreinterpretq_u32_s32(lv), inv));
        sp->rx = (int)vminvq_u32(vbslq_u32(inv, vreinterpretq_u32_s32(m0fff), vreinterpretq_u32_s32(rv)));
    } else {
        sp->lx = (int)vminvq_u32(vbslq_u32(inv, vreinterpretq_u32_s32(m0fff), vreinterpretq_u32_s32(lv)));
        sp->rx = (int)vmaxvq_u32(vbicq_u32(vreinterpretq_u32_s32(rv), inv));
    }

    const int32_t xld = (int32_t)(pw->xr0 + (n0 + (uint32_t)pw->ldflag) * pw->xri);
    sp->unscrx = SIGN(xld >> 16, 12);
    const uint32_t xfrac = (uint32_t)((xld >> 8) & 0xff);
    const uint32_t n = (uint32_t)(j - (pw->ycur >> 2));
    const uint32x4_t m1ff = vdupq_n_u32(~0x1ffu), m3ff = vdupq_n_u32(~0x3ffu);
    for (int h = 0; h < 8; h += 4) {
        uint32x4_t a = vmlaq_n_u32(vld1q_u32(&pw->a0[h]), vld1q_u32(&pw->de[h]), n);
        a = vaddq_u32(vandq_u32(a, m1ff), vld1q_u32(&pw->diff[h]));
        a = vandq_u32(vmlsq_n_u32(a, vld1q_u32(&pw->dxh[h]), xfrac), m3ff);
        vst1q_u32((uint32_t *)&sp->att[h], a);
    }
    return 1;
}
#endif

#if defined(FOIR_NEON)
#define walk_row walk_row_neon
#else
#define walk_row walk_row_c
#endif

#endif
