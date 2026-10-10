/*
 * mupen64plus-rdp-foir - spec.h
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_SPEC_H
#define FOIR_SPEC_H

#include "rdp.h"

#ifndef FOIR_SK_SPLIT
#define FOIR_SK_SPLIT 0     /* experimental: shape kernels know which variable operands are constants */
#endif

enum { SPEC_TEX, SPEC_COMB, SPEC_ZCMP, SPEC_BLEND, SPEC_STORE, SPEC_NSTAGES };
#define SPEC_SHAPE SPEC_NSTAGES

/* any = 1 ("any format" kernels): fmt*, siz*, tlt and mid are zero and read from the state */
struct tex_key {
    uint8_t two;
    uint8_t t0, t1, nx;           /* TEXEL0, second texture cycle, next-pixel TEXEL1 needed */
    uint8_t fmt0, siz0;
    uint8_t fmt1, siz1;           /* tile + 1; two-cycle with t1 only, else 0 */
    uint8_t smp;
    uint8_t mid;
    uint8_t tlut, tlt;
    uint8_t persp;
    uint8_t bl0, bl1;
    uint8_t lod;                  /* LOD fraction needed (variants: all tiles the LOD can select have tile 0's format) */
    uint8_t lodt;
    uint8_t any;
    uint8_t pad[16];
};

struct comb_key {
    uint8_t sel[16];
    uint8_t two;
    uint8_t rgbd, ad;
    uint8_t acmp;                 /* two-cycle and alpha compare enabled (cycle-0 alpha for the compare) */
    uint8_t acs, cta;
    uint8_t shrgb, sha;
    uint8_t wide;
    uint8_t any;                  /* rgbd, ad, acmp, acs, cta and sha are zero here and read at run time */
    uint8_t pad[6];
};

/* ANTIALIAS_EN and FORCE_BLEND are read from the state at run time, not part of the key */
struct zcmp_key {
    uint8_t fbsz;                 /* 2 = 16 bit, 3 = 32 bit */
    uint8_t fbfmt;                /* colour buffer is RGBA (only with image_read) */
    uint8_t rd;
    uint8_t zc;
    uint8_t zmode;                /* zc only */
    uint8_t shifts;               /* blender reads the memory alpha */
    uint8_t relaxed;              /* zc with opaque / transparent mode only */
    uint8_t pad[25];
};

struct blend_key {
    uint8_t m1a[2], m1b[2], m2a[2], m2b[2];   /* [cycle]; the second entry is zero in one-cycle states */
    uint8_t two;
    uint8_t acmp;
    uint8_t dal;
    uint8_t aa, force;
    uint8_t cvgcol;
    uint8_t rgbd;
    uint8_t any;                  /* acmp, dal, aa, force, cvgcol and rgbd are zero here and read at run time */
    uint8_t pad[16];
};

struct store_key {
    uint8_t fbsz, fbfmt;          /* colour buffer size (2 / 3) and RGBA format */
    uint8_t cvgdest;
    uint8_t zupd;
    uint8_t pad[28];
};

/* per cycle, the shape of the RGB and alpha equation: each of A, B, M, D is a letter 0, 1, COMBINED (cycle 1 of a two-cycle state only) or V (variable operand).
 * Encoded A | B << 2 | M << 4 | D << 6 with SKL_*; equations not depending on A, B or M (M = 0, or A and B the same constant) are canonical "D only": A = B = M = 0 */
enum { SKL_Z = 0, SKL_1 = 1, SKL_C = 2, SKL_V = 3 };
struct shape_key {
    uint8_t two;
    uint8_t ext;                  /* some variable operand can leave 0..255 (wide texels, sharpened LOD fraction) */
    uint8_t sh[2][2];             /* [cycle][0 = RGB, 1 = alpha] */
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

extern struct spec_tab foir_spec_tabs[SPEC_NSTAGES + 1];
extern const char *const foir_spec_stage_names[SPEC_NSTAGES];

void foir_spec_init(void);
/* ok[s] is false when the stage cannot be specialised in this state */
void foir_spec_keys(const struct rdp *r, union spec_key keys[SPEC_NSTAGES], bool ok[SPEC_NSTAGES]);
const struct spec_ent *foir_spec_find(int stage, const union spec_key *k);
/* exact match, else (combine, blend) the "any option" variant of the same selectors; NULL when there is none */
const struct spec_ent *foir_spec_lookup(int stage, const union spec_key *k);
void foir_spec_any_key(int stage, union spec_key *k);
/* fills r->ktab from r->kbase */
void foir_spec_select(struct rdp *r);
/* write the state a key stands for into r (r->pc.tilenum selects the sampled tile) */
void foir_spec_apply(int stage, const union spec_key *k, struct rdp *r);
char *foir_spec_describe(int stage, const union spec_key *k, char *buf, size_t n);

/* exact variant -> shape kernel (operands from the per-draw table r->sk) -> generic kernel */
enum { COMB_AUTO = 0, COMB_EXACT, COMB_SHAPE, COMB_GENERIC };    /* FOIR_SPEC_COMB=auto|exact|shape|generic (FOIR_SPEC_TEX: the same for the texture unit) */
int  foir_spec_comb_mode(void);
void foir_spec_set_comb_mode(int mode);
int  foir_spec_tex_mode(void);
void foir_spec_set_tex_mode(int mode);
/* false: no shape kernel is possible; with tab != NULL also fills the operand table and the splatted constants */
bool foir_shape_resolve(const struct rdp *r, union spec_key *key, struct sk_tab *tab, int16_t kc[SK_NKC]);
void foir_shape_export(const struct rdp *r, struct px_batch *dst);
void foir_shape_apply(const union spec_key *k, struct rdp *r, uint64_t seed);

#define SPEC_BIT(s) (1u << (s))

#endif
