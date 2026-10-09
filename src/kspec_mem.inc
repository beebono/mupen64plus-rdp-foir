/*
 * mupen64plus-rdp-foir - kspec_mem.inc (included by kneon_mem.c)
 * Specialised Z compare / colour buffer read and FB / Z store variants: zcmp_body() and store_body() with a
 * literal state. The variants are listed in spec_zcmp.def and spec_store.def. ANTIALIAS_EN and FORCE_BLEND are read
 * from the render state by every zcmp variant (they are not part of its key).
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "spec.h"

#define ZCMP(fbsz, rd, fbfmt, zc, zmode, shifts, relaxed) \
    ZCMP_NAMED(zcmp_##fbsz##rd##fbfmt##_##zc##zmode##_##shifts##relaxed, fbsz, rd, fbfmt, zc, zmode, shifts, relaxed)
#define ZCMP_NAMED(nm, fbsz, rd, fbfmt, zc, zmode, shifts, relaxed) \
    static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { zcmp_body(r, b, (const struct zs){ (fbsz) == PIXEL_SIZE_16BIT, rd, fbfmt, zc, zmode, r->om.antialias_en, r->om.force_blend, shifts, relaxed }); }
#include "spec_zcmp.def"
#undef ZCMP
#undef ZCMP_NAMED

#define ZCMP(fbsz, rd, fbfmt, zc, zmode, shifts, relaxed) \
    ZCMP_NAMED(zcmp_##fbsz##rd##fbfmt##_##zc##zmode##_##shifts##relaxed, fbsz, rd, fbfmt, zc, zmode, shifts, relaxed)
#define ZCMP_NAMED(nm, fbsz_, rd_, fbfmt_, zc_, zmode_, shifts_, relaxed_) \
    { .k.zcmp = { .fbsz = fbsz_, .rd = rd_, .fbfmt = fbfmt_, .zc = zc_, .zmode = zmode_, \
                  .shifts = shifts_, .relaxed = relaxed_ }, .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_zcmp[] = {
#include "spec_zcmp.def"
};
const int foir_spec_n_zcmp = (int)(sizeof(foir_spec_ents_zcmp) / sizeof(foir_spec_ents_zcmp[0]));
#undef ZCMP
#undef ZCMP_NAMED

#define STORE(fbsz, fbfmt, cvgdest, zupd) STORE_NAMED(store_##fbsz##fbfmt##cvgdest##zupd, fbsz, fbfmt, cvgdest, zupd)
#define STORE_NAMED(nm, fbsz, fbfmt, cvgdest, zupd) \
    static void spec_##nm(struct rdp *r, struct px_batch *b) \
    { store_body(r, b, (const struct ss){ (fbsz) == PIXEL_SIZE_16BIT, fbfmt, cvgdest, zupd }); }
#include "spec_store.def"
#undef STORE
#undef STORE_NAMED

#define STORE(fbsz, fbfmt, cvgdest, zupd) STORE_NAMED(store_##fbsz##fbfmt##cvgdest##zupd, fbsz, fbfmt, cvgdest, zupd)
#define STORE_NAMED(nm, fbsz_, fbfmt_, cvgdest_, zupd_) \
    { .k.store = { .fbsz = fbsz_, .fbfmt = fbfmt_, .cvgdest = cvgdest_, .zupd = zupd_ }, .fn = spec_##nm, .name = #nm },
const struct spec_ent foir_spec_ents_store[] = {
#include "spec_store.def"
};
const int foir_spec_n_store = (int)(sizeof(foir_spec_ents_store) / sizeof(foir_spec_ents_store[0]));
#undef STORE
#undef STORE_NAMED
