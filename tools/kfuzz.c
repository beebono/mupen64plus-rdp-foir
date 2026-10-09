/*
 * kfuzz: randomised equivalence test of the NEON pixel kernels against the scalar
 * reference kernels.
 *
 *   kfuzz [-n iterations] [-s seed] [-m stage mask] [-v]
 *
 * Every iteration draws a random RDP render state (through the real command
 * parser: set_other_modes, set_combine, set_tile, colours, framebuffer / Z image,
 * ...), random TMEM / framebuffer / Z-buffer contents and a random pixel batch
 * (several scanline runs, random interpolants and coverage). The batch is run
 * through the scalar stages (the reference chain); each NEON stage is then run
 * on the reference input of that stage and its outputs (and, for the store
 * stage, the RDRAM it wrote) are compared with the reference.
 *
 * Stages: 1 texture, 2 combine, 4 zcmp, 8 blend, 16 store, 32 copy (the COPY-cycle span kernel: random tile / TLUT / mask /
 * shift state, random spans including ones that touch the row ends, compared on the whole colour buffer and its hidden bits).
 *
 * -S also walks through the shape-specialised combiner kernels (the sixth table, spec.h "Shape kernels"): the state
 * is the random one with random combiner selectors / operands of the kernel's shape written over it
 * (foir_shape_apply), the selection is forced to the shape kernels (FOIR_SPEC_COMB=shape semantics) and must pick
 * exactly that kernel. -T n restricts the walk to table n (0 texture, 1 combine, 2 zcmp, 3 blend, 4 store, 5 shape).
 *
 * With -S the iterations walk through every specialised variant (spec.h): the render state is the random one
 * with the variant's key written over it (foir_spec_apply), the variant selection must pick exactly that
 * variant, and all five stages then run through the kernel table the selection produced (so the variants of the
 * other stages that the state happens to match are exercised too). The reference is the scalar chain, except for
 * a variant with RelaxedPrecision set, whose reference is the generic NEON chain run with the same knob (the
 * scalar kernels have no relaxed mode).
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rdp.h"
#include "../src/spec.h"

#define RDRAM_BYTES 0x200000
#define FB_BASE 0x10000
#define Z_BASE  0x80000

static uint64_t rng_s[2];
static uint64_t rnd64(void)
{
    uint64_t a = rng_s[0], b = rng_s[1];
    rng_s[0] = b;
    a ^= a << 23;
    rng_s[1] = a ^ b ^ (a >> 17) ^ (b >> 26);
    return rng_s[1] + b;
}
static uint32_t rnd(void) { return (uint32_t)(rnd64() >> 16); }
static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }
static void seed_rng(uint64_t s)
{
    rng_s[0] = s * 0x9e3779b97f4a7c15ull + 1; rng_s[1] = s ^ 0xdeadbeefcafef00dull;
    for (int i = 0; i < 8; i++) rnd64();
}

static void cmd(struct rdp *r, uint32_t w0, uint32_t w1)
{
    uint32_t c[2] = { w0, w1 };
    rdp_run_command(r, c);
}

static uint32_t rbits(int n) { return rnd() & (n >= 32 ? ~0u : ((1u << n) - 1)); }

/* ---- random state ------------------------------------------------------------------ */

struct fz_opts { int wide_modes; };

static void random_state(struct rdp *r)
{
    /* other modes: random bits, one or two cycle */
    uint32_t w0 = (0x2fu << 24) | (rbits(24) & ~(3u << 20));
    w0 |= (rnd() & 1) << 20;                      /* cycle type 0 or 1 */
    if (rndn(2)) w0 &= ~((1u << 18) | (1u << 17) | (1u << 16));   /* half the scenes: no detail / sharpen / lod */
    if (rndn(3)) w0 &= ~(1u << 8);                /* key_en rarely */
    if (rndn(5) == 0) w0 &= ~(3u << 6), w0 |= 2u << 6;           /* random rgb dither sometimes */
    uint32_t w1 = rnd();
    cmd(r, w0, w1);

    /* combiner */
    cmd(r, (0x3cu << 24) | rbits(24), rnd());

    /* tiles */
    for (int t = 0; t < 8; t++) {
        uint32_t fmt = rndn(5), size = rndn(4);
        if (rndn(6)) { fmt = rndn(5); if (fmt == FORMAT_YUV) fmt = FORMAT_RGBA; }
        if (fmt == FORMAT_RGBA && rndn(2)) size = 2 + rndn(2);
        uint32_t line = 1 + rndn(48), tmem = rndn(0x180);
        uint32_t maskt = rndn(3) ? rndn(8) : rndn(16), masks = rndn(3) ? rndn(8) : rndn(16);
        uint32_t w0t = (0x35u << 24) | (fmt << 21) | (size << 19) | (line << 9) | tmem;
        uint32_t w1t = ((uint32_t)t << 24) | (rndn(16) << 20) | (rndn(2) << 19) | (rndn(2) << 18) | (maskt << 14) |
                       (rndn(16) << 10) | (rndn(2) << 9) | (rndn(2) << 8) | (masks << 4) | rndn(16);
        cmd(r, w0t, w1t);
        uint32_t sl = rndn(0x100), tl = rndn(0x100), sh = sl + rndn(0x200), th = tl + rndn(0x200);
        cmd(r, (0x32u << 24) | (sl << 12) | tl, ((uint32_t)t << 24) | (sh << 12) | th);
    }

    /* colours and constants */
    cmd(r, 0x38u << 24, rnd());                    /* fog */
    cmd(r, 0x39u << 24, rnd());                    /* blend */
    cmd(r, (0x3au << 24) | (rbits(5) << 8) | rbits(8), rnd());   /* prim colour + lod */
    cmd(r, 0x3bu << 24, rnd());                    /* env */
    cmd(r, (0x2cu << 24) | rbits(24), rnd());      /* convert */
    cmd(r, (0x2eu << 24), (rnd() & 0x7fff0000u) | (1u << rndn(15)));   /* prim depth */
    cmd(r, (0x2au << 24) | rbits(24), rnd());      /* key gb */
    cmd(r, (0x2bu << 24), rnd());                  /* key r */

    /* colour image + Z image */
    uint32_t fsize = rndn(8) ? 2 + rndn(2) : rndn(2);
    uint32_t ffmt = rndn(4) ? FORMAT_RGBA : FORMAT_IA;
    uint32_t fw = 16 + rndn(305);
    cmd(r, (0x3fu << 24) | (ffmt << 21) | (fsize << 19) | (fw - 1), FB_BASE + (rndn(8) << 1) * 2);
    cmd(r, 0x3eu << 24, Z_BASE + (rndn(8) << 1) * 2);
    cmd(r, (0x2du << 24), (320u << 14) | (240u << 2));      /* scissor (unused by kernels) */
}

