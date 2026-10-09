/*
 * mupen64plus-rdp-foir - stats.c
 * Debug statistics (build with -DFOIR_STATS, enable with FOIR_STATE_STATS=1):
 * how many pixels are rendered under each distinct pipeline state. Used to judge
 * how much a static specialisation of the common states (M3) could cover.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"
#include "spec.h"

#ifdef FOIR_STATS
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct stat_ent { uint64_t key; uint64_t pixels, batches; int cycle, tex, z; char desc[160]; };
static struct stat_ent tab[2048];
static int nent, enabled = -1;

/* per-stage specialisation keys: pixels per distinct stage key and whether a variant exists for it */
struct skey_ent { union spec_key k; uint64_t pixels; bool covered; bool exact, shape; };   /* exact / shape: the combine stage has an exact variant / a shape kernel for it */
static struct skey_ent stab[SPEC_NSTAGES][512];
static int sn[SPEC_NSTAGES];
static uint64_t spix[SPEC_NSTAGES], scov[SPEC_NSTAGES];
static uint64_t cpix_exact, cpix_shape, cpix_shape_only;
static uint64_t tpix_exact, tpix_any, tpix_any_only;       /* texture pixels with an exact variant / an "any format" variant / an any-format variant but no exact one */     /* combine pixels with an exact variant / a shape kernel / a shape kernel but no exact variant */

static int scmp(const void *a, const void *b)
{
    const struct skey_ent *x = a, *y = b;
    return x->pixels < y->pixels ? 1 : (x->pixels > y->pixels ? -1 : 0);
}

static FILE *out;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

static void dump_stage_keys(void)
{
    const char *all = getenv("FOIR_STATE_STATS_ALL");
    for (int s = 0; s < SPEC_NSTAGES; s++) {
        if (!spix[s]) continue;
        qsort(stab[s], (size_t)sn[s], sizeof(stab[s][0]), scmp);
        fprintf(out, "stage %-7s: %d distinct keys, %llu pixels, %.1f%% in a specialised variant", foir_spec_stage_names[s], sn[s],
                (unsigned long long)spix[s], 100.0 * (double)scov[s] / (double)spix[s]);
        if (s == SPEC_TEX)
            fprintf(out, " (exact variant %.1f%%, any-format variant %.1f%% of which %.1f%% without an exact variant, neither %.1f%%; FOIR_SPEC_TEX mode %d)",
                    100.0 * (double)tpix_exact / (double)spix[s], 100.0 * (double)tpix_any / (double)spix[s],
                    100.0 * (double)tpix_any_only / (double)spix[s], 100.0 * (double)(spix[s] - tpix_exact - tpix_any_only) / (double)spix[s],
                    foir_spec_tex_mode());
        if (s == SPEC_COMB)
            fprintf(out, " (exact variant %.1f%%, shape kernel %.1f%% of which %.1f%% without an exact variant, neither %.1f%%; FOIR_SPEC_COMB mode %d)",
                    100.0 * (double)cpix_exact / (double)spix[s], 100.0 * (double)cpix_shape / (double)spix[s],
                    100.0 * (double)cpix_shape_only / (double)spix[s], 100.0 * (double)(spix[s] - cpix_exact - cpix_shape_only) / (double)spix[s],
                    foir_spec_comb_mode());
        fprintf(out, "\n");
        uint64_t cum = 0;
        for (int i = 0; i < sn[s] && (all || i < 12); i++) {
            char buf[200];
            cum += stab[s][i].pixels;
            fprintf(out, "  %c %5.1f%% (cum %5.1f%%)  %s\n", (s == SPEC_COMB || s == SPEC_TEX) ? (stab[s][i].exact ? '*' : stab[s][i].shape ? '+' : ' ') : stab[s][i].covered ? '*' : ' ', 100.0 * (double)stab[s][i].pixels / (double)spix[s],
                    100.0 * (double)cum / (double)spix[s], foir_spec_describe(s, &stab[s][i].k, buf, sizeof buf));
            if (all) fprintf(out, "SPEC %d %llu %s\n", s, (unsigned long long)stab[s][i].pixels, buf);
        }
    }
}

static int cmp(const void *a, const void *b)
{
    const struct stat_ent *x = a, *y = b;
    return x->pixels < y->pixels ? 1 : (x->pixels > y->pixels ? -1 : 0);
}

