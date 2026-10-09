/*
 * mupen64plus-rdp-foir - kspec_blend.inc (included by kneon_blend.c)
 * Specialised blender variants: blend_body() with a literal blender state (selectors, alpha compare, coverage,
 * dither). The variants are listed in spec_blend.def.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "spec.h"

/* BLEND: every option literal. BLENDX: the "any option" form of the same selectors (alpha compare, coverage, dither
 * options read from the render state). A state looks for BLEND first, then BLENDX. */
#define BLEND(two, a0, b0, c0, d0, a1, b1, c1, d1, acmp, dal, aa, force, cvgcol, rgbd) \
    BLEND_NAMED(blend_##two##_##a0##b0##c0##d0##_##a1##b1##c1##d1##_##acmp##dal##aa##force##cvgcol##rgbd, \
                two, a0, b0, c0, d0, a1, b1, c1, d1, acmp, dal, aa, force, cvgcol, rgbd)
#define BLEND_NAMED(nm, two, a0, b0, c0, d0, a1, b1, c1, d1, acmp_, dal_, aa_, force_, cvgcol_, rgbd_) \
    static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { blend_body(r, b, (const struct bs){ two, { a0, a1 }, { b0, b1 }, { c0, c1 }, { d0, d1 }, acmp_, dal_, aa_, force_, cvgcol_, rgbd_ }); }
#define BLENDX(two, a0, b0, c0, d0, a1, b1, c1, d1) \
    BLENDX_NAMED(blendx_##two##_##a0##b0##c0##d0##_##a1##b1##c1##d1, two, a0, b0, c0, d0, a1, b1, c1, d1)
#define BLENDX_NAMED(nm, two, a0, b0, c0, d0, a1, b1, c1, d1) \
    static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { const struct other_modes *om = &r->om; \
      blend_body(r, b, (const struct bs){ two, { a0, a1 }, { b0, b1 }, { c0, c1 }, { d0, d1 }, om->alpha_compare_en, \
                                          om->alpha_compare_en && om->dither_alpha_en, om->antialias_en, om->force_blend, \
                                          om->color_on_cvg, om->rgb_dither_sel }); }
#include "spec_blend.def"
#undef BLEND
#undef BLEND_NAMED
#undef BLENDX
#undef BLENDX_NAMED

#define BLEND(two, a0, b0, c0, d0, a1, b1, c1, d1, acmp, dal, aa, force, cvgcol, rgbd) \
    BLEND_NAMED(blend_##two##_##a0##b0##c0##d0##_##a1##b1##c1##d1##_##acmp##dal##aa##force##cvgcol##rgbd, \
                two, a0, b0, c0, d0, a1, b1, c1, d1, acmp, dal, aa, force, cvgcol, rgbd)
#define BLEND_NAMED(nm, two_, a0, b0, c0, d0, a1, b1, c1, d1, acmp_, dal_, aa_, force_, cvgcol_, rgbd_) \
    { .k.blend = { .m1a = { a0, a1 }, .m1b = { b0, b1 }, .m2a = { c0, c1 }, .m2b = { d0, d1 }, .two = two_, .acmp = acmp_, \
                   .dal = dal_, .aa = aa_, .force = force_, .cvgcol = cvgcol_, .rgbd = rgbd_ }, .fn = spec_##nm, .name = #nm },
#define BLENDX(two, a0, b0, c0, d0, a1, b1, c1, d1) \
    BLENDX_NAMED(blendx_##two##_##a0##b0##c0##d0##_##a1##b1##c1##d1, two, a0, b0, c0, d0, a1, b1, c1, d1)
#define BLENDX_NAMED(nm, two_, a0, b0, c0, d0, a1, b1, c1, d1) \
    { .k.blend = { .m1a = { a0, a1 }, .m1b = { b0, b1 }, .m2a = { c0, c1 }, .m2b = { d0, d1 }, .two = two_, .any = 1 }, \
      .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_blend[] = {
#include "spec_blend.def"
};
const int foir_spec_n_blend = (int)(sizeof(foir_spec_ents_blend) / sizeof(foir_spec_ents_blend[0]));
#undef BLEND
#undef BLEND_NAMED
#undef BLENDX
#undef BLENDX_NAMED
