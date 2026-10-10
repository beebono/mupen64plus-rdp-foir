/*
 * mupen64plus-rdp-foir - spec.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include "spec.h"
#include "combreg.h"

const char *const foir_spec_stage_names[SPEC_NSTAGES] = { "texture", "combine", "zcmp", "blend", "store" };

struct spec_tab foir_spec_tabs[SPEC_NSTAGES + 1];

#if defined(FOIR_NEON)
extern const struct spec_ent foir_spec_ents_tex[], foir_spec_ents_comb[], foir_spec_ents_zcmp[], foir_spec_ents_blend[], foir_spec_ents_store[];
extern const struct spec_ent foir_spec_ents_shape[];
extern const int foir_spec_n_tex, foir_spec_n_comb, foir_spec_n_zcmp, foir_spec_n_blend, foir_spec_n_store, foir_spec_n_shape;
#endif

#define HASH_SLOTS 1024    /* per stage, power of two */
static uint16_t hash_tab[SPEC_NSTAGES + 1][HASH_SLOTS];
static bool spec_ready, spec_off, spec_verify;
static int spec_comb_mode = COMB_AUTO, spec_tex_mode = COMB_AUTO;

int foir_spec_comb_mode(void) { return spec_comb_mode; }
void foir_spec_set_comb_mode(int mode) { spec_comb_mode = mode; }
int foir_spec_tex_mode(void) { return spec_tex_mode; }
void foir_spec_set_tex_mode(int mode) { spec_tex_mode = mode; }

static uint32_t key_hash(const union spec_key *k)
{
    uint64_t w[4];
    memcpy(w, k->raw, sizeof(w));
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 4; i++) { h ^= w[i]; h *= 0xff51afd7ed558ccdull; h ^= h >> 29; }
    return (uint32_t)(h >> 7);
}

void foir_spec_init(void)
{
    if (spec_ready) return;
    spec_ready = true;
    const char *e = getenv("FOIR_SPEC");
    if (e && *e == '0') spec_off = true;
    e = getenv("FOIR_NEON_VERIFY");
    if (e && *e == '1') spec_verify = true;
    e = getenv("FOIR_SPEC_COMB");
    if (e && *e) {
        if (!strcmp(e, "auto")) spec_comb_mode = COMB_AUTO;
        else if (!strcmp(e, "exact")) spec_comb_mode = COMB_EXACT;
        else if (!strcmp(e, "shape")) spec_comb_mode = COMB_SHAPE;
        else if (!strcmp(e, "generic")) spec_comb_mode = COMB_GENERIC;
        else fprintf(stderr, "foir: FOIR_SPEC_COMB=%s ignored (auto|exact|shape|generic)\n", e);
    }
    e = getenv("FOIR_SPEC_TEX");
    if (e && *e) {
        if (!strcmp(e, "auto")) spec_tex_mode = COMB_AUTO;
        else if (!strcmp(e, "exact")) spec_tex_mode = COMB_EXACT;
        else if (!strcmp(e, "shape")) spec_tex_mode = COMB_SHAPE;
        else if (!strcmp(e, "generic")) spec_tex_mode = COMB_GENERIC;
        else fprintf(stderr, "foir: FOIR_SPEC_TEX=%s ignored (auto|exact|shape|generic)\n", e);
    }
#if defined(FOIR_NEON)
    foir_spec_tabs[SPEC_TEX]   = (struct spec_tab){ foir_spec_ents_tex, foir_spec_n_tex };
    foir_spec_tabs[SPEC_COMB]  = (struct spec_tab){ foir_spec_ents_comb, foir_spec_n_comb };
    foir_spec_tabs[SPEC_ZCMP]  = (struct spec_tab){ foir_spec_ents_zcmp, foir_spec_n_zcmp };
    foir_spec_tabs[SPEC_BLEND] = (struct spec_tab){ foir_spec_ents_blend, foir_spec_n_blend };
    foir_spec_tabs[SPEC_STORE] = (struct spec_tab){ foir_spec_ents_store, foir_spec_n_store };
    foir_spec_tabs[SPEC_SHAPE] = (struct spec_tab){ foir_spec_ents_shape, foir_spec_n_shape };
#endif
    for (int s = 0; s <= SPEC_NSTAGES; s++) {
        const struct spec_tab *t = &foir_spec_tabs[s];
        const char *sn = s < SPEC_NSTAGES ? foir_spec_stage_names[s] : "shape";
        if (t->n * 2 > HASH_SLOTS) { fprintf(stderr, "foir: too many %s variants\n", sn); abort(); }
        for (int i = 0; i < t->n; i++) {
            uint32_t h = key_hash(&t->ent[i].k) & (HASH_SLOTS - 1);
            while (hash_tab[s][h]) {
                if (!memcmp(t->ent[hash_tab[s][h] - 1].k.raw, t->ent[i].k.raw, 32)) {
                    fprintf(stderr, "foir: duplicate %s variant %s / %s\n", sn, t->ent[hash_tab[s][h] - 1].name, t->ent[i].name);
                    abort();
                }
                h = (h + 1) & (HASH_SLOTS - 1);
            }
            hash_tab[s][h] = (uint16_t)(i + 1);
        }
    }
}