/* ---- random memory --------------------------------------------------------------------- */

static void random_memory(struct rdp *r, int zbias)
{
    for (int i = 0; i < 0x1000; i++) r->tmem[i] = (uint8_t)rnd();
    uint32_t *w = (uint32_t *)r->rdram;
    int mode = (int)rndn(4);
    for (uint32_t i = 0; i < 0x40000 / 4; i++) {
        uint32_t v = rnd();
        if (mode == 1) v = (v & 0x00ff00ff) | 0x00010001;
        if (mode == 2) v &= 0x0f0f0f0f;
        w[(FB_BASE >> 2) + i] = v;
        w[(Z_BASE >> 2) + i] = zbias ? (v & 0xfffcfffc) : v;
    }
    /* the first and last 32 KB too (buffers placed at the edges of RDRAM, see edge_placement) */
    for (uint32_t i = 0; i < 0x8000 / 4; i++) { w[i] = rnd(); w[(RDRAM_BYTES - 0x8000) / 4 + i] = rnd(); }
    for (uint32_t i = 0; i < 0x8000 / 2; i++) { r->hidden[i] = (uint8_t)rnd() & 0xf; r->hidden[(RDRAM_BYTES - 0x8000) / 2 + i] = (uint8_t)rnd() & 0xf; }
    for (uint32_t i = 0; i < 0x40000 / 2; i++) {
        r->hidden[(FB_BASE >> 1) + i] = (uint8_t)rnd();
        r->hidden[(Z_BASE >> 1) + i] = (uint8_t)rnd();
        if (rndn(2)) { r->hidden[(FB_BASE >> 1) + i] &= 0x7; r->hidden[(Z_BASE >> 1) + i] &= 0xf; }
    }
}

/* ---- random batch ---------------------------------------------------------------------------- */

static int32_t rnd_coord(void)
{
    switch (rndn(5)) {
    case 0: return (int32_t)rnd();
    case 1: return (int32_t)(rnd() & 0x00ffffff);
    case 2: return (int32_t)(rnd() & 0x03ffffff) - 0x01000000;
    case 3: return (int32_t)((rnd() & 0x3ff) << 16);
    default: return (int32_t)(rnd() & 0xffffff) << 4;
    }
}

