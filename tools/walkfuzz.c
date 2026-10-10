/*
 * foir-walkfuzz: the per-row edge walker against the sequential one.
 *
 *   foir-walkfuzz [-n iterations] [-s seed] [-v]
 *
 * Random primitives (the 44-word edge record, scissor, interlace field, cycle type) go through
 *   - the sequential walker of foir commit c73eb8e (tools/walk_ref.inc, verbatim: every subscanline from the top vertex,
 *     one struct span per scanline, accumulating by repeated 32-bit additions), and
 *   - setup_walk() + walk_row_c() and, in the NEON build, walk_row_neon() (src/setup.c, src/walk.h): the setup record, and
 *     the span of any scanline computed from it by multiplication.
 * Compared: whether the primitive is visible at all, the first / last scanline, the per-primitive constants (r->pc), and for
 * every scanline from first to last the validity and, for valid ones, every field of the span (lx, rx, unscrx, the eight
 * interpolants, majorx, minorx, invalyscan). The rows are computed twice, in ascending and in shuffled order, so a row
 * does not depend on the rows computed before it.
 *
 * The primitives range from ordinary triangles and rectangles to extreme ones: 14-bit vertices from -8192 to 8191
 * subscanlines (the sequential walk then has up to 12,000 subscanlines to accumulate over), full-range 28-bit edge
 * positions and 30-bit slopes (the products wrap around many times), and fully random records.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rdp.h"
#include "../src/walk.h"

/* ---- the oracle ---------------------------------------------------------------------------------------------------- */

static struct span g_rows[1100];           /* a primitive has at most 1025 scanlines */
static struct span *walkref_rows(int nrows) { if (nrows > 1100) abort(); return g_rows; }
static struct { int called, jlo, start, end; } g_cap;
static void walkref_draw(struct rdp *r, struct span *rows, int jlo, int start, int end)
{
    (void)r; (void)rows;
    g_cap.called = 1; g_cap.jlo = jlo; g_cap.start = start; g_cap.end = end;
}
#define raster_draw(r, rows, jlo, start, end) walkref_draw(r, rows, jlo, start, end)
#include "walk_ref.inc"
#undef raster_draw

/* ---- random primitives --------------------------------------------------------------------------------------------- */

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static uint32_t rndn(uint32_t n) { return rnd() % n; }
static int32_t rndr(int32_t lo, int32_t hi) { return lo + (int32_t)rndn((uint32_t)(hi - lo + 1)); }

static const int32_t extremes[] = { 0, 1, -1, 0x7fffffff, (int32_t)0x80000000, 0x3fffffff, (int32_t)0xc0000000, 0x0fffffff, (int32_t)0xf0000000,
                                    0x08000000, (int32_t)0xf8000000, 0x8000, (int32_t)0xffff8000, 0x7fff0000, (int32_t)0xffff0000 };
#define NEXT (int)(sizeof(extremes) / sizeof(extremes[0]))

/* a slope or coefficient of the given width (bits) in a mix of magnitudes */
static int32_t coef(int bits)
{
    uint32_t m = bits >= 32 ? 0xffffffffu : ((1u << bits) - 1);
    switch (rndn(10)) {
    case 0: return 0;
    case 1: return (int32_t)(rnd() & m);                                          /* any */
    case 2: return extremes[rndn(NEXT)] & (int32_t)m;
    case 3: case 4: return (int32_t)((int32_t)(rnd() & 0xfffff) - 0x80000) & (int32_t)m;     /* small */
    case 5: case 6: return (int32_t)((int32_t)(rnd() & 0x3fffff) - 0x200000) & (int32_t)m;
    case 7: return (int32_t)((int32_t)(rnd() & 0x3ffffff) - 0x2000000) & (int32_t)m;
    default: return (int32_t)(rnd() & m) >> (rndn(bits));
    }
}

static int32_t word32(void)
{
    switch (rndn(6)) {
    case 0: return 0;
    case 1: return extremes[rndn(NEXT)];
    case 2: return (int32_t)(rnd() & 0xffff) - 0x8000;
    default: return (int32_t)rnd() ^ (int32_t)(rnd() << 16);
    }
}