const struct spec_ent *foir_spec_find(int stage, const union spec_key *k)
{
    const struct spec_tab *t = &foir_spec_tabs[stage];
    if (!t->n) return NULL;
    uint32_t h = key_hash(k) & (HASH_SLOTS - 1);
    while (hash_tab[stage][h]) {
        const struct spec_ent *e = &t->ent[hash_tab[stage][h] - 1];
        if (!memcmp(e->k.raw, k->raw, 32)) return e;
        h = (h + 1) & (HASH_SLOTS - 1);
    }
    return NULL;
}

void foir_spec_any_key(int stage, union spec_key *k)
{
    if (stage == SPEC_COMB) {
        struct comb_key *c = &k->comb;
        c->rgbd = c->ad = c->acmp = c->acs = c->cta = c->sha = 0;
        c->any = 1;
    } else if (stage == SPEC_BLEND) {
        struct blend_key *b = &k->blend;
        b->acmp = b->dal = b->aa = b->force = b->cvgcol = b->rgbd = 0;
        b->any = 1;
    } else if (stage == SPEC_TEX) {
        struct tex_key *t = &k->tex;
        t->fmt0 = t->siz0 = t->fmt1 = t->siz1 = t->mid = t->tlt = 0;
        t->any = 1;
    }
}

const struct spec_ent *foir_spec_lookup(int stage, const union spec_key *k)
{
    const struct spec_ent *e = foir_spec_find(stage, k);
    if (e || (stage != SPEC_COMB && stage != SPEC_BLEND && stage != SPEC_TEX)) return e;
    union spec_key a = *k;
    foir_spec_any_key(stage, &a);
    return foir_spec_find(stage, &a);
}

/* texel values can leave 0..255 (YUV tiles, BI_LERP = 0) */
static bool spec_tex_wide(const struct rdp *r)
{
    const struct other_modes *om = &r->om;
    const int two = om->cycle_type == CYCLE_TYPE_2;
    const int tn = r->pc.tilenum & 7;
    const struct tile *tl0 = &r->tile[tn], *tl1 = &r->tile[(tn + 1) & 7];
    bool wide = false;
    if (r->pf.need_tex) {
        wide = tl0->format == FORMAT_YUV || !om->bi_lerp0 ||
               (two && (tl1->format == FORMAT_YUV || !om->bi_lerp1 || om->convert_one));
        if (!wide && om->tex_lod_en)
            for (int t = 0; t < 8; t++)
                if (r->tile[t].format == FORMAT_YUV) wide = true;
    }
    return wide;
}

void foir_spec_keys(const struct rdp *r, union spec_key keys[SPEC_NSTAGES], bool ok[SPEC_NSTAGES])
{
    const struct other_modes *om = &r->om;
    const int two = om->cycle_type == CYCLE_TYPE_2;
    const int tn = r->pc.tilenum & 7;
    const struct tile *tl0 = &r->tile[tn], *tl1 = &r->tile[(tn + 1) & 7];
    memset(keys, 0, sizeof(union spec_key) * SPEC_NSTAGES);

    {
        struct tex_key *k = &keys[SPEC_TEX].tex;
        const int pointplain = !(om->sample_type || om->en_tlut);
        int unsupp = tl0->format == FORMAT_YUV || (!om->bi_lerp0 && !pointplain);
        if (two) unsupp |= tl1->format == FORMAT_YUV || (!om->bi_lerp1 && !pointplain) || om->convert_one;
        ok[SPEC_TEX] = r->pf.need_tex && !unsupp && (r->pf.need_t0 || r->pf.need_t1 || r->pf.next_texel || om->dolod);
        k->two = (uint8_t)two;
        k->t0 = r->pf.need_t0; k->t1 = r->pf.need_t1; k->nx = r->pf.next_texel;
        k->fmt0 = (uint8_t)tl0->format; k->siz0 = (uint8_t)tl0->size;
        if (two && r->pf.need_t1) { k->fmt1 = (uint8_t)tl1->format; k->siz1 = (uint8_t)tl1->size; }
        k->smp = (uint8_t)om->sample_type;
        k->mid = (uint8_t)(om->sample_type ? om->mid_texel : 0);
        k->tlut = (uint8_t)om->en_tlut;
        k->tlt = (uint8_t)(om->en_tlut ? om->tlut_type : 0);
        k->persp = (uint8_t)om->persp_tex_en;
        k->bl0 = (uint8_t)om->bi_lerp0;
        k->bl1 = (uint8_t)(two ? om->bi_lerp1 : 0);
        k->lod = (uint8_t)om->dolod;
        k->lodt = (uint8_t)om->tex_lod_en;
    }
    {
        struct comb_key *k = &keys[SPEC_COMB].comb;
        ok[SPEC_COMB] = !om->key_en;
        for (int i = two ? 0 : 8; i < 16; i++) k->sel[i] = (uint8_t)r->comb.raw[i];
        k->two = (uint8_t)two;
        k->rgbd = (uint8_t)om->rgb_dither_sel; k->ad = (uint8_t)om->alpha_dither_sel;
        k->acmp = (uint8_t)(two && om->alpha_compare_en);
        k->acs = (uint8_t)om->alpha_cvg_select; k->cta = (uint8_t)om->cvg_times_alpha;
        k->shrgb = r->pf.shade_rgb; k->sha = (uint8_t)(r->pf.shade_a || r->pf.shade_rgb);
        k->wide = (uint8_t)spec_tex_wide(r);
    }
    {
        struct zcmp_key *k = &keys[SPEC_ZCMP].zcmp;
        ok[SPEC_ZCMP] = r->fb_size >= PIXEL_SIZE_16BIT;
        k->fbsz = (uint8_t)r->fb_size;
        k->rd = (uint8_t)om->image_read_en;
        k->fbfmt = (uint8_t)(om->image_read_en && r->fb_format == FORMAT_RGBA);
        k->zc = (uint8_t)om->z_compare_en;
        k->zmode = (uint8_t)(om->z_compare_en ? om->z_mode : 0);
        k->shifts = (uint8_t)om->real_blendshifts;
        k->relaxed = (uint8_t)((om->z_compare_en && (om->z_mode == ZMODE_OPAQUE || om->z_mode == ZMODE_TRANSPARENT))
                               ? r->cfg.relaxed_precision : 0);
    }
    {
        struct blend_key *k = &keys[SPEC_BLEND].blend;
        ok[SPEC_BLEND] = true;
        for (int c = 0; c < (two ? 2 : 1); c++) {
            k->m1a[c] = (uint8_t)om->blend_m1a[c]; k->m1b[c] = (uint8_t)om->blend_m1b[c];
            k->m2a[c] = (uint8_t)om->blend_m2a[c]; k->m2b[c] = (uint8_t)om->blend_m2b[c];
        }
        k->two = (uint8_t)two;
        k->acmp = (uint8_t)om->alpha_compare_en;
        k->dal = (uint8_t)(om->alpha_compare_en ? om->dither_alpha_en : 0);
        k->aa = (uint8_t)om->antialias_en; k->force = (uint8_t)om->force_blend;
        k->cvgcol = (uint8_t)om->color_on_cvg;
        k->rgbd = (uint8_t)om->rgb_dither_sel;
    }
    {
        struct store_key *k = &keys[SPEC_STORE].store;
        ok[SPEC_STORE] = r->fb_size >= PIXEL_SIZE_16BIT;
        k->fbsz = (uint8_t)r->fb_size;
        k->fbfmt = (uint8_t)(r->fb_size == PIXEL_SIZE_16BIT && r->fb_format == FORMAT_RGBA);
        k->cvgdest = (uint8_t)om->cvg_dest;
        k->zupd = (uint8_t)om->z_update_en;
    }
}

