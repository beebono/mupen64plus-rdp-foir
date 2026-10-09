/*
 * mupen64plus-rdp-foir - setup.c
 * Primitive setup: decodes triangle / rectangle commands into the RDP's
 * canonical 44-word edge-walker record and runs the 4-subscanline edge walker,
 * producing one `struct span` per scanline for the rasteriser.
 *
 * The fixed-point formats and the subscanline/coverage clip logic follow the
 * hardware behaviour documented by angrylion-rdp-plus.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"
#include "mt.h"

static int32_t normalize_dzpix(int32_t sum)
{
    if (sum & 0xc000) return 0x8000;
    if (!(sum & 0xffff)) return 1;
    if (sum == 1) return 3;
    for (int count = 0x2000; count > 0; count >>= 1)
        if (sum & count) return count << 1;
    return 0;
}

/* Walk the edges. `ew` is the canonical 44-word record (see setup_*): words
 * 0-7 edges, 8-23 shade, 24-39 texture, 40-43 depth. */
static void walk_edges(struct rdp *r, const int32_t *ew)
{
    struct prim_ctx *pc = &r->pc;
    int xleft, xright, xleft_inc, xright_inc;
    int32_t rr, gg, bb, aa, zz, ss, tt, ww;
    int drdx, dgdx, dbdx, dadx, dzdx, dsdx, dtdx, dwdx;
    int drdy, dgdy, dbdy, dady, dzdy, dsdy, dtdy, dwdy;
    int drde, dgde, dbde, dade, dzde, dsde, dtde, dwde;

    int flip = (ew[0] & 0x800000) != 0;
    pc->max_level = (ew[0] >> 19) & 7;
    int tilenum = (ew[0] >> 16) & 7;

    int32_t yl = SIGN(ew[0], 14);
    int32_t ym = SIGN(ew[1] >> 16, 14);
    int32_t yh = SIGN(ew[1], 14);
    int32_t xl = SIGN(ew[2], 28);
    int32_t xh = SIGN(ew[4], 28);
    int32_t xm = SIGN(ew[6], 28);
    int32_t dxldy = SIGN(ew[3], 30);
    int32_t dxhdy = SIGN(ew[5], 30);
    int32_t dxmdy = SIGN(ew[7], 30);

    rr   = (ew[8] & 0xffff0000) | ((ew[12] >> 16) & 0xffff);
    gg   = ((ew[8] << 16) & 0xffff0000) | (ew[12] & 0xffff);
    bb   = (ew[9] & 0xffff0000) | ((ew[13] >> 16) & 0xffff);
    aa   = ((ew[9] << 16) & 0xffff0000) | (ew[13] & 0xffff);
    drdx = (ew[10] & 0xffff0000) | ((ew[14] >> 16) & 0xffff);
    dgdx = ((ew[10] << 16) & 0xffff0000) | (ew[14] & 0xffff);
    dbdx = (ew[11] & 0xffff0000) | ((ew[15] >> 16) & 0xffff);
    dadx = ((ew[11] << 16) & 0xffff0000) | (ew[15] & 0xffff);
    drde = (ew[16] & 0xffff0000) | ((ew[20] >> 16) & 0xffff);
    dgde = ((ew[16] << 16) & 0xffff0000) | (ew[20] & 0xffff);
    dbde = (ew[17] & 0xffff0000) | ((ew[21] >> 16) & 0xffff);
    dade = ((ew[17] << 16) & 0xffff0000) | (ew[21] & 0xffff);
    drdy = (ew[18] & 0xffff0000) | ((ew[22] >> 16) & 0xffff);
    dgdy = ((ew[18] << 16) & 0xffff0000) | (ew[22] & 0xffff);
    dbdy = (ew[19] & 0xffff0000) | ((ew[23] >> 16) & 0xffff);
    dady = ((ew[19] << 16) & 0xffff0000) | (ew[23] & 0xffff);

    ss   = (ew[24] & 0xffff0000) | ((ew[28] >> 16) & 0xffff);
    tt   = ((ew[24] << 16) & 0xffff0000) | (ew[28] & 0xffff);
    ww   = (ew[25] & 0xffff0000) | ((ew[29] >> 16) & 0xffff);
    dsdx = (ew[26] & 0xffff0000) | ((ew[30] >> 16) & 0xffff);
    dtdx = ((ew[26] << 16) & 0xffff0000) | (ew[30] & 0xffff);
    dwdx = (ew[27] & 0xffff0000) | ((ew[31] >> 16) & 0xffff);
    dsde = (ew[32] & 0xffff0000) | ((ew[36] >> 16) & 0xffff);
    dtde = ((ew[32] << 16) & 0xffff0000) | (ew[36] & 0xffff);
    dwde = (ew[33] & 0xffff0000) | ((ew[37] >> 16) & 0xffff);
    dsdy = (ew[34] & 0xffff0000) | ((ew[38] >> 16) & 0xffff);
    dtdy = ((ew[34] << 16) & 0xffff0000) | (ew[38] & 0xffff);
    dwdy = (ew[35] & 0xffff0000) | ((ew[39] >> 16) & 0xffff);

    zz   = ew[40];
    dzdx = ew[41];
    dzde = ew[42];
    dzdy = ew[43];

    /* per-pixel increments handed to the rasteriser */
    pc->flip = flip;
    pc->tilenum = tilenum;
    pc->ds = dsdx & ~0x1f;
    pc->dt = dtdx & ~0x1f;
    pc->dw = dwdx & ~0x1f;
    pc->dr = drdx & ~0x1f;
    pc->dg = dgdx & ~0x1f;
    pc->db = dbdx & ~0x1f;
    pc->da = dadx & ~0x1f;
    pc->dz = dzdx;

    pc->drdy = SIGN(drdy >> 14, 13);
    pc->dgdy = SIGN(dgdy >> 14, 13);
    pc->dbdy = SIGN(dbdy >> 14, 13);
    pc->dady = SIGN(dady >> 14, 13);
    pc->dzdy = SIGN(dzdy >> 10, 22);
    pc->cdr = SIGN(pc->dr >> 14, 13);
    pc->cdg = SIGN(pc->dg >> 14, 13);
    pc->cdb = SIGN(pc->db >> 14, 13);
    pc->cda = SIGN(pc->da >> 14, 13);
    pc->cdz = SIGN(pc->dz >> 10, 22);

    pc->seed = r->stat_prims * 0x9e3779b1u + r->frame_seed * 0x7feb352du;
    pc->dsdy = dsdy & ~0x7fff;
    pc->dtdy = dtdy & ~0x7fff;
    pc->dwdy = dwdy & ~0x7fff;

    int dzdy_dz = (dzdy >> 16) & 0xffff;
    int dzdx_dz = (dzdx >> 16) & 0xffff;
    int dzpix = ((dzdy_dz & 0x8000) ? ((~dzdy_dz) & 0x7fff) : dzdy_dz)
              + ((dzdx_dz & 0x8000) ? ((~dzdx_dz) & 0x7fff) : dzdx_dz);
    pc->dzpix = normalize_dzpix(dzpix & 0xffff) & 0xffff;

    xleft_inc = (dxmdy >> 2) & ~0x1;
    xright_inc = (dxhdy >> 2) & ~0x1;
    xright = xh & ~0x1;
    xleft = xm & ~0x1;

    int sign_dxhdy = (ew[5] & 0x80000000) != 0;
    int do_offset = !(sign_dxhdy ^ flip);
    int dsdiff, dtdiff, dwdiff, drdiff, dgdiff, dbdiff, dadiff, dzdiff;

    if (do_offset) {
        int dsdeh = dsde & ~0x1ff, dtdeh = dtde & ~0x1ff, dwdeh = dwde & ~0x1ff, drdeh = drde & ~0x1ff;
        int dgdeh = dgde & ~0x1ff, dbdeh = dbde & ~0x1ff, dadeh = dade & ~0x1ff, dzdeh = dzde & ~0x1ff;
        int dsdyh = dsdy & ~0x1ff, dtdyh = dtdy & ~0x1ff, dwdyh = dwdy & ~0x1ff, drdyh = drdy & ~0x1ff;
        int dgdyh = dgdy & ~0x1ff, dbdyh = dbdy & ~0x1ff, dadyh = dady & ~0x1ff, dzdyh = dzdy & ~0x1ff;
        dsdiff = dsdeh - (dsdeh >> 2) - dsdyh + (dsdyh >> 2);
        dtdiff = dtdeh - (dtdeh >> 2) - dtdyh + (dtdyh >> 2);
        dwdiff = dwdeh - (dwdeh >> 2) - dwdyh + (dwdyh >> 2);
        drdiff = drdeh - (drdeh >> 2) - drdyh + (drdyh >> 2);
        dgdiff = dgdeh - (dgdeh >> 2) - dgdyh + (dgdyh >> 2);
        dbdiff = dbdeh - (dbdeh >> 2) - dbdyh + (dbdyh >> 2);
        dadiff = dadeh - (dadeh >> 2) - dadyh + (dadyh >> 2);
        dzdiff = dzdeh - (dzdeh >> 2) - dzdyh + (dzdyh >> 2);
    } else {
        dsdiff = dtdiff = dwdiff = drdiff = dgdiff = dbdiff = dadiff = dzdiff = 0;
    }

    int dsdxh, dtdxh, dwdxh, drdxh, dgdxh, dbdxh, dadxh, dzdxh;
    if (r->om.cycle_type != CYCLE_TYPE_COPY) {
        dsdxh = (dsdx >> 8) & ~1; dtdxh = (dtdx >> 8) & ~1; dwdxh = (dwdx >> 8) & ~1;
        drdxh = (drdx >> 8) & ~1; dgdxh = (dgdx >> 8) & ~1; dbdxh = (dbdx >> 8) & ~1;
        dadxh = (dadx >> 8) & ~1; dzdxh = (dzdx >> 8) & ~1;
    } else {
        dsdxh = dtdxh = dwdxh = drdxh = dgdxh = dbdxh = dadxh = dzdxh = 0;
    }

    struct span *rows = NULL;
    int jlo = 0;
    int j;
    int xfrac = 0;
#define SP(jj) rows[(jj) - jlo]

#define ADJUST_ATTR()                                                         \
    do {                                                                      \
        SP(j).s = ((ss & ~0x1ff) + dsdiff - (xfrac * dsdxh)) & ~0x3ff;     \
        SP(j).t = ((tt & ~0x1ff) + dtdiff - (xfrac * dtdxh)) & ~0x3ff;     \
        SP(j).w = ((ww & ~0x1ff) + dwdiff - (xfrac * dwdxh)) & ~0x3ff;     \
        SP(j).r = ((rr & ~0x1ff) + drdiff - (xfrac * drdxh)) & ~0x3ff;     \
        SP(j).g = ((gg & ~0x1ff) + dgdiff - (xfrac * dgdxh)) & ~0x3ff;     \
        SP(j).b = ((bb & ~0x1ff) + dbdiff - (xfrac * dbdxh)) & ~0x3ff;     \
        SP(j).a = ((aa & ~0x1ff) + dadiff - (xfrac * dadxh)) & ~0x3ff;     \
        SP(j).z = ((zz & ~0x1ff) + dzdiff - (xfrac * dzdxh)) & ~0x3ff;     \
    } while (0)

    int32_t maxxmx = 0, minxmx = 0, maxxhx = 0, minxhx = 0;
    int spix;
    int ycur = yh & ~3;
    int ldflag = (sign_dxhdy ^ flip) ? 0 : 3;
    int invaly = 1;
    int32_t xrsc, xlsc, stickybit;
    int32_t yllimit, yhlimit;

    if (yl & 0x2000) yllimit = 1;
    else if (yl & 0x1000) yllimit = 0;
    else yllimit = (yl & 0xfff) < r->clip.yl;
    yllimit = yllimit ? yl : r->clip.yl;

    int ylfar = yllimit | 3;
    bool ylclear = false;
    if ((yl >> 2) > (ylfar >> 2))
        ylfar += 4;
    else if ((yllimit >> 2) >= 0 && (yllimit >> 2) < 1023)
        ylclear = true;

    if (yh & 0x2000) yhlimit = 0;
    else if (yh & 0x1000) yhlimit = 1;
    else yhlimit = (yh >= r->clip.yh);
    yhlimit = yhlimit ? yh : r->clip.yh;

    /* scanlines this primitive can produce: yhlimit >> 2 .. (yllimit >> 2) + 1 */
    jlo = yhlimit >> 2;
    {
        int jhi = (yllimit >> 2) + 1;
        if (jlo < 0) jlo = 0;
        if (jhi < jlo || (yllimit >> 2) < 0) return;      /* nothing visible */
        int nrows = jhi - jlo + 1;
        rows = r->mt ? mt_alloc_rows(r, nrows) : r->span_scratch;
        for (int q = 0; q < nrows; q++) rows[q].validline = 0;
        (void)ylclear;
    }

    int yhclose = yhlimit & ~3;
    int32_t clipxlshift = r->clip.xl << 1;
    int32_t clipxhshift = r->clip.xh << 1;
    int allover = 1, allunder = 1, curover, curunder;
    int allinval = 1;
    int32_t curcross;

    xfrac = (xright >> 8) & 0xff;

    for (int k = ycur; k <= ylfar; k++) {
        if (k == ym) {
            xleft = xl & ~1;
            xleft_inc = (dxldy >> 2) & ~1;
        }
        spix = k & 3;

        if (k >= yhclose) {
            invaly = k < yhlimit || k >= yllimit;
            j = k >> 2;

            if (spix == 0) {
                if (flip) { maxxmx = 0; minxhx = 0xfff; }
                else      { maxxhx = 0; minxmx = 0xfff; }
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = ((xright >> 1) & 0x1fff) > 0;
            xrsc = ((xright >> 13) & 0x1ffe) | stickybit;
            curunder = ((xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000)));
            xrsc = curunder ? clipxhshift : (((xright >> 13) & 0x3ffe) | stickybit);
            curover = ((xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift);
            xrsc = curover ? clipxlshift : xrsc;
            SP(j).majorx[spix] = xrsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            stickybit = ((xleft >> 1) & 0x1fff) > 0;
            xlsc = ((xleft >> 13) & 0x1ffe) | stickybit;
            curunder = ((xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000)));
            xlsc = curunder ? clipxhshift : (((xleft >> 13) & 0x3ffe) | stickybit);
            curover = ((xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift);
            xlsc = curover ? clipxlshift : xlsc;
            SP(j).minorx[spix] = xlsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            if (flip)
                curcross = ((xleft ^ (1 << 27)) & (0x3fff << 14)) < ((xright ^ (1 << 27)) & (0x3fff << 14));
            else
                curcross = ((xright ^ (1 << 27)) & (0x3fff << 14)) < ((xleft ^ (1 << 27)) & (0x3fff << 14));

            invaly |= curcross;
            SP(j).invalyscan[spix] = invaly;
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

            if (spix == ldflag) {
                SP(j).unscrx = SIGN(xright >> 16, 12);
                xfrac = (xright >> 8) & 0xff;
                ADJUST_ATTR();
            }

            if (spix == 3) {
                if (flip) { SP(j).lx = maxxmx; SP(j).rx = minxhx; }
                else      { SP(j).lx = minxmx; SP(j).rx = maxxhx; }
                SP(j).validline = !allinval && !allover && !allunder &&
                                    (!r->scfield || (r->scfield && !(r->sckeepodd ^ (j & 1))));
            }
        }

        if (spix == 3) {
            ss += dsde; tt += dtde; ww += dwde;
            rr += drde; gg += dgde; bb += dbde; aa += dade; zz += dzde;
        }
        xleft += xleft_inc;
        xright += xright_inc;
    }
#undef ADJUST_ATTR
#undef SP

    int start = yhlimit >> 2, end = yllimit >> 2;
    if (start < 0) start = 0;
    if (end > 1023) end = 1023;
    r->stat_prims++;
    raster_draw(r, rows, jlo, start, end);
}

/* ---- triangles ----------------------------------------------------------- */

void setup_triangle(struct rdp *r, const uint32_t *a)
{
    int32_t ew[44];
    memset(ew, 0, sizeof(ew));
    unsigned id = (a[0] >> 24) & 0x3f;
    bool has_shade = (id & 4) != 0;
    bool has_tex = (id & 2) != 0;
    bool has_z = (id & 1) != 0;
    uint32_t pos = 8;

    memcpy(&ew[0], a, 8 * sizeof(uint32_t));
    if (has_shade) { memcpy(&ew[8], a + pos, 16 * sizeof(uint32_t)); pos += 16; }
    if (has_tex)   { memcpy(&ew[24], a + pos, 16 * sizeof(uint32_t)); pos += 16; }
    if (has_z)     { memcpy(&ew[40], a + pos, 4 * sizeof(uint32_t)); }
    walk_edges(r, ew);
}

/* ---- rectangles ----------------------------------------------------------- */

void setup_texrect(struct rdp *r, const uint32_t *a, bool flipped)
{
    uint32_t tilenum = (a[1] >> 24) & 0x7;
    uint32_t xl = (a[0] >> 12) & 0xfff;
    uint32_t yl = (a[0] >>  0) & 0xfff;
    uint32_t xh = (a[1] >> 12) & 0xfff;
    uint32_t yh = (a[1] >>  0) & 0xfff;
    int32_t s = (a[2] >> 16) & 0xffff;
    int32_t t = (a[2] >>  0) & 0xffff;
    int32_t dsdx = SIGN16((a[3] >> 16) & 0xffff);
    int32_t dtdy = SIGN16((a[3] >>  0) & 0xffff);

    if (r->om.cycle_type == CYCLE_TYPE_FILL || r->om.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ew[44];
    memset(ew, 0, sizeof(ew));
    ew[0] = (int32_t)(((flipped ? 0x25u : 0x24u) << 24) | ((0x80 | tilenum) << 16) | yl);
    ew[1] = (int32_t)((yl << 16) | yh);
    ew[2] = (int32_t)((xlint << 16) | ((xl & 3) << 14));
    ew[4] = (int32_t)((xhint << 16) | ((xh & 3) << 14));
    ew[6] = (int32_t)((xlint << 16) | ((xl & 3) << 14));
    ew[24] = (s << 16) | t;
    if (!flipped) {
        ew[26] = (dsdx >> 5) << 16;
        ew[30] = ((dsdx & 0x1f) << 11) << 16;
        ew[32] = (dtdy >> 5) & 0xffff;
        ew[34] = (dtdy >> 5) & 0xffff;
        ew[36] = (dtdy & 0x1f) << 11;
        ew[38] = (dtdy & 0x1f) << 11;
    } else {
        ew[26] = (dtdy >> 5) & 0xffff;
        ew[30] = ((dtdy & 0x1f) << 11);
        ew[32] = (dsdx >> 5) << 16;
        ew[34] = (dsdx >> 5) << 16;
        ew[36] = (int32_t)((uint32_t)(dsdx & 0x1f) << 27);
        ew[38] = (int32_t)((uint32_t)(dsdx & 0x1f) << 27);
    }
    walk_edges(r, ew);
}

void setup_fillrect(struct rdp *r, const uint32_t *a)
{
    uint32_t xl = (a[0] >> 12) & 0xfff;
    uint32_t yl = (a[0] >>  0) & 0xfff;
    uint32_t xh = (a[1] >> 12) & 0xfff;
    uint32_t yh = (a[1] >>  0) & 0xfff;

    if (r->om.cycle_type == CYCLE_TYPE_FILL || r->om.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ew[44];
    memset(ew, 0, sizeof(ew));
    ew[0] = (int32_t)((0x3680u << 16) | yl);
    ew[1] = (int32_t)((yl << 16) | yh);
    ew[2] = (int32_t)((xlint << 16) | ((xl & 3) << 14));
    ew[4] = (int32_t)((xhint << 16) | ((xh & 3) << 14));
    ew[6] = (int32_t)((xlint << 16) | ((xl & 3) << 14));
    walk_edges(r, ew);
}