static void random_batch(struct rdp *r, struct px_batch *b, int tilenum)
{
    memset(b, 0, sizeof(*b));
    struct prim_ctx *pc = &r->pc;
    memset(pc, 0, sizeof(*pc));
    pc->tilenum = tilenum >= 0 ? tilenum : (int)rndn(8);
    pc->max_level = (int)rndn(8);
    pc->flip = (int)rndn(2);
    pc->dsinc = rnd_coord() >> 8; pc->dtinc = rnd_coord() >> 8; pc->dwinc = rnd_coord() >> 10;
    pc->ds = pc->dsinc; pc->dt = pc->dtinc; pc->dw = pc->dwinc;
    pc->cdr = (int)rndn(0x1000) - 0x800; pc->cdg = (int)rndn(0x1000) - 0x800;
    pc->cdb = (int)rndn(0x1000) - 0x800; pc->cda = (int)rndn(0x1000) - 0x800;
    pc->drdy = (int)rndn(0x1000) - 0x800; pc->dgdy = (int)rndn(0x1000) - 0x800;
    pc->dbdy = (int)rndn(0x1000) - 0x800; pc->dady = (int)rndn(0x1000) - 0x800;
    pc->cdz = (int)rndn(0x4000) - 0x2000; pc->dzdy = (int)rndn(0x4000) - 0x2000;
    pc->dsdy = rnd_coord() >> 8; pc->dtdy = rnd_coord() >> 8; pc->dwdy = rnd_coord() >> 10;
    uint32_t dz = 1u << rndn(16);
    pc->dzpix = (int32_t)dz;
    pc->dzpixenc = (int32_t)dz_compress(dz);
    pc->seed = rnd();

    int n = 1 + (int)rndn(FOIR_BATCH);
    if (rndn(3) == 0) n = FOIR_BATCH - (int)rndn(3);
    b->n = n;
    int pos = 0;
    int width = r->fb_width;
    while (pos < n) {
        int len = 1 + (int)rndn(rndn(2) ? 24 : 70);
        if (len > n - pos) len = n - pos;
        uint32_t row = rndn(12), x0 = rndn((uint32_t)(width > 16 ? width - 8 : 1));
        if (rndn(3) == 0) x0 = (x0 & ~1u);
        for (int i = 0; i < len; i++) {
            uint32_t x = x0 + (uint32_t)i;
            b->fbidx[pos + i] = row * (uint32_t)width + x;
            b->xpos[pos + i] = (int16_t)x;
            b->ditidx[pos + i] = (uint8_t)(((row & 3) << 2) | (x & 3));
        }
        pos += len;
    }
    int cvgmode = (int)rndn(3);
    for (int i = 0; i < n; i++) {
        b->s[i] = rnd_coord(); b->t[i] = rnd_coord();
        b->w[i] = rndn(4) ? (int32_t)((rnd() & 0x3fff) << 16) : rnd_coord();
        b->cr[i] = rnd_coord(); b->cg[i] = rnd_coord(); b->cb[i] = rnd_coord(); b->ca[i] = rnd_coord();
        b->cz[i] = (int32_t)(rnd() >> (rndn(3) ? 2 : 0)) & (rndn(2) ? ~0 : 0x3fffffff);
        uint8_t m = cvgmode == 0 ? 0xff : (uint8_t)rnd();
        if (cvgmode == 1 && rndn(4)) m = 0xff;
        /* derive coverage fields the way the rasteriser does (cvg count + offsets) */
        int cnt = 0;
        for (int k = 0; k < 8; k++) cnt += (m >> k) & 1;
        b->cvg[i] = (uint8_t)cnt;
        b->cvbit[i] = (m >> 7) & 1;
        b->offx[i] = m == 0xff ? 0 : (uint8_t)rndn(4);
        b->offy[i] = m == 0xff ? 0 : (uint8_t)rndn(4);
    }
    b->all_full = 0;
    for (int i = n; i < FOIR_LANES; i++) { b->fbidx[i] = 0; b->cvg[i] = 8; }
}


/* one iteration in eight: the colour buffer or the Z buffer is moved to the first or last pixels of RDRAM so that the
 * vector windows of the memory kernels run over the edges (the bounds checks / per-lane fallbacks) */
static int g_edge;
static void edge_placement(struct rdp *r)
{
    g_edge = rndn(8) == 0 ? 1 + (int)rndn(4) : 0;
    switch (g_edge) {
    case 1: r->fb_address = RDRAM_BYTES - 2 * rndn(3000) - 2 * rndn(2); break;
    case 2: r->zb_address = RDRAM_BYTES - 2 * rndn(3000); break;
    case 3: r->fb_address = 2 * rndn(60); break;
    case 4: r->zb_address = 2 * rndn(60); break;
    }
}

/* the edge regions of two contexts must agree too */
static int cmp_edges(const struct rdp *ra, const struct rdp *rb)
{
    int bad = 0;
    for (uint32_t i = 0; i < 0x8000 && bad < 6; i += 4) {
        uint32_t o[2] = { i, RDRAM_BYTES - 0x8000 + i };
        for (int k = 0; k < 2; k++)
            if (memcmp(ra->rdram + o[k], rb->rdram + o[k], 4)) {
                fprintf(stderr, "  MISMATCH edge: rdram word at 0x%x differs (neon=%08x scalar=%08x)\n", o[k], *(const uint32_t *)(rb->rdram + o[k]), *(const uint32_t *)(ra->rdram + o[k]));
                bad++;
            }
    }
    for (uint32_t i = 0; i < 0x4000 && bad < 6; i++) {
        uint32_t o[2] = { i, (RDRAM_BYTES - 0x8000) / 2 + i };
        for (int k = 0; k < 2; k++)
            if (ra->hidden[o[k]] != rb->hidden[o[k]]) {
                fprintf(stderr, "  MISMATCH edge: hidden byte %u differs (neon=%02x scalar=%02x)\n", o[k], rb->hidden[o[k]], ra->hidden[o[k]]);
                bad++;
            }
    }
    return bad;
}

/* ---- comparison ----------------------------------------------------------------------------------- */

static int g_bad;
static long g_checks;

#define CHK(name, a, bb, i) do { if ((long)(a) != (long)(bb)) { if (g_bad < 12) fprintf(stderr, "  MISMATCH %s lane %d: neon=%ld scalar=%ld\n", name, i, (long)(a), (long)(bb)); g_bad++; } g_checks++; } while (0)