static const struct spec_ent *shape_pick(struct rdp *r);

void foir_spec_select(struct rdp *r)
{
    r->ktab = r->kbase;
    r->spec_mask = 0;
    r->spec_dirty = false;
    r->spec_tile = r->pc.tilenum;
    r->spec_comb_shape = false;
    r->spec_tex_any = false;
    for (int s = 0; s < SPEC_NSTAGES; s++) r->spec_cur[s] = NULL;
    if (spec_off || !r->cfg.specialize || !r->spec_ok_mask) return;

    union spec_key keys[SPEC_NSTAGES];
    bool ok[SPEC_NSTAGES];
    foir_spec_keys(r, keys, ok);
    for (int s = 0; s < SPEC_NSTAGES; s++) {
        if (!ok[s] || !(r->spec_ok_mask & SPEC_BIT(s))) continue;
        const struct spec_ent *e = NULL;
        if (s == SPEC_COMB) {
            /* exact variant -> shape kernel -> generic kernel (FOIR_SPEC_COMB restricts the choice) */
            if (spec_comb_mode == COMB_AUTO || spec_comb_mode == COMB_EXACT) e = foir_spec_lookup(s, &keys[s]);
            if (!e && (spec_comb_mode == COMB_AUTO || spec_comb_mode == COMB_SHAPE) && (e = shape_pick(r)) != NULL)
                r->spec_comb_shape = true;
        } else if (s == SPEC_TEX) {
            /* exact variant -> "any format" variant -> generic kernel (FOIR_SPEC_TEX restricts the choice) */
            if (spec_tex_mode == COMB_AUTO || spec_tex_mode == COMB_EXACT) e = foir_spec_find(s, &keys[s]);
            if (!e && (spec_tex_mode == COMB_AUTO || spec_tex_mode == COMB_SHAPE)) {
                union spec_key a = keys[s];
                foir_spec_any_key(s, &a);
                e = foir_spec_find(s, &a);
            }
        } else {
            e = foir_spec_lookup(s, &keys[s]);
        }
        if (!e) continue;
        if (s == SPEC_TEX) r->spec_tex_any = e->k.tex.any != 0;
        r->spec_cur[s] = e->fn;
        r->spec_mask |= SPEC_BIT(s);
        if (spec_verify) continue;      /* the verifying wrappers call the variant themselves */
        switch (s) {
        case SPEC_TEX:   r->ktab.texture = e->fn; break;
        case SPEC_COMB:  r->ktab.combine = e->fn; break;
        case SPEC_ZCMP:  r->ktab.zcmp = e->fn; break;
        case SPEC_BLEND: r->ktab.blend = e->fn; break;
        default:         r->ktab.store = e->fn; break;
        }
    }
}

#define SKO(field) offsetof(struct px_batch, field)
#define SKROW ((unsigned)(FOIR_LANES * sizeof(int16_t)))

