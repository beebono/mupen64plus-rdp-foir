/*
 * mupen64plus-rdp-foir - kernels_neon.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"

#if defined(FOIR_NEON)
#include <stdlib.h>
#include <stdio.h>
#include "kneon.h"
#include "spec.h"

static unsigned long vfy_count[8], vfy_bad[8];

static void vfy_report(const char *stage, const struct rdp *r, const char *field, int lane, long got, long want)
{
    static int printed;
    if (printed++ > 40) return;
    const struct other_modes *om = &r->om;
    fprintf(stderr, "VERIFY %s: %s lane %d neon=%ld scalar=%ld | cycle=%d tlut=%d sample=%d bilerp=%d,%d lod=%d persp=%d "
            "zc=%d zu=%d zmode=%d aa=%d fb=%d/%d rd=%d cvgdest=%d forceblend=%d cvgxa=%d acvg=%d acmp=%d "
            "m1a=%d%d m1b=%d%d m2a=%d%d m2b=%d%d\n",
            stage, field, lane, got, want, om->cycle_type, om->en_tlut, om->sample_type, om->bi_lerp0, om->bi_lerp1,
            om->dolod, om->persp_tex_en, om->z_compare_en, om->z_update_en, om->z_mode, om->antialias_en,
            r->fb_format, r->fb_size, om->image_read_en, om->cvg_dest, om->force_blend, om->cvg_times_alpha,
            om->alpha_cvg_select, om->alpha_compare_en, om->blend_m1a[0], om->blend_m1a[1], om->blend_m1b[0],
            om->blend_m1b[1], om->blend_m2a[0], om->blend_m2a[1], om->blend_m2b[0], om->blend_m2b[1]);
}

#define VCMP1(name, arr, idx, T) do { if ((long)(T)b->arr[idx] != (long)(T)ref.arr[idx]) { bad++; vfy_report(stage, r, name, idx, (long)b->arr[idx], (long)ref.arr[idx]); } } while (0)
#define VCMP4(name, arr, idx) do { for (int c_ = 0; c_ < 4; c_++) if (b->arr[c_][idx] != ref.arr[c_][idx]) { bad++; vfy_report(stage, r, name "[c]", idx, b->arr[c_][idx], ref.arr[c_][idx]); } } while (0)

static __thread struct px_batch vfy_ref;      /* the workers verify concurrently */

static void vfy_texture(struct rdp *r, struct px_batch *b)
{
    const char *stage = "texture";
    memcpy(&vfy_ref, b, sizeof(*b));
    foir_kernels_scalar.texture(r, &vfy_ref);
    if (r->spec_cur[SPEC_TEX]) r->spec_cur[SPEC_TEX](r, b); else neon_texture(r, b);
    struct px_batch ref2; (void)ref2;
    struct px_batch *rp = &vfy_ref;
    #define ref (*rp)
    int bad = 0;
    int two = r->om.cycle_type == CYCLE_TYPE_2;
    for (int i = 0; i < b->n; i++) {
        if (r->pf.need_t0) VCMP4("tex0", tex0, i);
        if (two ? r->pf.need_t1 : r->pf.next_texel) VCMP4("tex1", tex1, i);
        if (r->pf.next_texel) VCMP4("tex0n", tex0n, i);
        if (r->om.dolod) VCMP1("lodfrac", lodfrac, i, int16_t);
    }
    #undef ref
    __atomic_fetch_add(&vfy_count[0], 1, __ATOMIC_RELAXED); if (bad) __atomic_fetch_add(&vfy_bad[0], 1, __ATOMIC_RELAXED);
}