static void dump_state(const struct rdp *r)
{
    const struct other_modes *om = &r->om;
    fprintf(stderr, "  state: cycle=%d tlut=%d/%d sample=%d bilerp=%d,%d mid=%d conv=%d key=%d persp=%d lod=%d dolod=%d rgbd=%d alphad=%d | "
            "acmp=%d zcmp=%d zupd=%d zmode=%d aa=%d fb=%d/%d w=%d rd=%d cvgdest=%d fb=%d cxa=%d acvg=%d col=%d dithA=%d | m1a=%d%d m1b=%d%d m2a=%d%d m2b=%d%d\n",
            om->cycle_type, om->en_tlut, om->tlut_type, om->sample_type, om->bi_lerp0, om->bi_lerp1, om->mid_texel, om->convert_one,
            om->key_en, om->persp_tex_en, om->tex_lod_en, om->dolod, om->rgb_dither_sel, om->alpha_dither_sel, om->alpha_compare_en,
            om->z_compare_en, om->z_update_en, om->z_mode, om->antialias_en, r->fb_format, r->fb_size, r->fb_width, om->image_read_en,
            om->cvg_dest, om->force_blend, om->cvg_times_alpha, om->alpha_cvg_select, om->color_on_cvg, om->dither_alpha_en,
            om->blend_m1a[0], om->blend_m1a[1], om->blend_m1b[0], om->blend_m1b[1], om->blend_m2a[0], om->blend_m2a[1],
            om->blend_m2b[0], om->blend_m2b[1]);
    for (int cy = 0; cy < 2; cy++) {
        fprintf(stderr, "  comb cycle %d: ", cy);
        for (int ch = 0; ch < 3; ch++)
            fprintf(stderr, "rgb%d(%d,%d,%d,%d) ", ch, r->comb.rgb[cy][0][ch], r->comb.rgb[cy][1][ch], r->comb.rgb[cy][2][ch], r->comb.rgb[cy][3][ch]);
        fprintf(stderr, "a(%d,%d,%d,%d)\n", r->comb.alpha[cy][0], r->comb.alpha[cy][1], r->comb.alpha[cy][2], r->comb.alpha[cy][3]);
    }
    for (int k = 0; k < (r->om.cycle_type == CYCLE_TYPE_2 ? 2 : 1); k++) {
    int tn = (r->pc.tilenum + k) & 7;
    const struct tile *t = &r->tile[tn];
    fprintf(stderr, "  tile%d: fmt=%d size=%d line=%d tmem=%d pal=%d ct=%d mt=%d cs=%d ms=%d mask=%d,%d shift=%d,%d sl=%d tl=%d sh=%d th=%d\n",
            tn, t->format, t->size, t->line, t->tmem, t->palette, t->ct, t->mt, t->cs, t->ms, t->mask_s, t->mask_t,
            t->shift_s, t->shift_t, t->sl, t->tl, t->sh, t->th);
    }
}

static int g_spec_mode;                 /* -S: walk through the specialised variants */
static int g_spec_only = -1;            /* -T: only this table */
static long g_spec_hits[SPEC_NSTAGES + 1][512];
static long g_spec_other;               /* iterations in which a stage other than the one under test ran a variant */