/* COMBINED only exists in cycle 1 of a two-cycle state (else it reads zero); an RGB equation can read COMBINED alpha, which is not "its" channel: variable (c0a array) */
static int sk_letter(int reg, int ch, bool combact)
{
    if (reg == REG_ZERO) return SKL_Z;
    if (reg == REG_ONE) return SKL_1;
    if (reg >= REG_COMB && reg < REG_COMB + 4) {
        if (!combact) return SKL_Z;
        return reg == REG_COMB + ch ? SKL_C : SKL_V;
    }
    return SKL_V;
}

static uint8_t sk_canon(int la, int lb, int lm, int ld)
{
    if (lm == SKL_Z || (la == lb && la != SKL_V)) la = lb = lm = SKL_Z;
    return (uint8_t)(la | lb << 2 | lm << 4 | ld << 6);
}

static int16_t sk_ext9(int v)  { v &= 0x1ff; return (int16_t)((v & 0x180) == 0x180 ? v - 0x200 : v); }
static int16_t sk_sext9(int v) { v &= 0x1ff; return (int16_t)((v & 0x100) ? v - 0x200 : v); }

static int sk_constval(const struct rdp *r, int reg)
{
    switch (reg) {
    case REG_PRIM + 0: return r->prim_color.r;  case REG_PRIM + 1: return r->prim_color.g;
    case REG_PRIM + 2: return r->prim_color.b;  case REG_PRIM + 3: return r->prim_color.a;
    case REG_ENV + 0:  return r->env_color.r;   case REG_ENV + 1:  return r->env_color.g;
    case REG_ENV + 2:  return r->env_color.b;   case REG_ENV + 3:  return r->env_color.a;
    case REG_KEYC + 0: return r->key_center.r;  case REG_KEYC + 1: return r->key_center.g;  case REG_KEYC + 2: return r->key_center.b;
    case REG_KEYS + 0: return r->key_scale.r;   case REG_KEYS + 1: return r->key_scale.g;   case REG_KEYS + 2: return r->key_scale.b;
    case REG_K4: return r->k4;
    case REG_K5: return r->k5;
    case REG_PLODF: return r->primitive_lod_frac;
    default: return -1;
    }
}

struct sk_build {
    struct sk_tab t;
    int16_t kv[SK_NKC];
    int kreg[SK_NKC];
    int nk;
    bool texv, lodfm;
    uint8_t cm[2][2];
};

static bool sk_slot(const struct rdp *r, struct sk_build *B, int cy, bool two, int pos, int ch, int reg, int half)
{
    const bool c1 = two && cy == 1;
    unsigned off;
    if (reg >= REG_TEX0 && reg < REG_TEX0 + 4) {
        off = (c1 ? SKO(tex1) : SKO(tex0)) + (unsigned)(reg - REG_TEX0) * SKROW; B->texv = true;
    } else if (reg >= REG_TEX1 && reg < REG_TEX1 + 4) {
        off = (c1 ? SKO(tex0n) : SKO(tex1)) + (unsigned)(reg - REG_TEX1) * SKROW; B->texv = true;
    } else if (reg >= REG_SHADE && reg < REG_SHADE + 4) {
        off = SKO(shade) + (unsigned)(reg - REG_SHADE) * SKROW;
    } else if (reg == REG_NOISE) {
        off = SKO(noise_s); B->t.noise = 1;
    } else if (reg == REG_LODF) {
        off = SKO(lodfrac); B->lodfm = true;
    } else if (reg == REG_COMB + 3 && c1) {
        off = SKO(c0a); B->t.c0a = 1;
    } else {
        int v = sk_constval(r, reg);
        if (v < 0) return false;
        const int form = pos == 2;            /* multiply input: sign extended, else the A/B/D extension */
#if FOIR_SK_SPLIT
        B->cm[cy][half] |= (uint8_t)(1 << pos);
        B->t.off[cy][pos][ch] = (uint16_t)(form ? sk_sext9(v) : sk_ext9(v));
        return true;
#endif
        int i;
        for (i = 0; i < B->nk; i++) if (B->kreg[i] == reg * 2 + form) break;
        if (i == B->nk) {
            if (B->nk >= SK_NKC) return false;
            B->kreg[i] = reg * 2 + form;
            B->kv[i] = form ? sk_sext9(v) : sk_ext9(v);
            B->nk++;
        }
        off = SKO(kc) + (unsigned)i * SKROW;
        B->cm[cy][half] |= (uint8_t)(1 << pos);
    }
    B->t.off[cy][pos][ch] = (uint16_t)off;
    return true;
}