static void vfy_combine(struct rdp *r, struct px_batch *b)
{
    const char *stage = "combine";
    memcpy(&vfy_ref, b, sizeof(*b));
    foir_kernels_scalar.combine(r, &vfy_ref);
    if (r->spec_cur[SPEC_COMB]) r->spec_cur[SPEC_COMB](r, b); else neon_combine(r, b);
    struct px_batch *rp = &vfy_ref;
    #define ref (*rp)
    int bad = 0;
    for (int i = 0; i < b->n; i++) {
        VCMP4("pixel", pixel, i);
        VCMP1("acalpha", acalpha, i, int16_t);
        VCMP1("shade_alpha", shade_alpha, i, int16_t);
        VCMP1("cvg", cvg, i, uint8_t);
        VCMP1("cdith", cdith, i, int16_t);
        VCMP1("adith", adith, i, int8_t);
    }
    #undef ref
    if (bad) {
        static int shown;
        if (shown++ < 3) {
            const struct combiner *c = &r->comb;
            for (int cy = 0; cy < 2; cy++) {
                fprintf(stderr, "  comb cycle %d: ", cy);
                for (int ch = 0; ch < 3; ch++) fprintf(stderr, "rgb%d(%d,%d,%d,%d) ", ch, c->rgb[cy][0][ch], c->rgb[cy][1][ch], c->rgb[cy][2][ch], c->rgb[cy][3][ch]);
                fprintf(stderr, "a(%d,%d,%d,%d)\n", c->alpha[cy][0], c->alpha[cy][1], c->alpha[cy][2], c->alpha[cy][3]);
            }
            fprintf(stderr, "  lane0: tex0=%d,%d,%d,%d tex1=%d,%d,%d,%d shade? cvg=%d prim=%d,%d,%d,%d env=%d,%d,%d,%d\n", b->tex0[0][0], b->tex0[1][0], b->tex0[2][0], b->tex0[3][0],
                    b->tex1[0][0], b->tex1[1][0], b->tex1[2][0], b->tex1[3][0], b->cvg[0], r->prim_color.r, r->prim_color.g, r->prim_color.b, r->prim_color.a, r->env_color.r, r->env_color.g, r->env_color.b, r->env_color.a);
        }
    }
    __atomic_fetch_add(&vfy_count[1], 1, __ATOMIC_RELAXED); if (bad) __atomic_fetch_add(&vfy_bad[1], 1, __ATOMIC_RELAXED);
}

static void vfy_zcmp(struct rdp *r, struct px_batch *b)
{
    const char *stage = "zcmp";
    memcpy(&vfy_ref, b, sizeof(*b));
    foir_kernels_scalar.zcmp(r, &vfy_ref);
    if (r->spec_cur[SPEC_ZCMP]) r->spec_cur[SPEC_ZCMP](r, b); else neon_zcmp(r, b);
    struct px_batch *rp = &vfy_ref;
    #define ref (*rp)
    int bad = 0;
    for (int i = 0; i < b->n; i++) {
        if (!!b->wen[i] != !!ref.wen[i]) { bad++; vfy_report(stage, r, "wen", i, b->wen[i], ref.wen[i]); }
        if (!!b->blend_en[i] != !!ref.blend_en[i]) { bad++; vfy_report(stage, r, "blend_en", i, b->blend_en[i], ref.blend_en[i]); }
        if (!!b->prewrap[i] != !!ref.prewrap[i]) { bad++; vfy_report(stage, r, "prewrap", i, b->prewrap[i], ref.prewrap[i]); }
        VCMP1("memcvg", memcvg, i, uint8_t);
        VCMP1("cvg", cvg, i, uint8_t);
        VCMP1("sz", sz, i, int32_t);
        if (r->om.real_blendshifts) {
            VCMP1("blshifta", blshifta, i, int8_t);
            VCMP1("blshiftb", blshiftb, i, int8_t);
        }
        VCMP4("mem", mem, i);
    }
    #undef ref
    __atomic_fetch_add(&vfy_count[2], 1, __ATOMIC_RELAXED); if (bad) __atomic_fetch_add(&vfy_bad[2], 1, __ATOMIC_RELAXED);
}

static void vfy_blend(struct rdp *r, struct px_batch *b)
{
    const char *stage = "blend";
    memcpy(&vfy_ref, b, sizeof(*b));
    foir_kernels_scalar.blend(r, &vfy_ref);
    if (r->spec_cur[SPEC_BLEND]) r->spec_cur[SPEC_BLEND](r, b); else neon_blend(r, b);
    struct px_batch *rp = &vfy_ref;
    #define ref (*rp)
    int bad = 0;
    for (int i = 0; i < b->n; i++) {
        VCMP1("wen", wen, i, uint8_t);
        if (!ref.wen[i]) continue;
        for (int c = 0; c < 3; c++)
            if (b->fcol[c][i] != ref.fcol[c][i]) { bad++; vfy_report(stage, r, "fcol", i, b->fcol[c][i], ref.fcol[c][i]); }
    }
    #undef ref
    __atomic_fetch_add(&vfy_count[3], 1, __ATOMIC_RELAXED); if (bad) __atomic_fetch_add(&vfy_bad[3], 1, __ATOMIC_RELAXED);
}

/* save the lanes' pixels, run scalar and keep its result, restore, run NEON, compare; only those pixels are touched, so this is safe with several workers */
struct vfy_px { uint32_t v; uint8_t h; };