static int run_one(uint64_t seed, int mask, int verbose, long iter)
{
    static struct rdp ra, rb;
    static uint8_t *mem_a, *mem_b, *mem_save;
    if (!mem_a) {
        mem_a = calloc(1, RDRAM_BYTES); mem_b = calloc(1, RDRAM_BYTES); mem_save = calloc(1, RDRAM_BYTES);
    }
    seed_rng(seed);
    struct foir_config cfg;
    foir_config_defaults(&cfg);
    cfg.neon = false;
    cfg.relaxed_precision = false;
    rdp_init(&ra, &cfg);
    rdp_attach_memory(&ra, mem_a, RDRAM_BYTES);
    const struct foir_kernels *nk = foir_kernels_neon();
    if (!nk) { fprintf(stderr, "kfuzz: no NEON kernels in this build\n"); exit(2); }

    random_state(&ra);
    random_memory(&ra, rndn(2));
    edge_placement(&ra);
    cmd(&ra, 0x27u << 24, 0);
    int tilenum = -1, vstage = -1, vidx = -1;
    int relaxed = 0;
    if (g_spec_mode) {
        long total = 0;
        for (int st = 0; st <= SPEC_NSTAGES; st++) if (g_spec_only < 0 || st == g_spec_only) total += foir_spec_tabs[st].n;
        long vi = iter % total;
        for (vstage = 0; ; vstage++) {
            if (g_spec_only >= 0 && vstage != g_spec_only) continue;
            if (vi < foir_spec_tabs[vstage].n) break;
            vi -= foir_spec_tabs[vstage].n;
        }
        vidx = (int)vi;
        tilenum = (int)rndn(8);
        ra.pc.tilenum = tilenum;
        foir_spec_set_comb_mode(vstage == SPEC_SHAPE ? COMB_SHAPE : COMB_AUTO);
        /* an "any format" texture kernel is only chosen when no exact variant matches: the walk forces the second tier */
        foir_spec_set_tex_mode(vstage == SPEC_TEX && foir_spec_tabs[SPEC_TEX].ent[vidx].k.tex.any ? COMB_SHAPE : COMB_AUTO);
        if (vstage == SPEC_SHAPE) foir_shape_apply(&foir_spec_tabs[vstage].ent[vidx].k, &ra, rnd64());
        else foir_spec_apply(vstage, &foir_spec_tabs[vstage].ent[vidx].k, &ra);
        relaxed = ra.cfg.relaxed_precision;
    }
    rdp_update_derived(&ra);
    if (g_spec_mode) {
        /* an "any option" variant is only used when no variant with these exact options exists: draw the options
         * (dither, alpha compare, coverage, force blend, ...) again until the selection falls on the variant under test */
        const struct spec_ent *want = &foir_spec_tabs[vstage].ent[vidx];
        int any = (vstage == SPEC_COMB && want->k.comb.any) || (vstage == SPEC_BLEND && want->k.blend.any);
        for (int tries = 0; any && tries < 200; tries++) {
            union spec_key kk[SPEC_NSTAGES]; bool okk[SPEC_NSTAGES];
            foir_spec_keys(&ra, kk, okk);
            if (foir_spec_lookup(vstage, &kk[vstage]) == want) break;
            struct other_modes *om = &ra.om;
            om->rgb_dither_sel = (int)rndn(4); om->alpha_dither_sel = (int)rndn(4);
            om->alpha_compare_en = (int)rndn(2); om->alpha_cvg_select = (int)rndn(2); om->cvg_times_alpha = (int)rndn(2);
            om->dither_alpha_en = (int)rndn(2); om->antialias_en = (int)rndn(2); om->force_blend = (int)rndn(2);
            om->color_on_cvg = (int)rndn(2);
            if (vstage == SPEC_COMB) { om->blend_m1b[0] = (int)rndn(4); om->blend_m1b[1] = (int)rndn(4); }
            rdp_update_derived(&ra);
        }
    }
    struct px_batch *bref = malloc(sizeof(*bref)), *bn = malloc(sizeof(*bn)), *pre = malloc(sizeof(*pre));
    random_batch(&ra, bref, tilenum);
    const struct foir_kernels *refk = &foir_kernels_scalar;

    /* B context: identical render state and memory */
    memcpy(&rb, &ra, sizeof(rb));
    rb.rdram = mem_b; rb.hidden = calloc(1, (RDRAM_BYTES >> 1) + 16);
    memcpy(mem_b, mem_a, RDRAM_BYTES);
    memcpy(rb.hidden, ra.hidden, (RDRAM_BYTES >> 1) + 16);
    rb.span_scratch = NULL;
    rb.k = nk;
    ra.k = &foir_kernels_scalar;
    if (g_spec_mode) {
        rb.kbase = *nk; rb.spec_ok_mask = foir_kernels_neon_mask() & 0x1f;
        rb.cfg.specialize = true;
        foir_spec_select(&rb);
        const struct spec_ent *want = &foir_spec_tabs[vstage].ent[vidx];
        const int sstage = vstage == SPEC_SHAPE ? SPEC_COMB : vstage;     /* the stage whose kernel the variant replaces */
        if (!(rb.spec_mask & SPEC_BIT(sstage)) || rb.spec_cur[sstage] != want->fn ||
            ((vstage == SPEC_SHAPE || vstage == SPEC_COMB) && (vstage == SPEC_SHAPE) != rb.spec_comb_shape)) {
            union spec_key kk[SPEC_NSTAGES]; bool okk[SPEC_NSTAGES]; char b1[200], b2[200];
            foir_spec_keys(&rb, kk, okk);
            if (vstage == SPEC_SHAPE) foir_shape_resolve(&rb, &kk[0], NULL, NULL);
            fprintf(stderr, "seed %llu: variant %s/%s was not selected (state key %s, wanted %s)\n", (unsigned long long)seed,
                    vstage == SPEC_SHAPE ? "shape" : foir_spec_stage_names[vstage], want->name,
                    foir_spec_describe(vstage == SPEC_SHAPE ? SPEC_SHAPE : vstage, vstage == SPEC_SHAPE ? &kk[0] : &kk[vstage], b1, sizeof b1),
                    foir_spec_describe(vstage, &want->k, b2, sizeof b2));
            if (vstage == SPEC_SHAPE) dump_state(&ra);
            g_bad++;
            free(rb.hidden); rdp_destroy(&ra); free(bref); free(bn); free(pre);
            return 1;
        }
        g_spec_hits[vstage][vidx]++;
        for (int st = 0; st < SPEC_NSTAGES; st++) if (st != sstage && (rb.spec_mask & SPEC_BIT(st))) { g_spec_other++; break; }
        nk = &rb.ktab;
        if (relaxed) refk = &rb.kbase;
    }
    memcpy(mem_save, mem_a, RDRAM_BYTES);
    uint8_t *hid_save = malloc((RDRAM_BYTES >> 1) + 16);
    memcpy(hid_save, ra.hidden, (RDRAM_BYTES >> 1) + 16);

    int bad_before = g_bad;
    int n = bref->n;
    const char *stage = "";

    /* texture */
    if (ra.pf.need_tex) {
        memcpy(pre, bref, sizeof(*pre));
        refk->texture(&ra, bref);
        if (mask & 1) {
            stage = "texture";
            memcpy(bn, pre, sizeof(*bn));
            nk->texture(&rb, bn);
            int two = ra.om.cycle_type == CYCLE_TYPE_2;
            for (int i = 0; i < n; i++) {
                for (int c = 0; c < 4; c++) {
                    if (ra.pf.need_t0) CHK("tex0", bn->tex0[c][i], bref->tex0[c][i], i);
                    if (two ? ra.pf.need_t1 : ra.pf.next_texel) CHK("tex1", bn->tex1[c][i], bref->tex1[c][i], i);
                    if (ra.pf.next_texel) CHK("tex0n", bn->tex0n[c][i], bref->tex0n[c][i], i);
                }
                if (ra.om.dolod) CHK("lodfrac", bn->lodfrac[i], bref->lodfrac[i], i);
            }
            if (g_bad != bad_before) { fprintf(stderr, "seed %llu: %s\n", (unsigned long long)seed, stage); dump_state(&ra); goto done; }
        }
        /* keep the reference output as input for the next stage */
    } else {
        memset(bref->tex0, 0, sizeof(bref->tex0)); memset(bref->tex1, 0, sizeof(bref->tex1)); memset(bref->tex0n, 0, sizeof(bref->tex0n));
        memset(bref->lodfrac, 0, sizeof(bref->lodfrac));
    }

    /* combine */
    memcpy(pre, bref, sizeof(*pre));
    refk->combine(&ra, bref);
    if (mask & 2) {
        stage = "combine";
        memcpy(bn, pre, sizeof(*bn));
        if (g_spec_mode) foir_shape_export(&rb, bn);
        nk->combine(&rb, bn);
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 4; c++) { char nm[16]; snprintf(nm, sizeof nm, "pixel%d", c); CHK(nm, bn->pixel[c][i], bref->pixel[c][i], i); }
            CHK("acalpha", bn->acalpha[i], bref->acalpha[i], i);
            CHK("shade_alpha", bn->shade_alpha[i], bref->shade_alpha[i], i);
            CHK("cvg", bn->cvg[i], bref->cvg[i], i);
            CHK("cdith", bn->cdith[i], bref->cdith[i], i);
            CHK("adith", bn->adith[i], bref->adith[i], i);
        }
        if (g_bad != bad_before) { fprintf(stderr, "seed %llu: %s\n", (unsigned long long)seed, stage); dump_state(&ra); goto done; }
    }

    /* zcmp */
    memcpy(pre, bref, sizeof(*pre));
    refk->zcmp(&ra, bref);
    if (mask & 4) {
        stage = "zcmp";
        memcpy(bn, pre, sizeof(*bn));
        nk->zcmp(&rb, bn);
        for (int i = 0; i < n; i++) {
            CHK("wen", !!bn->wen[i], !!bref->wen[i], i);
            CHK("blend_en", !!bn->blend_en[i], !!bref->blend_en[i], i);
            CHK("prewrap", !!bn->prewrap[i], !!bref->prewrap[i], i);
            CHK("memcvg", bn->memcvg[i], bref->memcvg[i], i);
            CHK("cvg", bn->cvg[i], bref->cvg[i], i);
            CHK("sz", bn->sz[i], bref->sz[i], i);
            if (ra.om.real_blendshifts) {
                CHK("blshifta", bn->blshifta[i], bref->blshifta[i], i);
                CHK("blshiftb", bn->blshiftb[i], bref->blshiftb[i], i);
            }
            for (int c = 0; c < 4; c++) CHK("mem", bn->mem[c][i], bref->mem[c][i], i);
        }
        if (g_bad != bad_before) { fprintf(stderr, "seed %llu: %s\n", (unsigned long long)seed, stage); dump_state(&ra); goto done; }
    }

    /* blend */
    memcpy(pre, bref, sizeof(*pre));
    refk->blend(&ra, bref);
    if (mask & 8) {
        stage = "blend";
        memcpy(bn, pre, sizeof(*bn));
        nk->blend(&rb, bn);
        for (int i = 0; i < n; i++) {
            CHK("wen", !!bn->wen[i], !!bref->wen[i], i);
            if (bref->wen[i])
                for (int c = 0; c < 3; c++) CHK("fcol", bn->fcol[c][i], bref->fcol[c][i], i);
        }
        if (g_bad != bad_before) { fprintf(stderr, "seed %llu: %s\n", (unsigned long long)seed, stage); dump_state(&ra); goto done; }
    }

    /* store */
    refk->store(&ra, bref);
    if (mask & 16) {
        stage = "store";
        /* the blend stage above may have been skipped (mask) but bref holds the scalar chain's result */
        memcpy(bn, bref, sizeof(*bn));
        /* restore B memory to the pre-store state (it equals the initial state: only store writes) */
        nk->store(&rb, bn);
        for (uint32_t i = 0; i < 0x100000 && g_bad < 12; i += 4)
            if (memcmp(mem_a + FB_BASE - 0x8000 + i, mem_b + FB_BASE - 0x8000 + i, 4)) {
                fprintf(stderr, "  MISMATCH store: rdram word at 0x%x differs (neon=%08x scalar=%08x)\n", FB_BASE - 0x8000 + i,
                        *(uint32_t *)(mem_b + FB_BASE - 0x8000 + i), *(uint32_t *)(mem_a + FB_BASE - 0x8000 + i));
                g_bad++;
            }
        for (uint32_t i = 0; i < 0x100000 / 2 && g_bad < 12; i++)
            if (ra.hidden[(FB_BASE >> 1) - 0x4000 + i] != rb.hidden[(FB_BASE >> 1) - 0x4000 + i]) {
                fprintf(stderr, "  MISMATCH store: hidden byte %u differs (neon=%02x scalar=%02x)\n", (FB_BASE >> 1) - 0x4000 + i,
                        rb.hidden[(FB_BASE >> 1) - 0x4000 + i], ra.hidden[(FB_BASE >> 1) - 0x4000 + i]);
                g_bad++;
            }
        if (g_edge) g_bad += cmp_edges(&ra, &rb);
        g_checks++;
        if (g_bad != bad_before) { fprintf(stderr, "seed %llu: %s (edge placement %d)\n", (unsigned long long)seed, stage, g_edge); dump_state(&ra); }
    }