bool foir_shape_resolve(const struct rdp *r, union spec_key *key, struct sk_tab *tab, int16_t kc[SK_NKC])
{
    const struct other_modes *om = &r->om;
    const bool two = om->cycle_type == CYCLE_TYPE_2;
    struct shape_key *k = &key->shape;
    struct sk_build B;
    memset(key, 0, sizeof *key);
    memset(&B, 0, sizeof B);
    k->two = (uint8_t)two;
    for (int cy = two ? 0 : 1; cy < 2; cy++) {
        const bool combact = two && cy == 1;
        for (int half = 0; half < 2; half++) {
            const int nch = half ? 1 : 3;
            int L[4];
            for (int pos = 0; pos < 4; pos++)
                L[pos] = sk_letter(half ? r->comb.alpha[cy][pos] : r->comb.rgb[cy][pos][0], half ? 3 : 0, combact);
            const uint8_t sh = sk_canon(L[0], L[1], L[2], L[3]);
            k->sh[cy][half] = sh;
            for (int pos = 0; pos < 4; pos++) {
                if (((sh >> (pos * 2)) & 3) != SKL_V) continue;
                for (int c = 0; c < nch; c++) {
                    const int ch = half ? 3 : c;
                    const int reg = half ? r->comb.alpha[cy][pos] : r->comb.rgb[cy][pos][c];
                    if (!sk_slot(r, &B, cy, two, pos, ch, reg, half)) return false;
                }
            }
        }
    }
    k->ext = (uint8_t)((B.texv && spec_tex_wide(r)) || (B.lodfm && om->sharpen_tex_en));
#if FOIR_SK_SPLIT
    memcpy(k->cm, B.cm, sizeof k->cm);
#endif
    if (tab) {
        B.t.sh_rgb = r->pf.shade_rgb;
        B.t.sh_a = (uint8_t)(r->pf.shade_a || r->pf.shade_rgb);
        B.t.nkc = (uint8_t)B.nk;
        *tab = B.t;
        for (int i = 0; i < B.nk; i++) kc[i] = B.kv[i];
    }
    return true;
}

static const struct spec_ent *shape_pick(struct rdp *r)
{
    if (!foir_spec_tabs[SPEC_SHAPE].n) return NULL;
    union spec_key k;
    struct sk_tab t;
    int16_t kc[SK_NKC];
    if (!foir_shape_resolve(r, &k, &t, kc)) return NULL;
    const struct spec_ent *e = foir_spec_find(SPEC_SHAPE, &k);
    if (!e) return NULL;
    r->sk = t;
    for (int i = 0; i < t.nkc; i++)
        for (int l = 0; l < FOIR_LANES; l++) r->batch.kc[i][l] = kc[i];
    return e;
}

void foir_shape_export(const struct rdp *r, struct px_batch *dst)
{
    memcpy(dst->kc, r->batch.kc, sizeof(dst->kc));
}

static void set_tile_fs(struct rdp *r, int t, int fmt, int siz)
{
    r->tile[t & 7].format = fmt;
    r->tile[t & 7].size = siz;
    tile_update_derived(&r->tile[t & 7]);
}

void foir_spec_apply(int stage, const union spec_key *key, struct rdp *r)
{
    struct other_modes *om = &r->om;
    const int tn = r->pc.tilenum & 7;
    switch (stage) {
    case SPEC_TEX: {
        const struct tex_key *k = &key->tex;
        om->cycle_type = k->two ? CYCLE_TYPE_2 : CYCLE_TYPE_1;
        om->sample_type = k->smp; om->en_tlut = k->tlut;
        if (!k->any) { om->tlut_type = k->tlt; om->mid_texel = k->mid; }       /* any format: the state keeps whatever it had */
        om->persp_tex_en = k->persp; om->bi_lerp0 = k->bl0; om->bi_lerp1 = k->bl1; om->convert_one = 0;
        om->tex_lod_en = k->lodt; om->detail_tex_en = om->sharpen_tex_en = 0;
        if (k->any) {
            /* any format: the tiles keep the (random) formats they have, except YUV, which the texture unit does not handle */
            for (int t = 0; t < 8; t++)
                if (r->tile[t].format == FORMAT_YUV) set_tile_fs(r, t, FORMAT_RGBA, r->tile[t].size);
        } else {
            set_tile_fs(r, tn, k->fmt0, k->siz0);
            if (k->lod)                      /* the tiles the LOD can select all have the format of tile 0 */
                for (int t = 0; t < 8; t++) set_tile_fs(r, t, k->fmt0, k->siz0);
            if (k->two && k->t1) set_tile_fs(r, tn + 1, k->fmt1, k->siz1);
            else if (k->two && r->tile[(tn + 1) & 7].format == FORMAT_YUV) set_tile_fs(r, tn + 1, FORMAT_RGBA, r->tile[(tn + 1) & 7].size);
        }
        int *c = r->comb.raw;
        for (int i = 0; i < 16; i++) c[i] = 0;
        for (int cy = 0; cy < 2; cy++) {
            c[cy * 8 + 0] = 8; c[cy * 8 + 1] = 8; c[cy * 8 + 2] = 0x1f; c[cy * 8 + 3] = 7;
            c[cy * 8 + 4] = 7; c[cy * 8 + 5] = 7; c[cy * 8 + 6] = 7; c[cy * 8 + 7] = 7;
        }
        if (!k->two) {
            if (k->t0) c[8 + 0] = 1;                 /* sub a = TEXEL0 */
            if (k->nx) c[8 + 2] = 2;                 /* mul = TEXEL1 (next pixel's TEXEL0) */
            if (k->lod && !k->lodt) c[8 + 6] = 0;    /* alpha mul = LOD fraction */
        } else {
            if (k->t0) c[0] = 1;                     /* cycle 0: TEXEL0 */
            if (k->t1) c[8 + 0] = 1;                 /* cycle 1: TEXEL0 = the second texture cycle */
            if (k->nx) c[8 + 2] = 2;
            if (k->lod && !k->lodt) c[8 + 6] = 0;
        }
        break;
    }
    case SPEC_COMB: {
        const struct comb_key *k = &key->comb;
        for (int i = 0; i < 16; i++) r->comb.raw[i] = k->sel[i];
        om->cycle_type = k->two ? CYCLE_TYPE_2 : CYCLE_TYPE_1;
        om->key_en = 0;
        if (!k->any) {
            om->rgb_dither_sel = k->rgbd; om->alpha_dither_sel = k->ad;
            om->alpha_compare_en = k->acmp;
            om->alpha_cvg_select = k->acs; om->cvg_times_alpha = k->cta;
            /* shade alpha needed only by the blender: select it as the blender's shade alpha input */
            if (k->sha) om->blend_m1b[0] = 2;
            else for (int c = 0; c < 2; c++) if (om->blend_m1b[c] == 2) om->blend_m1b[c] = 3;
        }
        if (k->wide) {
            om->bi_lerp0 = 0;
        } else {
            om->bi_lerp0 = om->bi_lerp1 = 1; om->convert_one = 0;
            for (int t = 0; t < 8; t++) if (r->tile[t].format == FORMAT_YUV) set_tile_fs(r, t, FORMAT_RGBA, r->tile[t].size);
        }
        break;
    }
    case SPEC_ZCMP: {
        const struct zcmp_key *k = &key->zcmp;
        r->fb_size = k->fbsz;
        om->image_read_en = k->rd;
        if (k->rd) r->fb_format = k->fbfmt ? FORMAT_RGBA : FORMAT_IA;
        om->z_compare_en = k->zc; om->z_mode = k->zmode;
        r->cfg.relaxed_precision = k->relaxed;
        /* real_blendshifts: blender uses the memory alpha */
        if (k->shifts) om->blend_m2b[0] = 1;
        else for (int c = 0; c < 2; c++) if (om->blend_m2b[c] == 1) om->blend_m2b[c] = 2;
        break;
    }
    case SPEC_BLEND: {
        const struct blend_key *k = &key->blend;
        om->cycle_type = k->two ? CYCLE_TYPE_2 : CYCLE_TYPE_1;
        for (int c = 0; c < 2; c++) {
            om->blend_m1a[c] = k->m1a[c]; om->blend_m1b[c] = k->m1b[c];
            om->blend_m2a[c] = k->m2a[c]; om->blend_m2b[c] = k->m2b[c];
        }
        if (!k->any) {
            om->alpha_compare_en = k->acmp; om->dither_alpha_en = k->dal;
            om->antialias_en = k->aa; om->force_blend = k->force; om->color_on_cvg = k->cvgcol;
            om->rgb_dither_sel = k->rgbd;
        }
        break;
    }
    case SPEC_STORE: {
        const struct store_key *k = &key->store;
        r->fb_size = k->fbsz;
        if (k->fbsz == PIXEL_SIZE_16BIT) r->fb_format = k->fbfmt ? FORMAT_RGBA : FORMAT_IA;
        om->cvg_dest = k->cvgdest; om->z_update_en = k->zupd;
        break;
    }
    }
}