static void dump(void)
{
    if (!out) out = stderr;
    qsort(tab, (size_t)nent, sizeof(tab[0]), cmp);
    uint64_t total = 0, cum = 0;
    for (int i = 0; i < nent; i++) total += tab[i].pixels;
    fprintf(out, "state statistics: %d distinct pipeline states, %llu pixels\n", nent, (unsigned long long)total);
    if (getenv("FOIR_STATE_STATS_ALL"))
        for (int i = 0; i < nent; i++)
            fprintf(out, "STATE %016llx %llu %s\n", (unsigned long long)tab[i].key, (unsigned long long)tab[i].pixels, tab[i].desc);
    for (int i = 0; i < nent && i < 16; i++) {
        cum += tab[i].pixels;
        fprintf(out, "  #%2d %5.1f%% (cum %5.1f%%) %9llu px %6llu batches  %s\n", i + 1, 100.0 * tab[i].pixels / total,
                100.0 * cum / total, (unsigned long long)tab[i].pixels, (unsigned long long)tab[i].batches, tab[i].desc);
    }
    int n90 = 0; cum = 0;
    for (int i = 0; i < nent; i++) { cum += tab[i].pixels; n90++; if (cum * 10 >= total * 9) break; }
    fprintf(out, "  states needed to cover 90%% of the pixels: %d\n", n90);
    dump_stage_keys();
}