static void gen_prim(int32_t ew[44], int style)
{
    memset(ew, 0, 44 * sizeof(int32_t));
    const int flip = rndn(2);
    int yh, ym, yl;
    if (style == 0) {                      /* ordinary: on screen, sorted vertices */
        yh = rndr(-60, 800) * 4 + (int)rndn(4);
        ym = yh + rndr(0, 500) * 4 + (int)rndn(4);
        yl = ym + rndr(0, 500) * 4 + (int)rndn(4);
        if (rndn(8) == 0) { yh &= ~3; ym &= ~3; yl |= 3; }          /* rectangles */
    } else if (style == 1) {               /* anywhere in the 14-bit range, any order */
        yh = rndr(-8192, 8191); ym = rndr(-8192, 8191); yl = rndr(-8192, 8191);
        if (rndn(2)) { int t[3] = { yh, ym, yl }; for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++) if (t[j] > t[j + 1]) { int x = t[j]; t[j] = t[j + 1]; t[j + 1] = x; } yh = t[0]; ym = t[1]; yl = t[2]; }
    } else {                               /* far above the screen: long walks before the first visible row */
        yh = rndr(-8192, -200); ym = rndr(yh, 1500); yl = rndr(ym, 4095);
    }
    ew[0] = ((int32_t)rndn(64) << 24) | (flip << 23) | ((int32_t)rndn(8) << 19) | ((int32_t)rndn(8) << 16) | (yl & 0x3fff);
    if (style == 3 || rndn(40) == 0) ew[0] = word32() & ~0x00800000, ew[0] |= flip << 23;
    ew[1] = ((ym & 0x3fff) << 16) | (yh & 0x3fff);
    if (rndn(60) == 0) ew[1] = word32();
    int32_t mag = rndn(3) ? 0 : 1;
    for (int e = 0; e < 3; e++) {
        int32_t x = mag ? coef(28) : (int32_t)((rndr(-40, 700) << 16) + (int)(rnd() & 0xffff));
        int32_t d = coef(30);
        if (rndn(3) == 0) d = (int32_t)((int32_t)(rnd() & 0x3ffff) - 0x20000);       /* up to +-2 pixels per row */
        const uint32_t sign = rndn(10) ? ((uint32_t)d & 0x80000000u ? 0xc0000000u : 0) : (rnd() & 0xc0000000u);
        ew[2 + 2 * e] = x & 0xfffffff;
        ew[3 + 2 * e] = (int32_t)(((uint32_t)d & 0x3fffffffu) | sign);
    }
    if (rndn(12) == 0) for (int i = 2; i < 8; i++) ew[i] = word32();
    for (int i = 8; i < 44; i++) ew[i] = word32();
    if (rndn(5) == 0) memset(&ew[8], 0, 32 * sizeof(int32_t));            /* no shade / texture, like a rectangle */
    if (rndn(6) == 0) memset(&ew[40], 0, 4 * sizeof(int32_t));
}

#if defined(FOIR_NEON)
#define NVAR 2          /* the portable walker and the NEON one */
#else
#define NVAR 1
#endif

/* ---- comparison ------------------------------------------------------------------------------------------------------ */

static int span_same(const struct span *a, const struct span *b)
{
    return a->lx == b->lx && a->rx == b->rx && a->unscrx == b->unscrx && a->validline == b->validline &&
           !memcmp(a->att, b->att, sizeof(a->att)) && !memcmp(a->majorx, b->majorx, sizeof(a->majorx)) &&
           !memcmp(a->minorx, b->minorx, sizeof(a->minorx)) && !memcmp(a->invalyscan, b->invalyscan, sizeof(a->invalyscan));
}