static uint32_t sk_rnd(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return (uint32_t)((x * 0x2545f4914f6cdd1dull) >> 33);
}

static bool sk_isconst(int reg) { return reg == REG_K4 || reg == REG_K5 || reg == REG_PLODF || (reg >= REG_PRIM && reg < REG_PRIM + 4) ||
                                         (reg >= REG_ENV && reg < REG_ENV + 4) || (reg >= REG_KEYC && reg < REG_KEYC + 6); }

/* cmode: -1 any variable operand, 0 only per-pixel ones, 1 only constants (the split-operand build keys on that) */
static int sk_sels(int half, int pos, int letter, bool combact, int *out, int cmode)
{
    const int nsel = half ? 8 : (pos == 2 ? 32 : pos == 3 ? 8 : 16);
    int n = 0;
    for (int sel = 0; sel < nsel; sel++) {
        const int reg = half ? comb_reg_alpha(pos, sel) : comb_reg_rgb(pos, sel, 0);
        if (sk_letter(reg, half ? 3 : 0, combact) != letter) continue;
        if (letter == SKL_V && cmode >= 0 && sk_isconst(reg) != (cmode == 1)) continue;
        out[n++] = sel;
    }
    return n;
}

static int sk_pick(uint64_t *rs, int half, int pos, int letter, bool combact, int cmode)
{
    int sels[32];
    int n = sk_sels(half, pos, letter, combact, sels, cmode);
    return n ? sels[sk_rnd(rs) % (unsigned)n] : 0;
}

static int sk_pick_any(uint64_t *rs, int half, int pos)
{
    const int nsel = half ? 8 : (pos == 2 ? 32 : pos == 3 ? 8 : 16);
    return (int)(sk_rnd(rs) % (unsigned)nsel);
}