done:
    if (verbose) fprintf(stderr, "seed %llu done (%s)\n", (unsigned long long)seed, g_bad == bad_before ? "ok" : "BAD");
    free(rb.hidden);
    free(hid_save);
    rdp_destroy(&ra);
    free(bref); free(bn); free(pre);
    return g_bad != bad_before;
}


/* ---- COPY-cycle span kernel ---------------------------------------------------------------------- */

static long g_copy_kind[8], g_copy_wrote;

/* compare the framebuffer region of two contexts (RDRAM words and hidden bytes); returns the number of differences found */
static int cmp_fb(const struct rdp *ra, const struct rdp *rb, const char *what)
{
    int bad = 0;
    for (uint32_t i = 0; i < 0x100000 && bad < 6; i += 4)
        if (memcmp(ra->rdram + FB_BASE - 0x8000 + i, rb->rdram + FB_BASE - 0x8000 + i, 4)) {
            fprintf(stderr, "  MISMATCH %s: rdram word at 0x%x differs (neon=%08x scalar=%08x)\n", what, FB_BASE - 0x8000 + i,
                    *(const uint32_t *)(rb->rdram + FB_BASE - 0x8000 + i), *(const uint32_t *)(ra->rdram + FB_BASE - 0x8000 + i));
            bad++;
        }
    for (uint32_t i = 0; i < 0x100000 / 2 && bad < 6; i++)
        if (ra->hidden[(FB_BASE >> 1) - 0x4000 + i] != rb->hidden[(FB_BASE >> 1) - 0x4000 + i]) {
            fprintf(stderr, "  MISMATCH %s: hidden byte %u differs (neon=%02x scalar=%02x)\n", what, (FB_BASE >> 1) - 0x4000 + i,
                    rb->hidden[(FB_BASE >> 1) - 0x4000 + i], ra->hidden[(FB_BASE >> 1) - 0x4000 + i]);
            bad++;
        }
    return bad;
}