static void dump_span(const char *tag, const struct span *s)
{
    fprintf(stderr, "    %s: valid=%d lx=%d rx=%d unscrx=%d att=%08x %08x %08x %08x %08x %08x %08x %08x\n"
                    "         major=%d,%d,%d,%d minor=%d,%d,%d,%d inval=%d%d%d%d\n", tag, s->validline, s->lx, s->rx, s->unscrx,
            s->att[0], s->att[1], s->att[2], s->att[3], s->att[4], s->att[5], s->att[6], s->att[7],
            s->majorx[0], s->majorx[1], s->majorx[2], s->majorx[3], s->minorx[0], s->minorx[1], s->minorx[2], s->minorx[3],
            s->invalyscan[0], s->invalyscan[1], s->invalyscan[2], s->invalyscan[3]);
}


int main(int argc, char **argv)
{
    long n = 20000;
    uint64_t seed = 1;
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "usage: %s [-n iterations] [-s seed] [-v]\n", argv[0]); return 2; }
    }
    g_rng ^= seed * 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 8; i++) rnd();

    struct rdp *ra = calloc(1, sizeof(*ra)), *rb = calloc(1, sizeof(*rb));
    if (!ra || !rb) return 1;
    long visible = 0, invisible = 0, rows = 0, validrows = 0, valid_switch = 0, valid_wrapx = 0, valid_wrapa = 0, switch_in = 0;
    long diffs = 0, shown = 0, flips = 0, scrows = 0, first_bad = -1;
    long perstyle[4] = { 0 }, vis_style[4] = { 0 };

    for (long it = 0; it < n; it++) {
        int32_t ew[44];
        const int style = (int)(it % 4);
        gen_prim(ew, style);
        perstyle[style]++;

        /* scissor and the rest of the state the walker reads */
        struct rdp *rs[2] = { ra, rb };
        const int x0 = rndn(3) ? 0 : (int)rndn(0x1000), y0 = rndn(3) ? 0 : (int)rndn(0x1000);
        int cxl = x0 + (int)rndn(0x1000), cyl = y0 + (int)rndn(0x1000);
        if (cxl > 0xfff) cxl = 0xfff;
        if (cyl > 0xfff) cyl = 0xfff;
        int cxh = x0, cyh = y0;
        switch (rndn(16)) {                /* the scissor of a reset RDP, and the ordinary 320x240 one */
        case 0: cxh = 0x2000; cyh = 0x2000; cxl = 0; cyl = 0; break;
        case 1: case 2: case 3: cxh = 0; cyh = 0; cxl = 1280; cyl = 960; break;
        default: break;
        }
        const int scf = rndn(4) == 0, sck = (int)rndn(2), ct = (int)rndn(4);
        const uint32_t prims = rnd(), fseed = rnd();
        for (int q = 0; q < 2; q++) {
            struct rdp *r = rs[q];
            memset(&r->pc, 0, sizeof(r->pc));
            r->clip.xh = (uint16_t)cxh; r->clip.yh = (uint16_t)cyh; r->clip.xl = (uint16_t)cxl; r->clip.yl = (uint16_t)cyl;
            r->scfield = scf; r->sckeepodd = sck; r->om.cycle_type = ct;
            r->stat_prims = prims; r->frame_seed = fseed;
            r->mt = NULL;
        }

        memset(g_rows, 0xa5, sizeof(g_rows));
        memset(&g_cap, 0, sizeof(g_cap));
        walk_edges_ref(ra, ew);

        struct prim_walk pw;
        int start = 0, end = 0;
        const bool vis = setup_walk(rb, ew, &pw, &start, &end);
        int bad = 0;
        if (vis != (bool)g_cap.called) { bad = 1; if (shown++ < 5) fprintf(stderr, "iteration %ld: visible %d, reference %d\n", it, vis, g_cap.called); }
        if (!bad && memcmp(&ra->pc, &rb->pc, sizeof(ra->pc))) { bad = 1; if (shown++ < 5) fprintf(stderr, "iteration %ld: prim_ctx differs\n", it); }
        if (!bad && vis) {
            if (start != g_cap.start || end != g_cap.end || start != g_cap.jlo) {
                bad = 1;
                if (shown++ < 5) fprintf(stderr, "iteration %ld: rows %d..%d, reference %d..%d (jlo %d)\n", it, start, end, g_cap.start, g_cap.end, g_cap.jlo);
            }
        }
        if (vis) {
            visible++; vis_style[style]++;
            if (!bad) {
                /* rows in ascending order, then in a shuffled order (a row must not depend on the ones before it) */
                const int nr = end - start + 1;
                int *order = nr > 0 ? malloc(sizeof(int) * (size_t)nr) : NULL;
                for (int q = 0; q < nr; q++) order[q] = start + q;
                for (int pass = 0; pass < 2; pass++) {
                    if (pass) for (int q = nr - 1; q > 0; q--) { int t = (int)rndn((uint32_t)q + 1); int x = order[q]; order[q] = order[t]; order[t] = x; }
                    for (int q = 0; q < nr; q++) {
                        const int y = order[q];
                        const struct span *ref = &g_rows[y - g_cap.jlo];
                        for (int var = 0; var < NVAR; var++) {
                            struct span sp;
                            memset(&sp, 0x5a, sizeof(sp));
                            const int v = var == 0 ? walk_row_c(&pw, y, &sp)
#if defined(FOIR_NEON)
                                                    : walk_row_neon(&pw, y, &sp);
#else
                                                    : 0;
#endif
                            const int same = v == ref->validline && sp.validline == ref->validline && (!v || span_same(&sp, ref));
                            if (!same) {
                                bad = 1;
                                if (shown++ < 5) {
                                    fprintf(stderr, "iteration %ld (style %d): row %d (range %d..%d) differs (%s)\n", it, style, y, start, end, var ? "NEON" : "C");
                                    dump_span("walk_row", &sp); dump_span("sequential", ref);
                                }
                            }
                        }
                        if (pass == 0) {
                            rows++;
                            if (ref->validline) {
                                validrows++;
                                const int64_t nk = (int64_t)(y * 4 - pw.ycur);
                                if (pw.ysw >= y * 4 && pw.ysw < y * 4 + 4) valid_switch++;
                                if (nk * (int64_t)(int32_t)pw.xri != (int64_t)(int32_t)(nk * (int64_t)(int32_t)pw.xri) ||
                                    nk * (int64_t)(int32_t)pw.xmi != (int64_t)(int32_t)(nk * (int64_t)(int32_t)pw.xmi)) valid_wrapx++;
                                int wa = 0;
                                for (int c = 0; c < 8; c++) {
                                    const int64_t pr = (int64_t)(y - (pw.ycur >> 2)) * (int64_t)(int32_t)pw.de[c];
                                    wa |= pr != (int64_t)(int32_t)pr;
                                }
                                valid_wrapa += wa;
                                if (pw.scfield) scrows++;
                            }
                        }
                    }
                }
                free(order);
                if (pw.ysw != INT32_MAX && pw.ysw < pw.yllimit) switch_in++;
                flips += pw.flip;
            }
        } else invisible++;
        if (bad) { diffs++; if (first_bad < 0) first_bad = it + 1; }
        if (bad && !verbose && shown >= 5) { /* keep going, count */ }
    }
    printf("foir-walkfuzz: %ld primitives (%ld visible, %ld not; per style %ld/%ld/%ld/%ld visible of %ld/%ld/%ld/%ld), %ld flipped,\n"
           "  %ld scanlines compared (%ld valid; %ld with the switch of the left edge in the row, %ld with a wrapped edge product, %ld with a wrapped\n"
           "  interpolant product, %ld on a scissored field), %ld primitives with the left edge switching inside the drawn range: %ld differences\n",
           n, visible, invisible, vis_style[0], vis_style[1], vis_style[2], vis_style[3], perstyle[0], perstyle[1], perstyle[2], perstyle[3],
           flips, rows, validrows, valid_switch, valid_wrapx, valid_wrapa, scrows, switch_in, diffs);
    if (diffs) printf("  FAILED: %ld primitives differ, the first one is number %ld\n", diffs, first_bad);
    free(ra); free(rb);
    return diffs ? 1 : 0;
}
