/*
 * mupen64plus-rdp-foir - spec.h
 * Static pipeline specialisation (M3). Every pixel stage (texture, combine, zcmp, blend, store) has a
 * generic NEON kernel that handles any render state, and a set of variants compiled ahead of time for the
 * state shapes real games use, with the state constants folded into the code. A variant is selected per
 * stage by a small key derived from the render state; a state without a variant simply keeps the
 * generic kernel for that stage.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_SPEC_H
#define FOIR_SPEC_H

#include "rdp.h"

#ifndef FOIR_SK_SPLIT
#define FOIR_SK_SPLIT 0     /* 1: shape kernels know which variable operands are constants (experimental, see kspec_shape.inc) */
#endif

enum { SPEC_TEX, SPEC_COMB, SPEC_ZCMP, SPEC_BLEND, SPEC_STORE, SPEC_NSTAGES };
/* pseudo stage: the shape-specialised combiner kernels (a second table behind SPEC_COMB, see "Shape kernels" below) */
#define SPEC_SHAPE SPEC_NSTAGES

/* ---- stage keys: 32 bytes each, padding zero, compared bytewise ---------------------------------------- */

/* texture unit: tile formats of the tiles the primitive samples, sampling mode and which texels the combiner reads.
 * A second kind of key (any = 1, the "any format" kernels) is the shape of the texture unit alone (cycles, texels read,
 * filter, TLUT use, perspective, BI_LERP, LOD): the formats and sizes of the tiles, the TLUT type and MID_TEXEL are read
 * from the state, so one kernel serves every format combination. */
struct tex_key {
    uint8_t two;                  /* two-cycle */
    uint8_t t0, t1, nx;           /* TEXEL0 (first texture cycle), second texture cycle, next-pixel TEXEL1 are needed */
    uint8_t fmt0, siz0;           /* format / size of the primitive's tile (any: zero) */
    uint8_t fmt1, siz1;           /* tile + 1 (two-cycle with t1 only, else 0; any: zero) */
    uint8_t smp;                  /* SAMPLE_TYPE: bilinear */
    uint8_t mid;                  /* MID_TEXEL (bilinear only; any: zero, read at run time) */
    uint8_t tlut, tlt;            /* EN_TLUT, TLUT_TYPE (IA; any: zero) */
    uint8_t persp;                /* PERSP_TEX_EN */
    uint8_t bl0, bl1;             /* BI_LERP_0 / BI_LERP_1 (bl1: two-cycle only) */
    uint8_t lod;                  /* LOD fraction needed (variants: all tiles the LOD can select have the format of tile 0) */
    uint8_t lodt;                 /* TEX_LOD_EN: the LOD selects the tiles */
    uint8_t any;                  /* "any format" form (see above) */
    uint8_t pad[16];
};

/* colour combiner: the 16 selector fields of SET_COMBINE (the unused cycle 0 of a one-cycle state is zero),
 * the dither / alpha options of the combiner stage and a few facts derived from the rest of the state */
struct comb_key {
    uint8_t sel[16];
    uint8_t two;
    uint8_t rgbd, ad;             /* RGB dither / alpha dither select */
    uint8_t acmp;                 /* two-cycle and alpha compare enabled (cycle-0 alpha for the compare) */
    uint8_t acs, cta;             /* ALPHA_CVG_SELECT, CVG_TIMES_ALPHA */
    uint8_t shrgb, sha;           /* shade RGB / shade alpha needed (the blender may read the shade alpha) */
    uint8_t wide;                 /* texel values can leave 0..255 (YUV tiles, BI_LERP = 0) */
    uint8_t any;                  /* "any option" form: rgbd, ad, acmp, acs, cta and sha are zero here and read at run time */
    uint8_t pad[6];
};

/* Z compare + colour buffer read. ANTIALIAS_EN and FORCE_BLEND only steer the blend-enable select at the end of the
 * kernel and are read from the state at run time, not part of the key. */
struct zcmp_key {
    uint8_t fbsz;                 /* colour buffer pixel size (2 = 16 bit, 3 = 32 bit) */
    uint8_t fbfmt;                /* colour buffer is RGBA (only with image_read) */
    uint8_t rd;                   /* IMAGE_READ_EN */
    uint8_t zc;                   /* Z_COMPARE_EN */
    uint8_t zmode;                /* Z_MODE (zc only) */
    uint8_t shifts;               /* blender reads the memory alpha (needs the memory delta-Z shifts) */
    uint8_t relaxed;              /* RelaxedPrecision (zc with opaque / transparent mode only) */
    uint8_t pad[25];
};

/* blender */
struct blend_key {
    uint8_t m1a[2], m1b[2], m2a[2], m2b[2];   /* [cycle]; the second entry is zero in one-cycle states */
    uint8_t two;
    uint8_t acmp;                 /* ALPHA_COMPARE_EN */
    uint8_t dal;                  /* DITHER_ALPHA_EN (random threshold) */
    uint8_t aa, force;            /* ANTIALIAS_EN, FORCE_BLEND */
    uint8_t cvgcol;               /* COLOR_ON_CVG */
    uint8_t rgbd;                 /* RGB dither select */
    uint8_t any;                  /* "any option" form: acmp, dal, aa, force, cvgcol and rgbd are zero here and read at run time */
    uint8_t pad[16];
};