void foir_shape_apply(const union spec_key *key, struct rdp *r, uint64_t seed)
{
    const struct shape_key *k = &key->shape;
    struct other_modes *om = &r->om;
    uint64_t rs = seed * 0x9e3779b97f4a7c15ull + 0x1234567ull;
    for (int i = 0; i < 4; i++) sk_rnd(&rs);
    const bool two = k->two;
    om->cycle_type = two ? CYCLE_TYPE_2 : CYCLE_TYPE_1;
    om->key_en = 0;
    int *c = r->comb.raw;
    if (!two) for (int half = 0; half < 2; half++) for (int pos = 0; pos < 4; pos++) c[half * 4 + pos] = sk_pick_any(&rs, half, pos);
    int mslot[2][2] = { { 0, 0 }, { 0, 0 } };
    int vany[2][2][4] = { { { 0 } } };
    for (int cy = two ? 0 : 1; cy < 2; cy++) {
        const bool combact = two && cy == 1;
        for (int half = 0; half < 2; half++) {
            const uint8_t sh = k->sh[cy][half];
            int sel[4];
            int L[4] = { sh & 3, (sh >> 2) & 3, (sh >> 4) & 3, (sh >> 6) & 3 };
            if (L[0] == SKL_Z && L[1] == SKL_Z && L[2] == SKL_Z) {
                /* "D only": either the multiply input is zero (anything for A and B), or A and B are the same constant */
                int pairs[3], np = 0;
                for (int l = SKL_Z; l <= SKL_C; l++) {
                    int tmp[32];
                    if (sk_sels(half, 0, l, combact, tmp, -1) && sk_sels(half, 1, l, combact, tmp, -1)) pairs[np++] = l;
                }
                if (np && (sk_rnd(&rs) & 1)) {
                    int l = pairs[sk_rnd(&rs) % (unsigned)np];
                    sel[0] = sk_pick(&rs, half, 0, l, combact, -1); sel[1] = sk_pick(&rs, half, 1, l, combact, -1);
                    sel[2] = sk_pick_any(&rs, half, 2);
                } else {
                    sel[0] = sk_pick_any(&rs, half, 0); sel[1] = sk_pick_any(&rs, half, 1);
                    sel[2] = sk_pick(&rs, half, 2, SKL_Z, combact, -1);
                }
                sel[3] = sk_pick(&rs, half, 3, L[3], combact, FOIR_SK_SPLIT ? (k->cm[cy][half] >> 3) & 1 : -1);
            } else {
                for (int pos = 0; pos < 4; pos++) sel[pos] = sk_pick(&rs, half, pos, L[pos], combact, FOIR_SK_SPLIT ? (k->cm[cy][half] >> pos) & 1 : -1);
            }
            for (int pos = 0; pos < 4; pos++) {
                c[cy * 8 + half * 4 + pos] = sel[pos];
                vany[cy][half][pos] = L[pos] == SKL_V;
            }
            mslot[cy][half] = L[2] == SKL_V;
        }
    }
    bool routed = false;
    if (k->ext) {
        /* either sharpened LOD fraction in a multiply input, or wide texels in some variable operand */
        int cand[16][3], nc = 0;
        for (int cy = two ? 0 : 1; cy < 2; cy++) for (int half = 0; half < 2; half++) for (int pos = 0; pos < 4; pos++)
            if (vany[cy][half][pos] && !(FOIR_SK_SPLIT && ((k->cm[cy][half] >> pos) & 1))) { cand[nc][0] = cy; cand[nc][1] = half; cand[nc][2] = pos; nc++; }
        for (int cy = two ? 0 : 1; cy < 2 && !routed; cy++) for (int half = 0; half < 2 && !routed; half++)
            if (mslot[cy][half] && !(FOIR_SK_SPLIT && ((k->cm[cy][half] >> 2) & 1)) && (sk_rnd(&rs) & 1)) {
                c[cy * 8 + half * 4 + 2] = half ? 0 : 13;       /* LOD fraction (alpha: sel 0, RGB: sel 13) */
                om->sharpen_tex_en = 1; om->bi_lerp0 = om->bi_lerp1 = 1; om->convert_one = 0;
                routed = true;
            }
        if (!routed && nc) {
            int i = (int)(sk_rnd(&rs) % (unsigned)nc);
            int cy = cand[i][0], half = cand[i][1], pos = cand[i][2];
            int ts[8], nt = 0;
            for (int sel = 0; sel < 32; sel++) {
                const int nsel = half ? 8 : (pos == 2 ? 32 : pos == 3 ? 8 : 16);
                if (sel >= nsel) break;
                const int reg = half ? comb_reg_alpha(pos, sel) : comb_reg_rgb(pos, sel, 0);
                if ((reg >= REG_TEX0 && reg < REG_TEX1 + 4) && nt < 8) ts[nt++] = sel;
            }
            if (nt) {
                c[cy * 8 + half * 4 + pos] = ts[sk_rnd(&rs) % (unsigned)nt];
                om->bi_lerp0 = 0; om->sharpen_tex_en = 0;
                routed = true;
            }
        }
    }
    if (!k->ext || !routed) {
        om->bi_lerp0 = om->bi_lerp1 = 1; om->convert_one = 0;
        for (int t = 0; t < 8; t++) if (r->tile[t].format == FORMAT_YUV) set_tile_fs(r, t, FORMAT_RGBA, r->tile[t].size);
        if (!k->ext) om->sharpen_tex_en = 0;
    }
}

