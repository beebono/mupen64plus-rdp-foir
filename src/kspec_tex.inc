/*
 * mupen64plus-rdp-foir - kspec_tex.inc (included by kneon_tex.c)
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "spec.h"

#ifdef FOIR_STATS
#define FOIR_LSTFB() LST_ADD(lst_lodfb, 1)
#else
#define FOIR_LSTFB() ((void)0)
#endif
/* out of line: the variants are flattened and must not inline it */
static __attribute__((noinline)) void tex_generic(struct rdp *r, struct px_batch *b)
{
#ifdef FOIR_STATS
    LST_ADD(lst_generic, 1);
#endif
    neon_texture(r, b);
}

FOIR_INLINE bool lod_chain_matches(const struct rdp *r, int fmt, int siz)
{
    const int tn = r->pc.tilenum;
    for (int k = 0; k <= lod_chain_last(r) && k < 8; k++) {
        const struct tile *t = &r->tile[(tn + k) & 7];
        if (t->format != fmt || t->size != siz) return false;
    }
    return true;
}

FOIR_INLINE bool lod_chain_nonyuv(const struct rdp *r)
{
    const int tn = r->pc.tilenum;
    for (int k = 0; k <= lod_chain_last(r) && k < 8; k++)
        if (r->tile[(tn + k) & 7].format == FORMAT_YUV) return false;
    return true;
}

/* TEXL: LOD fraction needed (lodt: the LOD also selects the tile); every tile the LOD can select must have the primitive tile's format, checked per batch (else generic).
 * TEXA / TEXAL: "any format": format, size, TLUT type and MID_TEXEL come from the state; YUV tiles go to the generic kernel. */
#define TEX(two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1) \
    TEX_NAMED(tex_##two##t0##t1##nx##_##fmt0##siz0##fmt1##siz1##_##smp##mid##tlut##tlt##persp##bl0##bl1, \
              two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1)
#define TEX_NAMED(nm, two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1) \
    static __attribute__((flatten)) void spec_##nm(struct rdp *r, struct px_batch *b) \
    { \
        static const struct tspec S = { two, t0, t1, nx, 0, 0, 0, persp, \
                                        { siz0, fmt0, tlut, tlt, smp, mid, bl0 }, { siz1, fmt1, tlut, tlt, smp, mid, bl1 }, 0 }; \
        texture_body(r, b, &S); \
    }
#define TEXL(two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1, lodt) \
    TEXL_NAMED(texl_##two##t0##t1##nx##_##fmt0##siz0##fmt1##siz1##_##smp##mid##tlut##tlt##persp##bl0##bl1##_##lodt, \
               two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1, lodt)
#define TEXL_NAMED(nm, two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1, lodt) \
    static __attribute__((flatten)) void spec_##nm(struct rdp *r, struct px_batch *b) \
    { \
        static const struct tspec S = { two, t0, t1, nx, 1, lodt, lodt, persp, \
                                        { siz0, fmt0, tlut, tlt, smp, mid, bl0 }, { siz1, fmt1, tlut, tlt, smp, mid, bl1 }, 0 }; \
        if ((lodt) && !lod_chain_matches(r, fmt0, siz0)) { FOIR_LSTFB(); tex_generic(r, b); return; } \
        texture_body(r, b, &S); \
    }