void foir_stats_batch(const struct rdp *r, int npix)
{
    if (enabled < 0) {
        enabled = getenv("FOIR_STATE_STATS") != NULL;
        if (enabled) atexit(dump);
    }
    if (!enabled) return;
    pthread_mutex_lock(&stats_lock);       /* worker threads call this concurrently */
    {
        union spec_key keys[SPEC_NSTAGES];
        bool ok[SPEC_NSTAGES];
        foir_spec_keys(r, keys, ok);
        for (int s = 0; s < SPEC_NSTAGES; s++) {
            if (s == SPEC_TEX && !r->pf.need_tex) continue;
            spix[s] += (uint64_t)npix;
            if (!ok[s]) {      /* a state the stage cannot be specialised for (generic only): one bucket per reason */
                const struct other_modes *om = &r->om;
                const struct tile *t0 = &r->tile[r->pc.tilenum & 7];
                int why = 0;
                if (s == SPEC_TEX) why = om->tex_lod_en ? 1 : om->dolod ? 2 : t0->format == FORMAT_YUV ? 3 : (!om->bi_lerp0 && (om->sample_type || om->en_tlut)) ? 4 :
                                         om->cycle_type == CYCLE_TYPE_2 ? 5 : 6;
                else if (s == SPEC_COMB) why = 7;       /* chroma key */
                else why = 8;                           /* 4 / 8-bit colour buffer */
                memset(&keys[s], 0xff, sizeof keys[s]);
                keys[s].raw[1] = (uint8_t)why;
            }
            bool cov = ok[s] && foir_spec_lookup(s, &keys[s]) != NULL;
            bool exact_has = cov, shape_has = false;
            if (s == SPEC_TEX) {
                union spec_key ak = keys[s];
                foir_spec_any_key(s, &ak);
                exact_has = ok[s] && foir_spec_find(s, &keys[s]) != NULL;
                shape_has = ok[s] && foir_spec_find(s, &ak) != NULL;
                const int mode = foir_spec_tex_mode();
                cov = mode == COMB_EXACT ? exact_has : mode == COMB_SHAPE ? shape_has : mode == COMB_GENERIC ? false : (exact_has || shape_has);
                if (exact_has) tpix_exact += (uint64_t)npix;
                if (shape_has) tpix_any += (uint64_t)npix;
                if (shape_has && !exact_has) tpix_any_only += (uint64_t)npix;
            }
            if (s == SPEC_COMB) {
                union spec_key sk;
                shape_has = ok[s] && foir_spec_tabs[SPEC_SHAPE].n && foir_shape_resolve(r, &sk, NULL, NULL) && foir_spec_find(SPEC_SHAPE, &sk) != NULL;
                const int mode = foir_spec_comb_mode();
                cov = mode == COMB_EXACT ? exact_has : mode == COMB_SHAPE ? shape_has : mode == COMB_GENERIC ? false : (exact_has || shape_has);
                if (exact_has) cpix_exact += (uint64_t)npix;
                if (shape_has) cpix_shape += (uint64_t)npix;
                if (shape_has && !exact_has) cpix_shape_only += (uint64_t)npix;
            }
            if (cov) scov[s] += (uint64_t)npix;
            int i;
            for (i = 0; i < sn[s]; i++)
                if (!memcmp(stab[s][i].k.raw, keys[s].raw, 32)) break;
            if (i == sn[s]) {
                if (sn[s] >= 512) continue;
                sn[s]++;
                stab[s][i].k = keys[s]; stab[s][i].pixels = 0; stab[s][i].covered = cov;
                stab[s][i].exact = exact_has; stab[s][i].shape = shape_has;
            }
            stab[s][i].pixels += (uint64_t)npix;
        }
    }
    const struct other_modes *om = &r->om;
    const struct tile *tl = &r->tile[r->pc.tilenum];
    uint64_t h = 1469598103934665603ull;
#define MIX(v) do { h ^= (uint64_t)(uint32_t)(v); h *= 1099511628211ull; } while (0)
    MIX(om->cycle_type); MIX(om->persp_tex_en); MIX(om->en_tlut); MIX(om->tlut_type); MIX(om->sample_type);
    MIX(om->mid_texel); MIX(om->bi_lerp0); MIX(om->bi_lerp1); MIX(om->convert_one); MIX(om->key_en);
    MIX(om->rgb_dither_sel); MIX(om->alpha_dither_sel);
    for (int c = 0; c < 2; c++) { MIX(om->blend_m1a[c]); MIX(om->blend_m1b[c]); MIX(om->blend_m2a[c]); MIX(om->blend_m2b[c]); }
    MIX(om->force_blend); MIX(om->alpha_cvg_select); MIX(om->cvg_times_alpha); MIX(om->z_mode); MIX(om->cvg_dest);
    MIX(om->color_on_cvg); MIX(om->image_read_en); MIX(om->z_update_en); MIX(om->z_compare_en); MIX(om->antialias_en);
    MIX(om->z_source_sel); MIX(om->dither_alpha_en); MIX(om->alpha_compare_en); MIX(om->tex_lod_en); MIX(om->dolod);
    for (int cy = 0; cy < 2; cy++) {
        for (int i = 0; i < 4; i++) for (int ch = 0; ch < 3; ch++) MIX(r->comb.rgb[cy][i][ch]);
        for (int i = 0; i < 4; i++) MIX(r->comb.alpha[cy][i]);
    }
    MIX(tl->format); MIX(tl->size); MIX(r->fb_size); MIX(r->fb_format);
    MIX(r->pf.need_tex); MIX(r->pf.shade_rgb);
    for (int i = 0; i < nent; i++)
        if (tab[i].key == h) { tab[i].pixels += (uint64_t)npix; tab[i].batches++; pthread_mutex_unlock(&stats_lock); return; }
    if (nent >= 2048) { pthread_mutex_unlock(&stats_lock); return; }
    struct stat_ent *e = &tab[nent++];
    e->key = h; e->pixels = (uint64_t)npix; e->batches = 1;
    static const char *cyn[] = { "1cyc", "2cyc", "copy", "fill" }, *fmn[] = { "RGBA", "YUV", "CI", "IA", "I" };
    snprintf(e->desc, sizeof(e->desc), "%s %s tex=%s%d%s%s z=%c%c/m%d aa=%d rd=%d fb=%d cd=%d cxa=%d acs=%d blend=%d%d%d%d rgb0=%d,%d,%d,%d",
             cyn[om->cycle_type & 3], om->cycle_type == 1 ? "" : "", r->pf.need_tex ? fmn[tl->format % 5] : "-", 4 << tl->size,
             om->sample_type ? "/bilerp" : "/point", om->en_tlut ? "/tlut" : "", om->z_compare_en ? 'c' : '-', om->z_update_en ? 'u' : '-', om->z_mode,
             om->antialias_en, om->image_read_en, om->force_blend, om->cvg_dest, om->cvg_times_alpha, om->alpha_cvg_select, om->blend_m1a[om->cycle_type == 1], om->blend_m1b[om->cycle_type == 1], om->blend_m2a[om->cycle_type == 1],
             om->blend_m2b[om->cycle_type == 1], r->comb.rgb[1][0][0], r->comb.rgb[1][1][0], r->comb.rgb[1][2][0], r->comb.rgb[1][3][0]);
    pthread_mutex_unlock(&stats_lock);
}

void foir_stats_flush(void)
{
    const char *fn = getenv("FOIR_STATE_STATS_FILE");
    if (!fn || !*fn || !enabled || !nent) return;
    FILE *f = fopen(fn, "w");
    if (!f) return;
    pthread_mutex_lock(&stats_lock);
    out = f;
    dump();
    fclose(f);
    out = NULL;
    pthread_mutex_unlock(&stats_lock);
}
#else
void foir_stats_batch(const struct rdp *r, int npix) { (void)r; (void)npix; }
void foir_stats_flush(void) {}
#endif