char *foir_spec_describe(int stage, const union spec_key *key, char *buf, size_t n)
{
    if (key->raw[0] == 0xff && key->raw[2] == 0xff) {      /* the statistics' bucket of states the stage handles generically */
        static const char *why[] = { "?", "TEX_LOD_EN", "LOD fraction used", "YUV tile", "BI_LERP=0 with filter / TLUT", "two-cycle with convert / YUV tile 1", "other", "chroma key", "4 / 8-bit colour buffer" };
        snprintf(buf, n, "GENERIC(255, %s)", key->raw[1] < 9 ? why[key->raw[1]] : "?");
        return buf;
    }
    if (stage == SPEC_SHAPE) {
        const struct shape_key *k = &key->shape;
        char let[2][2][5];
        for (int cy = 0; cy < 2; cy++) for (int h = 0; h < 2; h++) {
            for (int p = 0; p < 4; p++) let[cy][h][p] = "01CV"[(k->sh[cy][h] >> (p * 2)) & 3];
            let[cy][h][4] = 0;
        }
        if (k->two) snprintf(buf, n, "SHAPE(1, 0x%02x, 0x%02x, 0x%02x, 0x%02x, %d)  /* %s %s | %s %s */", k->sh[0][0], k->sh[0][1], k->sh[1][0], k->sh[1][1], k->ext,
                             let[0][0], let[0][1], let[1][0], let[1][1]);
        else snprintf(buf, n, "SHAPE(0, 0x%02x, 0x%02x, 0x%02x, 0x%02x, %d)  /* %s %s */", k->sh[0][0], k->sh[0][1], k->sh[1][0], k->sh[1][1], k->ext, let[1][0], let[1][1]);
        return buf;
    }
    switch (stage) {
    case SPEC_TEX: {
        const struct tex_key *k = &key->tex;
        if (k->any) {
            if (k->lod) snprintf(buf, n, "TEXAL(%d, %d,%d,%d, %d, %d, %d, %d,%d, %d)", k->two, k->t0, k->t1, k->nx, k->smp, k->tlut, k->persp, k->bl0, k->bl1, k->lodt);
            else snprintf(buf, n, "TEXA(%d, %d,%d,%d, %d, %d, %d, %d,%d)", k->two, k->t0, k->t1, k->nx, k->smp, k->tlut, k->persp, k->bl0, k->bl1);
        } else if (k->lod) {
            snprintf(buf, n, "TEXL(%d, %d,%d,%d, %d,%d, %d,%d, %d,%d, %d,%d, %d, %d,%d, %d)", k->two, k->t0, k->t1, k->nx, k->fmt0, k->siz0,
                     k->fmt1, k->siz1, k->smp, k->mid, k->tlut, k->tlt, k->persp, k->bl0, k->bl1, k->lodt);
        } else {
            snprintf(buf, n, "TEX(%d, %d,%d,%d, %d,%d, %d,%d, %d,%d, %d,%d, %d, %d,%d)", k->two, k->t0, k->t1, k->nx, k->fmt0, k->siz0,
                     k->fmt1, k->siz1, k->smp, k->mid, k->tlut, k->tlt, k->persp, k->bl0, k->bl1);
        }
        break;
    }
    case SPEC_COMB: {
        const struct comb_key *k = &key->comb;
        const uint8_t *c = k->sel;
        uint32_t w0 = (uint32_t)c[0] << 20 | (uint32_t)c[2] << 15 | (uint32_t)c[4] << 12 | (uint32_t)c[6] << 9 | (uint32_t)c[8] << 5 | c[10];
        uint32_t w1 = (uint32_t)c[1] << 28 | (uint32_t)c[9] << 24 | (uint32_t)c[12] << 21 | (uint32_t)c[14] << 18 | (uint32_t)c[3] << 15 |
                      (uint32_t)c[5] << 12 | (uint32_t)c[7] << 9 | (uint32_t)c[11] << 6 | (uint32_t)c[13] << 3 | c[15];
        if (k->any) snprintf(buf, n, "COMBX(%d, 0x%06x, 0x%08x, %d, %d)", k->two, w0, w1, k->shrgb, k->wide);
        else snprintf(buf, n, "COMB(%d, 0x%06x, 0x%08x, %d,%d, %d,%d,%d, %d,%d, %d)", k->two, w0, w1, k->rgbd, k->ad, k->acmp, k->acs, k->cta,
                      k->shrgb, k->sha, k->wide);
        break;
    }
    case SPEC_ZCMP: {
        const struct zcmp_key *k = &key->zcmp;
        snprintf(buf, n, "ZCMP(%d, %d,%d, %d,%d, %d, %d)", k->fbsz, k->rd, k->fbfmt, k->zc, k->zmode, k->shifts, k->relaxed);
        break;
    }
    case SPEC_BLEND: {
        const struct blend_key *k = &key->blend;
        if (k->any) snprintf(buf, n, "BLENDX(%d, %d,%d,%d,%d, %d,%d,%d,%d)", k->two, k->m1a[0], k->m1b[0], k->m2a[0], k->m2b[0],
                             k->m1a[1], k->m1b[1], k->m2a[1], k->m2b[1]);
        else snprintf(buf, n, "BLEND(%d, %d,%d,%d,%d, %d,%d,%d,%d, %d,%d, %d,%d, %d, %d)", k->two, k->m1a[0], k->m1b[0], k->m2a[0], k->m2b[0],
                      k->m1a[1], k->m1b[1], k->m2a[1], k->m2b[1], k->acmp, k->dal, k->aa, k->force, k->cvgcol, k->rgbd);
        break;
    }
    default: {
        const struct store_key *k = &key->store;
        snprintf(buf, n, "STORE(%d, %d, %d, %d)", k->fbsz, k->fbfmt, k->cvgdest, k->zupd);
        break;
    }
    }
    return buf;
}