static void vfy_px_get(const struct rdp *r, int kind, uint32_t idx, struct vfy_px *o)
{
    if (kind == 2) {
        o->v = rdram_rd32(r, idx); o->h = 0;
    } else {
        const uint32_t i = idx & (RDRAM_MASK >> 1);
        o->v = rdram_rd16(r, i);
        o->h = (i << 1) < r->rdram_size ? r->hidden[i] : 0;
    }
}

static void vfy_px_put(struct rdp *r, int kind, uint32_t idx, const struct vfy_px *o)
{
    if (kind == 2) {
        rdram_wr32(r, idx, o->v);
    } else {
        const uint32_t i = idx & (RDRAM_MASK >> 1);
        rdram_wr16(r, i, (uint16_t)o->v);
        if ((i << 1) < r->rdram_size) r->hidden[i] = o->h;
    }
}

static void vfy_store(struct rdp *r, struct px_batch *b)
{
    const bool z = r->om.z_update_en;
    const int fk = r->fb_size == PIXEL_SIZE_32BIT ? 2 : 1;
    const uint32_t fbase = fk == 2 ? r->fb_address >> 2 : r->fb_address >> 1, zbase = r->zb_address >> 1;
    struct vfy_px before[2][FOIR_BATCH], want[2][FOIR_BATCH];
    const int n = b->n < FOIR_BATCH ? b->n : FOIR_BATCH;
    if (r->fb_size < PIXEL_SIZE_16BIT) {                 /* 4 / 8-bit colour buffers are not checked */
        r->spec_cur[SPEC_STORE] ? r->spec_cur[SPEC_STORE](r, b) : neon_store(r, b);
        return;
    }
    for (int i = 0; i < n; i++)
        if (b->wen[i]) {
            vfy_px_get(r, fk, fbase + b->fbidx[i], &before[0][i]);
            if (z) vfy_px_get(r, 1, zbase + b->fbidx[i], &before[1][i]);
        }
    foir_kernels_scalar.store(r, b);
    for (int i = 0; i < n; i++)
        if (b->wen[i]) {
            vfy_px_get(r, fk, fbase + b->fbidx[i], &want[0][i]);
            if (z) vfy_px_get(r, 1, zbase + b->fbidx[i], &want[1][i]);
            vfy_px_put(r, fk, fbase + b->fbidx[i], &before[0][i]);
            if (z) vfy_px_put(r, 1, zbase + b->fbidx[i], &before[1][i]);
        }
    if (r->spec_cur[SPEC_STORE]) r->spec_cur[SPEC_STORE](r, b); else neon_store(r, b);
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (b->wen[i]) {
            struct vfy_px g;
            for (int k = 0; k < 1 + z; k++) {
                vfy_px_get(r, k ? 1 : fk, (k ? zbase : fbase) + b->fbidx[i], &g);
                if (g.v != want[k][i].v || g.h != want[k][i].h) {
                    bad++;
                    vfy_report(k ? "store(z)" : "store", r, "pixel", i, (long)g.v | ((long)g.h << 32), (long)want[k][i].v | ((long)want[k][i].h << 32));
                }
            }
        }
    __atomic_fetch_add(&vfy_count[4], 1, __ATOMIC_RELAXED); if (bad) __atomic_fetch_add(&vfy_bad[4], 1, __ATOMIC_RELAXED);
}

/* same for a COPY span, on its colour buffer row; per pixel / byte, never a whole word (a neighbouring row may belong to another worker) */
static void vfy_copy(struct rdp *r, int y, const struct span *sp, int flip)
{
    const int px16 = r->fb_size == PIXEL_SIZE_16BIT;
    int npix = r->fb_width;
    if (npix > 1024) npix = 1024;
    const uint32_t row = (uint32_t)(r->fb_width * y);
    const uint32_t base = px16 ? (r->fb_address >> 1) + row : r->fb_address + row;
    struct pxs { uint16_t v; uint8_t h; };
    struct pxs before[1024], want[1024];
    #define VGET(dst, i) do { uint32_t a_ = (base + (uint32_t)(i)) & (px16 ? RDRAM_MASK >> 1 : RDRAM_MASK); \
        if (px16) { uint16_t v_; uint8_t h_; rdram_rd_pair16(r, a_, &v_, &h_); dst[i].v = v_; dst[i].h = (a_ << 1) < r->rdram_size ? r->hidden[a_] : 0; } \
        else { dst[i].v = rdram_rd8(r, a_); dst[i].h = 0; } } while (0)
    #define VPUT(src, i) do { uint32_t a_ = (base + (uint32_t)(i)) & (px16 ? RDRAM_MASK >> 1 : RDRAM_MASK); \
        if (px16) { rdram_wr16(r, a_, src[i].v); if ((a_ << 1) < r->rdram_size) r->hidden[a_] = src[i].h; } \
        else rdram_wr8(r, a_, (uint8_t)src[i].v); } while (0)
    for (int i = 0; i < npix; i++) VGET(before, i);
    foir_kernels_scalar.copy(r, y, sp, flip);
    for (int i = 0; i < npix; i++) VGET(want, i);
    for (int i = 0; i < npix; i++) VPUT(before, i);
    neon_copy(r, y, sp, flip);
    int bad = 0;
    for (int i = 0; i < npix; i++) VGET(before, i);
    for (int i = 0; i < npix; i++) {
        if (before[i].v != want[i].v || before[i].h != want[i].h) {
            bad++;
            vfy_report("copy", r, "pixel", i, before[i].v | (before[i].h << 16), want[i].v | (want[i].h << 16));
        }
    }
    #undef VGET
    #undef VPUT
    __atomic_fetch_add(&vfy_count[6], 1, __ATOMIC_RELAXED); if (bad) __atomic_fetch_add(&vfy_bad[6], 1, __ATOMIC_RELAXED);
}