#define TEXA(two, t0, t1, nx, smp, tlut, persp, bl0, bl1) \
    TEXA_NAMED(texa_##two##t0##t1##nx##_##smp##tlut##persp##bl0##bl1, two, t0, t1, nx, smp, tlut, persp, bl0, bl1)
#define TEXA_NAMED(nm, two, t0, t1, nx, smp, tlut, persp, bl0, bl1) \
    static __attribute__((flatten)) void spec_##nm(struct rdp *r, struct px_batch *b) \
    { \
        static const struct tspec S = { two, t0, t1, nx, 0, 0, 0, persp, { 0, 0, tlut, 0, smp, TSP_MID_RT, bl0 }, { 0, 0, tlut, 0, smp, TSP_MID_RT, bl1 }, 1 }; \
        texture_body(r, b, &S); \
    }
#define TEXAL(two, t0, t1, nx, smp, tlut, persp, bl0, bl1, lodt) \
    TEXAL_NAMED(texal_##two##t0##t1##nx##_##smp##tlut##persp##bl0##bl1##_##lodt, two, t0, t1, nx, smp, tlut, persp, bl0, bl1, lodt)
#define TEXAL_NAMED(nm, two, t0, t1, nx, smp, tlut, persp, bl0, bl1, lodt) \
    static __attribute__((flatten)) void spec_##nm(struct rdp *r, struct px_batch *b) \
    { \
        static const struct tspec S = { two, t0, t1, nx, 1, lodt, 0, persp, { 0, 0, tlut, 0, smp, TSP_MID_RT, bl0 }, { 0, 0, tlut, 0, smp, TSP_MID_RT, bl1 }, 1 }; \
        if ((lodt) && !lod_chain_nonyuv(r)) { tex_generic(r, b); return; } \
        texture_body(r, b, &S); \
    }
#include "spec_tex.def"
#undef TEX
#undef TEX_NAMED
#undef TEXL
#undef TEXL_NAMED
#undef TEXA
#undef TEXA_NAMED
#undef TEXAL
#undef TEXAL_NAMED

#define TEX(two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1) \
    TEX_NAMED(tex_##two##t0##t1##nx##_##fmt0##siz0##fmt1##siz1##_##smp##mid##tlut##tlt##persp##bl0##bl1, \
              two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1)
#define TEX_NAMED(nm, two_, t0_, t1_, nx_, fmt0_, siz0_, fmt1_, siz1_, smp_, mid_, tlut_, tlt_, persp_, bl0_, bl1_) \
    { .k.tex = { .two = two_, .t0 = t0_, .t1 = t1_, .nx = nx_, .fmt0 = fmt0_, .siz0 = siz0_, .fmt1 = fmt1_, .siz1 = siz1_, \
                 .smp = smp_, .mid = mid_, .tlut = tlut_, .tlt = tlt_, .persp = persp_, .bl0 = bl0_, .bl1 = bl1_ }, \
      .fn = spec_##nm, .name = #nm },
#define TEXL(two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1, lodt) \
    TEXL_NAMED(texl_##two##t0##t1##nx##_##fmt0##siz0##fmt1##siz1##_##smp##mid##tlut##tlt##persp##bl0##bl1##_##lodt, \
               two, t0, t1, nx, fmt0, siz0, fmt1, siz1, smp, mid, tlut, tlt, persp, bl0, bl1, lodt)
#define TEXL_NAMED(nm, two_, t0_, t1_, nx_, fmt0_, siz0_, fmt1_, siz1_, smp_, mid_, tlut_, tlt_, persp_, bl0_, bl1_, lodt_) \
    { .k.tex = { .two = two_, .t0 = t0_, .t1 = t1_, .nx = nx_, .fmt0 = fmt0_, .siz0 = siz0_, .fmt1 = fmt1_, .siz1 = siz1_, \
                 .smp = smp_, .mid = mid_, .tlut = tlut_, .tlt = tlt_, .persp = persp_, .bl0 = bl0_, .bl1 = bl1_, .lod = 1, .lodt = lodt_ }, \
      .fn = spec_##nm, .name = #nm },
#define TEXA(two, t0, t1, nx, smp, tlut, persp, bl0, bl1) \
    TEXA_NAMED(texa_##two##t0##t1##nx##_##smp##tlut##persp##bl0##bl1, two, t0, t1, nx, smp, tlut, persp, bl0, bl1)
#define TEXA_NAMED(nm, two_, t0_, t1_, nx_, smp_, tlut_, persp_, bl0_, bl1_) \
    { .k.tex = { .two = two_, .t0 = t0_, .t1 = t1_, .nx = nx_, .smp = smp_, .tlut = tlut_, .persp = persp_, .bl0 = bl0_, .bl1 = bl1_, .any = 1 }, \
      .fn = spec_##nm, .name = #nm },
#define TEXAL(two, t0, t1, nx, smp, tlut, persp, bl0, bl1, lodt) \
    TEXAL_NAMED(texal_##two##t0##t1##nx##_##smp##tlut##persp##bl0##bl1##_##lodt, two, t0, t1, nx, smp, tlut, persp, bl0, bl1, lodt)
#define TEXAL_NAMED(nm, two_, t0_, t1_, nx_, smp_, tlut_, persp_, bl0_, bl1_, lodt_) \
    { .k.tex = { .two = two_, .t0 = t0_, .t1 = t1_, .nx = nx_, .smp = smp_, .tlut = tlut_, .persp = persp_, .bl0 = bl0_, .bl1 = bl1_, \
                 .lod = 1, .lodt = lodt_, .any = 1 }, \
      .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_tex[] = {
#include "spec_tex.def"
};
const int foir_spec_n_tex = (int)(sizeof(foir_spec_ents_tex) / sizeof(foir_spec_ents_tex[0]));
#undef TEX
#undef TEX_NAMED
#undef TEXL
#undef TEXL_NAMED
#undef TEXA
#undef TEXA_NAMED
#undef TEXAL
#undef TEXAL_NAMED