/* FB / Z store */
struct store_key {
    uint8_t fbsz, fbfmt;          /* colour buffer size (2 / 3) and RGBA format */
    uint8_t cvgdest;              /* CVG_DEST */
    uint8_t zupd;                 /* Z_UPDATE_EN */
    uint8_t pad[28];
};

/* shape-specialised combiner: cycle type and, per cycle, the shape of the RGB and the alpha equation
 * ((A - B) * C + D). A shape gives each of the four inputs a letter: 0 (zero), 1 (the constant 1.0), COMBINED
 * (the cycle-0 result, cycle 1 of a two-cycle state only) or V (some variable operand, chosen per draw from the
 * operand table). Encoded A | B << 2 | M << 4 | D << 6 with the letters SKL_*; equations that do not depend on
 * A, B or M (M = 0, or A and B the same constant) are canonical "D only": A = B = M = 0. */
enum { SKL_Z = 0, SKL_1 = 1, SKL_C = 2, SKL_V = 3 };
struct shape_key {
    uint8_t two;
    uint8_t ext;                  /* some variable operand can leave 0..255 (wide texels, sharpened LOD fraction): extend them */
    uint8_t sh[2][2];             /* [cycle][0 = RGB, 1 = alpha]; the unused cycle 0 of a one-cycle state is zero */
    uint8_t cm[2][2];             /* split operand mode only: bit p set = variable operand p is a constant (A, B, M, D = bits 0..3) */
    uint8_t pad[20];
};

union spec_key {
    uint8_t raw[32];
    struct shape_key shape;
    struct tex_key tex;
    struct comb_key comb;
    struct zcmp_key zcmp;
    struct blend_key blend;
    struct store_key store;
};

typedef void (*spec_fn)(struct rdp *r, struct px_batch *b);

struct spec_ent {
    union spec_key k;
    spec_fn fn;
    const char *name;
};

struct spec_tab { const struct spec_ent *ent; int n; };

/* tables of the compiled variants (empty in builds without NEON) */
extern struct spec_tab foir_spec_tabs[SPEC_NSTAGES + 1];  /* filled by foir_spec_init(); [SPEC_SHAPE] = the shape kernels */
extern const char *const foir_spec_stage_names[SPEC_NSTAGES];

void foir_spec_init(void);
/* derive the key of every stage from the render state; ok[s] is false when the stage cannot be specialised in this state */
void foir_spec_keys(const struct rdp *r, union spec_key keys[SPEC_NSTAGES], bool ok[SPEC_NSTAGES]);
/* find the variant whose key is exactly k (NULL: none) */
const struct spec_ent *foir_spec_find(int stage, const union spec_key *k);
/* the variant to use for the state key k of a stage: an exact match, else (combine, blend) the "any option" variant
 * of the same selectors; NULL when there is none */
const struct spec_ent *foir_spec_lookup(int stage, const union spec_key *k);
/* the "any option" key of a state key (stages without that form: unchanged) */
void foir_spec_any_key(int stage, union spec_key *k);
/* (re)select the variants for the current render state; fills r->ktab from r->kbase */
void foir_spec_select(struct rdp *r);
/* write the state a key stands for into r (random-state fuzzing; r->pc.tilenum selects the sampled tile) */
void foir_spec_apply(int stage, const union spec_key *k, struct rdp *r);
/* the key as source text for the variant lists (statistics); returns buf */
char *foir_spec_describe(int stage, const union spec_key *k, char *buf, size_t n);

/* ---- shape kernels --------------------------------------------------------------------------------------
 * A shape kernel has the structure of an equation compiled in (which inputs are zero, one, COMBINED or variable)
 * but not its operands: the variable operands are taken from the per-draw operand table r->sk (rdp.h), built by
 * foir_spec_select() from the combiner selectors. It sits between the exact variants and the generic kernel:
 * exact variant (selectors known) -> shape kernel (equation shape known) -> generic kernel. */
enum { COMB_AUTO = 0, COMB_EXACT, COMB_SHAPE, COMB_GENERIC };    /* FOIR_SPEC_COMB=auto|exact|shape|generic (FOIR_SPEC_TEX: the same for the texture unit) */
int  foir_spec_comb_mode(void);
void foir_spec_set_comb_mode(int mode);
int  foir_spec_tex_mode(void);
void foir_spec_set_tex_mode(int mode);
/* the shape key of the combiner in the state of r (false: no shape kernel is possible for it). With tab != NULL also
 * fills the operand table and the splatted constants (kc[], SK_NKC entries) it needs */
bool foir_shape_resolve(const struct rdp *r, union spec_key *key, struct sk_tab *tab, int16_t kc[SK_NKC]);
/* copy the per-draw constants the selected shape kernel needs from the batch of r into another batch (tests) */
void foir_shape_export(const struct rdp *r, struct px_batch *dst);
/* write a state with the given shape into r: random selectors/operands that have this shape (tests) */
void foir_shape_apply(const union spec_key *k, struct rdp *r, uint64_t seed);

/* bits of struct rdp::spec_mask / cumulative coverage counters */
#define SPEC_BIT(s) (1u << (s))

#endif