static int run_copy(uint64_t seed, int verbose)
{
    static struct rdp ra, rb;
    static uint8_t *mem_a, *mem_b;
    if (!mem_a) { mem_a = calloc(1, RDRAM_BYTES); mem_b = calloc(1, RDRAM_BYTES); }
    seed_rng(seed ^ 0xc0b1c0b1ull);
    struct foir_config cfg;
    foir_config_defaults(&cfg);
    cfg.neon = false;
    rdp_init(&ra, &cfg);
    rdp_attach_memory(&ra, mem_a, RDRAM_BYTES);
    const struct foir_kernels *nk = foir_kernels_neon();
    random_state(&ra);
    random_memory(&ra, 0);
    /* colour buffer: mostly 16 bit (the case the vector kernel takes), sometimes 8 / 4 bit; sometimes an odd halfword address */
    uint32_t fsize = rndn(10) ? PIXEL_SIZE_16BIT : (rndn(2) ? PIXEL_SIZE_8BIT : PIXEL_SIZE_4BIT);
    uint32_t fw = 16 + rndn(305);
    if (rndn(4) == 0) fw = 8 * (2 + rndn(38));
    cmd(&ra, (0x3fu << 24) | (fsize << 19) | (fw - 1), FB_BASE + (rndn(8) << 2) + (rndn(4) == 0 ? 2 : 0));
    cmd(&ra, 0x2fu << 24 | (2u << 20) | (rndn(2) ? 1u << 15 : 0) | (rndn(4) == 0 ? 1u << 19 : 0), rndn(2));   /* COPY cycle, TLUT, perspective, alpha compare */
    cmd(&ra, 0x27u << 24, 0);
    edge_placement(&ra);
    /* the primitive's tile: the common copy formats (RGBA16, CI8 / CI4 with TLUT) most of the time */
    int tn = (int)rndn(8);
    for (int k = 0; k < 8 && rndn(4); k++) {
        const struct tile *t = &ra.tile[(tn + k) & 7];
        if (ra.om.en_tlut || t->size == PIXEL_SIZE_16BIT) { tn = (tn + k) & 7; break; }
    }
    rdp_update_derived(&ra);
    struct prim_ctx *pc = &ra.pc;
    memset(pc, 0, sizeof(*pc));
    pc->tilenum = tn;
    pc->flip = rndn(12) ? 1 : 0;
    if (rndn(2)) { pc->ds = 0x800000; pc->dt = rndn(2) ? 0 : (int32_t)(rnd_coord() >> 12); pc->dw = 0; }
    else { pc->ds = rnd_coord() >> 8; pc->dt = rnd_coord() >> 8; pc->dw = rnd_coord() >> 10; }
    struct span sp;
    memset(&sp, 0, sizeof(sp));
    int y = (int)rndn(12);
    int xs = (int)rndn(fw + 6);
    if (rndn(5) == 0) xs = rndn(2) ? 0 : (int)fw - 1 - (int)rndn(10);
    int len = (int)rndn(rndn(2) ? 40 : (fw + 12));
    if (rndn(16) == 0) len = -1 - (int)rndn(3);
    if (rndn(16) == 0) xs = -(int)rndn(3);
    sp.rx = xs; sp.lx = xs + len;
    if (!pc->flip && rndn(2)) { sp.rx = xs + len; sp.lx = xs; }
    sp.validline = 1;
    sp.s = rnd_coord(); sp.t = rnd_coord(); sp.w = rndn(3) ? (int32_t)((rnd() & 0x3fff) << 16) : rnd_coord();
    if (rndn(2)) { sp.s = (int32_t)(rnd() & 0x7fffff) << 3; sp.t = (int32_t)(rnd() & 0x3fffff) << 3; }

    memcpy(&rb, &ra, sizeof(rb));
    rb.rdram = mem_b; rb.hidden = calloc(1, (RDRAM_BYTES >> 1) + 16);
    memcpy(mem_b, mem_a, RDRAM_BYTES);
    memcpy(rb.hidden, ra.hidden, (RDRAM_BYTES >> 1) + 16);
    rb.span_scratch = NULL;
    rb.k = nk;

    foir_kernels_scalar.copy(&ra, y, &sp, pc->flip);
    if (memcmp(mem_a + FB_BASE - 0x8000, mem_b + FB_BASE - 0x8000, 0x100000)) g_copy_wrote++;
    nk->copy(&rb, y, &sp, pc->flip);
    int bad = cmp_fb(&ra, &rb, "copy") + (g_edge ? cmp_edges(&ra, &rb) : 0);
    g_checks++;
    const struct tile *tl = &ra.tile[tn];
    g_copy_kind[ra.fb_size != PIXEL_SIZE_16BIT || !pc->flip ? 0 : ra.om.en_tlut ? 1 + (tl->size == PIXEL_SIZE_8BIT) + 2 * (tl->size == PIXEL_SIZE_4BIT) : tl->size == PIXEL_SIZE_16BIT ? 4 : 5]++;
    if (bad) {
        fprintf(stderr, "seed %llu: copy y=%d rx=%d lx=%d flip=%d s=%d t=%d w=%d ds=%d dt=%d dw=%d fb=%d/%d w=%d addr=0x%x acmp=%d tlut=%d persp=%d\n", (unsigned long long)seed,
                y, sp.rx, sp.lx, pc->flip, sp.s, sp.t, sp.w, pc->ds, pc->dt, pc->dw, ra.fb_format, ra.fb_size, ra.fb_width, ra.fb_address,
                ra.om.alpha_compare_en, ra.om.en_tlut, ra.om.persp_tex_en);
        dump_state(&ra);
        g_bad += bad;
    }
    if (verbose) fprintf(stderr, "seed %llu copy done (%s)\n", (unsigned long long)seed, bad ? "BAD" : "ok");
    free(rb.hidden);
    rdp_destroy(&ra);
    return bad != 0;
}