#ifdef FOIR_STATS
unsigned long neon_stat_fb[9], neon_stat_ok[9];
#endif

void foir_neon_verify_report(char *buf, size_t n)
{
    static const char *nm[] = { "texture", "combine", "zcmp", "blend", "store", "fill", "copy" };
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < 7; i++)
        if (vfy_count[i] && o < n)
            o += (size_t)snprintf(buf + o, n - o, "%s%s %lu batches / %lu mismatching", o ? ", " : "", nm[i], vfy_count[i], vfy_bad[i]);
}

static void vfy_atexit(void)
{
#ifdef FOIR_STATS
    static const char *nms[] = { "texture", "combine", "zcmp", "blend", "store", "fill", "zcmp-fastpath(fb=slow)", "z groups dead(fb=alive)", "copy spans" };
    for (int i = 0; i < 9; i++)
        if (neon_stat_fb[i] || neon_stat_ok[i]) fprintf(stderr, "neon %s: %lu batches handled, %lu scalar fallbacks\n", nms[i], neon_stat_ok[i], neon_stat_fb[i]);
#endif
    static const char *nm[] = { "texture", "combine", "zcmp", "blend", "store", "fill", "copy" };
    for (int i = 0; i < 7; i++)
        if (vfy_count[i]) fprintf(stderr, "verify %s: %lu batches, %lu mismatching\n", nm[i], vfy_count[i], vfy_bad[i]);
}

static unsigned l_neon_mask = 0x7f;

unsigned foir_kernels_neon_mask(void) { foir_kernels_neon(); return l_neon_mask; }

const struct foir_kernels *foir_kernels_neon(void)
{
    static struct foir_kernels k;
    static bool done;
    if (!done) {
        unsigned mask = 0x7f;
        /* FOIR_NEON_MASK: bit per stage (0 texture, 1 combine, 2 zcmp, 3 blend, 4 store, 5 fill, 6 copy) enables its NEON kernel */
        const char *e = getenv("FOIR_NEON_MASK");
        if (e && *e) mask = (unsigned)strtoul(e, NULL, 0);
        l_neon_mask = mask;
        k = foir_kernels_scalar;
        k.name = "neon";
        if (mask & 0x01) k.texture = neon_texture;
        if (mask & 0x02) k.combine = neon_combine;
        if (mask & 0x04) k.zcmp = neon_zcmp;
        if (mask & 0x08) k.blend = neon_blend;
        if (mask & 0x10) k.store = neon_store;
        if (mask & 0x20) k.fill = neon_fill;
        if (mask & 0x40) k.copy = neon_copy;
        const char *v = getenv("FOIR_NEON_VERIFY");
        if (getenv("FOIR_NEON_STATS") || (getenv("FOIR_NEON_VERIFY"))) atexit(vfy_atexit);
        if (v && *v == '1') {
            if (mask & 0x01) k.texture = vfy_texture;
            if (mask & 0x02) k.combine = vfy_combine;
            if (mask & 0x04) k.zcmp = vfy_zcmp;
            if (mask & 0x08) k.blend = vfy_blend;
            if (mask & 0x10) k.store = vfy_store;
            if (mask & 0x40) k.copy = vfy_copy;
        }
        done = true;
    }
    return &k;
}
#else
const struct foir_kernels *foir_kernels_neon(void) { return NULL; }
void foir_neon_verify_report(char *buf, size_t n) { if (n) buf[0] = 0; }
unsigned foir_kernels_neon_mask(void) { return 0; }
#endif