int main(int argc, char **argv)
{
    long iters = 1000;
    uint64_t seed0 = 1;
    int mask = 63, verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) iters = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed0 = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mask = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-S")) g_spec_mode = 1;
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) g_spec_only = atoi(argv[++i]);
        else { fprintf(stderr, "usage: kfuzz [-n iterations] [-s seed] [-m stage mask] [-v] [-S [-T table]]\n"); return 2; }
    }
    raster_init_tables();
    foir_spec_init();
    long failed = 0;
    for (long i = 0; i < iters; i++) {
        if ((mask & 31) && run_one(seed0 + (uint64_t)i, mask, verbose, i)) {
            failed++;
            if (failed >= 20) break;
        }
        if ((mask & 32) && !g_spec_mode && run_copy(seed0 + (uint64_t)i, verbose)) {
            failed++;
            if (failed >= 20) break;
        }
    }
    if ((mask & 32) && !g_spec_mode)
        printf("kfuzz copy: %ld spans wrote pixels; by kind: scalar fallback %ld, RGBA16 %ld, TLUT8 %ld, TLUT16 %ld, TLUT4 %ld, other texels %ld\n", g_copy_wrote,
               g_copy_kind[0], g_copy_kind[4], g_copy_kind[2], g_copy_kind[1], g_copy_kind[3], g_copy_kind[5]);
    if (g_spec_mode) {
        long nvar = 0, nmiss = 0;
        for (int st = 0; st <= SPEC_NSTAGES; st++)
            for (int v = 0; v < foir_spec_tabs[st].n; v++) {
                if (g_spec_only >= 0 && st != g_spec_only) continue;
                nvar++;
                if (!g_spec_hits[st][v]) { nmiss++; fprintf(stderr, "variant %s/%s was never exercised\n", st == SPEC_SHAPE ? "shape" : foir_spec_stage_names[st], foir_spec_tabs[st].ent[v].name); }
            }
        printf("kfuzz -S: %ld variants, %ld never exercised, %ld iterations also ran a variant of another stage\n", nvar, nmiss, g_spec_other);
        if (nmiss && iters >= nvar) failed++;
    }
    printf("kfuzz: %ld iterations, %ld failing, %ld value checks\n", iters, failed, g_checks);
    return failed ? 1 : 0;
}
