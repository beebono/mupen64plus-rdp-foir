/*
 * mupen64plus-rdp-foir - parser.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <time.h>
#include "rdp.h"
#include "mt.h"
#include "combreg.h"
#include "spec.h"

#ifdef FOIR_FRAMELOG
#define FL_LOAD(r) ((r)->stat_loads++)
#else
#define FL_LOAD(r) ((void)0)
#endif

static uint8_t cmd_length_bytes[64];

static void init_cmd_lengths(void)
{
    static bool done;
    if (done) return;
    for (int i = 0; i < 64; i++) cmd_length_bytes[i] = 8;
    cmd_length_bytes[CMD_FILL_TRI] = 32;
    cmd_length_bytes[CMD_FILL_ZBUF_TRI] = 48;
    cmd_length_bytes[CMD_TEX_TRI] = 96;
    cmd_length_bytes[CMD_TEX_ZBUF_TRI] = 112;
    cmd_length_bytes[CMD_SHADE_TRI] = 96;
    cmd_length_bytes[CMD_SHADE_ZBUF_TRI] = 112;
    cmd_length_bytes[CMD_SHADE_TEX_TRI] = 160;
    cmd_length_bytes[CMD_SHADE_TEX_ZBUF_TRI] = 176;
    cmd_length_bytes[CMD_TEX_RECT] = 16;
    cmd_length_bytes[CMD_TEX_RECT_FLIP] = 16;
    done = true;
}

static int reg_in_group(int idx, int base) { return idx >= base && idx < base + 4; }

static void combiner_resolve(struct rdp *r)
{
    const int *c = r->comb.raw;
    /* raw: [0..3] suba, subb, mul, add of rgb0; [4..7] same for alpha0; [8..15] cycle 1 (selector -> register mapping: combreg.h) */
    for (int cy = 0; cy < 2; cy++) {
        for (int ch = 0; ch < 3; ch++)
            for (int i = 0; i < 4; i++)
                r->comb.rgb[cy][i][ch] = (uint8_t)comb_reg_rgb(i, c[cy * 8 + i], ch);
        for (int i = 0; i < 4; i++)
            r->comb.alpha[cy][i] = (uint8_t)comb_reg_alpha(i, c[cy * 8 + 4 + i]);
    }
}

void rdp_update_derived(struct rdp *r)
{
    struct other_modes *om = &r->om;
    combiner_resolve(r);

    int first = (om->cycle_type == CYCLE_TYPE_2) ? 0 : 1;
    int t0 = 0, t1 = 0, lodf = 0, noise = 0;
    for (int cy = first; cy < 2; cy++) {
        for (int i = 0; i < 4; i++) {
            for (int ch = 0; ch < 3; ch++) {
                int idx = r->comb.rgb[cy][i][ch];
                t0 |= reg_in_group(idx, REG_TEX0);
                t1 |= reg_in_group(idx, REG_TEX1);
                lodf |= (idx == REG_LODF);
                noise |= (idx == REG_NOISE);
            }
            int idx = r->comb.alpha[cy][i];
            t0 |= reg_in_group(idx, REG_TEX0);
            t1 |= reg_in_group(idx, REG_TEX1);
            lodf |= (idx == REG_LODF);
        }
    }
    om->uses_texel0 = t0;
    om->uses_texel1 = t1;
    om->uses_noise = noise;
    om->dolod = om->tex_lod_en || lodf;
    om->rgb_alpha_dither = (om->rgb_dither_sel << 2) | om->alpha_dither_sel;
    om->partialreject[0] = (om->blend_m2b[0] == 0 && om->blend_m1b[0] == 0);
    om->partialreject[1] = (om->blend_m2b[1] == 0 && om->blend_m1b[1] == 0);
    om->real_blendshifts = (om->cycle_type == CYCLE_TYPE_1) ? (om->blend_m2b[0] == 1)
                         : (om->cycle_type == CYCLE_TYPE_2) ? (om->blend_m2b[0] == 1 || om->blend_m2b[1] == 1) : 0;

    struct pipe_flags *pf = &r->pf;
    int two = om->cycle_type == CYCLE_TYPE_2;
    int sh_rgb = 0, sh_a = 0, c1t1 = 0;
    int refs0[2] = { 0, 0 }, refs1[2] = { 0, 0 };
    for (int cy = first; cy < 2; cy++) {
        for (int i = 0; i < 4; i++) {
            for (int ch = 0; ch < 3; ch++) {
                int idx = r->comb.rgb[cy][i][ch];
                sh_rgb |= reg_in_group(idx, REG_SHADE);
                refs0[cy] |= reg_in_group(idx, REG_TEX0);
                refs1[cy] |= reg_in_group(idx, REG_TEX1);
                if (cy == 1) c1t1 |= reg_in_group(idx, REG_TEX1);
            }
            int idx = r->comb.alpha[cy][i];
            sh_a |= (idx == REG_SHADE + 3);
            refs0[cy] |= reg_in_group(idx, REG_TEX0);
            refs1[cy] |= reg_in_group(idx, REG_TEX1);
            if (cy == 1) c1t1 |= reg_in_group(idx, REG_TEX1);
        }
    }
    /* the blender can read the shade alpha; the combiner's shade rgb is needed for the colour only */
    int bl_sha = 0;
    for (int cy = 0; cy < (two ? 2 : 1); cy++) bl_sha |= (om->blend_m1b[cy] == 2);
    pf->shade_rgb = (uint8_t)sh_rgb;
    pf->shade_a = (uint8_t)(sh_a | bl_sha);
    pf->need_tex = (uint8_t)(t0 || t1 || om->dolod);
    pf->next_texel = (uint8_t)(c1t1 && pf->need_tex);
    /* two-cycle: cycle 1 sees TEXEL0 = the second texture cycle, TEXEL1 = TEXEL0 of the next pixel; one-cycle: only TEXEL0 / next-pixel TEXEL1 of the single cycle */
    pf->need_t0 = (uint8_t)(two ? refs0[0] : refs0[1]);
    pf->need_t1 = (uint8_t)(two && (refs1[0] || refs0[1]));
    pf->need_z = (uint8_t)(om->z_compare_en || om->z_update_en);
    pf->need_cz = pf->need_z;
    pf->rng = (uint8_t)(noise || om->alpha_dither_sel == 2 || om->rgb_dither_sel == 2 || om->dither_alpha_en);
    r->state_dirty = false;
    r->rs_dirty = true;
    r->spec_dirty = true;
}

static void cmd_set_other_modes(struct rdp *r, const uint32_t *a)
{
    struct other_modes *om = &r->om;
    om->cycle_type       = (a[0] >> 20) & 3;
    om->persp_tex_en     = (a[0] >> 19) & 1;
    om->detail_tex_en    = (a[0] >> 18) & 1;
    om->sharpen_tex_en   = (a[0] >> 17) & 1;
    om->tex_lod_en       = (a[0] >> 16) & 1;
    om->en_tlut          = (a[0] >> 15) & 1;
    om->tlut_type        = (a[0] >> 14) & 1;
    om->sample_type      = (a[0] >> 13) & 1;
    om->mid_texel        = (a[0] >> 12) & 1;
    om->bi_lerp0         = (a[0] >> 11) & 1;
    om->bi_lerp1         = (a[0] >> 10) & 1;
    om->convert_one      = (a[0] >>  9) & 1;
    om->key_en           = (a[0] >>  8) & 1;
    om->rgb_dither_sel   = (a[0] >>  6) & 3;
    om->alpha_dither_sel = (a[0] >>  4) & 3;
    om->blend_m1a[0]     = (a[1] >> 30) & 3;
    om->blend_m1a[1]     = (a[1] >> 28) & 3;
    om->blend_m1b[0]     = (a[1] >> 26) & 3;
    om->blend_m1b[1]     = (a[1] >> 24) & 3;
    om->blend_m2a[0]     = (a[1] >> 22) & 3;
    om->blend_m2a[1]     = (a[1] >> 20) & 3;
    om->blend_m2b[0]     = (a[1] >> 18) & 3;
    om->blend_m2b[1]     = (a[1] >> 16) & 3;
    om->force_blend      = (a[1] >> 14) & 1;
    om->alpha_cvg_select = (a[1] >> 13) & 1;
    om->cvg_times_alpha  = (a[1] >> 12) & 1;
    om->z_mode           = (a[1] >> 10) & 3;
    om->cvg_dest         = (a[1] >>  8) & 3;
    om->color_on_cvg     = (a[1] >>  7) & 1;
    om->image_read_en    = (a[1] >>  6) & 1;
    om->z_update_en      = (a[1] >>  5) & 1;
    om->z_compare_en     = (a[1] >>  4) & 1;
    om->antialias_en     = (a[1] >>  3) & 1;
    om->z_source_sel     = (a[1] >>  2) & 1;
    om->dither_alpha_en  = (a[1] >>  1) & 1;
    om->alpha_compare_en = (a[1] >>  0) & 1;
    r->state_dirty = true;
}

static void cmd_set_combine(struct rdp *r, const uint32_t *a)
{
    int *c = r->comb.raw;
    c[0]  = (a[0] >> 20) & 0xf;
    c[2]  = (a[0] >> 15) & 0x1f;
    c[4]  = (a[0] >> 12) & 0x7;
    c[6]  = (a[0] >>  9) & 0x7;
    c[8]  = (a[0] >>  5) & 0xf;
    c[10] = (a[0] >>  0) & 0x1f;
    c[1]  = (a[1] >> 28) & 0xf;
    c[9]  = (a[1] >> 24) & 0xf;
    c[12] = (a[1] >> 21) & 0x7;
    c[14] = (a[1] >> 18) & 0x7;
    c[3]  = (a[1] >> 15) & 0x7;
    c[5]  = (a[1] >> 12) & 0x7;
    c[7]  = (a[1] >>  9) & 0x7;
    c[11] = (a[1] >>  6) & 0x7;
    c[13] = (a[1] >>  3) & 0x7;
    c[15] = (a[1] >>  0) & 0x7;
    r->state_dirty = true;
}

static void unpack_rgba(struct color *c, uint32_t v)
{
    c->r = (v >> 24) & 0xff; c->g = (v >> 16) & 0xff; c->b = (v >> 8) & 0xff; c->a = v & 0xff;
}

static void cmd_set_tile(struct rdp *r, const uint32_t *a)
{
    struct tile *t = &r->tile[(a[1] >> 24) & 7];
    t->format  = (a[0] >> 21) & 7;
    t->size    = (a[0] >> 19) & 3;
    t->line    = (a[0] >>  9) & 0x1ff;
    t->tmem    = (a[0] >>  0) & 0x1ff;
    t->palette = (a[1] >> 20) & 0xf;
    t->ct      = (a[1] >> 19) & 1;
    t->mt      = (a[1] >> 18) & 1;
    t->mask_t  = (a[1] >> 14) & 0xf;
    t->shift_t = (a[1] >> 10) & 0xf;
    t->cs      = (a[1] >>  9) & 1;
    t->ms      = (a[1] >>  8) & 1;
    t->mask_s  = (a[1] >>  4) & 0xf;
    t->shift_s = (a[1] >>  0) & 0xf;
    tile_update_derived(t);
}

static void cmd_set_tile_size(struct rdp *r, const uint32_t *a)
{
    struct tile *t = &r->tile[(a[1] >> 24) & 7];
    t->sl = (a[0] >> 12) & 0xfff;
    t->tl = (a[0] >>  0) & 0xfff;
    t->sh = (a[1] >> 12) & 0xfff;
    t->th = (a[1] >>  0) & 0xfff;
    tile_update_derived(t);
}

static void cmd_set_convert(struct rdp *r, const uint32_t *a)
{
    int32_t k0 = (a[0] >> 13) & 0x1ff;
    int32_t k1 = (a[0] >> 4) & 0x1ff;
    int32_t k2 = ((a[0] & 0xf) << 5) | ((a[1] >> 27) & 0x1f);
    int32_t k3 = (a[1] >> 18) & 0x1ff;
    r->k0_tf = (SIGN(k0, 9) << 1) + 1;
    r->k1_tf = (SIGN(k1, 9) << 1) + 1;
    r->k2_tf = (SIGN(k2, 9) << 1) + 1;
    r->k3_tf = (SIGN(k3, 9) << 1) + 1;
    r->k4 = (a[1] >> 9) & 0x1ff;
    r->k5 = a[1] & 0x1ff;
}

uint32_t rdp_command_words(uint32_t first_word)
{
    init_cmd_lengths();
    return cmd_length_bytes[(first_word >> 24) & 0x3f] >> 2;
}

void rdp_run_command(struct rdp *r, const uint32_t *a)
{
    unsigned id = (a[0] >> 24) & 0x3f;
    r->frame_cmds++;
    if (r->trace) r->trace(r->trace_ctx, a, rdp_command_words(a[0]));
    if (r->state_dirty && (id < 0x10 || id == CMD_TEX_RECT || id == CMD_TEX_RECT_FLIP || id == CMD_FILL_RECT))
        rdp_update_derived(r);

    if (!(id < 0x10 || id == CMD_TEX_RECT || id == CMD_TEX_RECT_FLIP || id == CMD_FILL_RECT ||
          id == CMD_NOOP || id == CMD_SYNC_LOAD || id == CMD_SYNC_PIPE || id == CMD_SYNC_TILE || id == CMD_SYNC_FULL))
        r->rs_dirty = r->spec_dirty = true;

    switch (id) {
    case CMD_FILL_TRI: case CMD_FILL_ZBUF_TRI: case CMD_TEX_TRI: case CMD_TEX_ZBUF_TRI:
    case CMD_SHADE_TRI: case CMD_SHADE_ZBUF_TRI: case CMD_SHADE_TEX_TRI: case CMD_SHADE_TEX_ZBUF_TRI:
        setup_triangle(r, a);
        break;
    case CMD_TEX_RECT:      setup_texrect(r, a, false); break;
    case CMD_TEX_RECT_FLIP: setup_texrect(r, a, true); break;
    case CMD_FILL_RECT:     setup_fillrect(r, a); break;

    case CMD_SYNC_FULL:
        if (r->mt) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            rdp_sync(r);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            r->sync_wait_ms += (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        }
        r->frame_seed++;
        if (r->frame_end_cb) r->frame_end_cb(r->frame_end_ctx);
        if (r->mi_intr) *r->mi_intr |= MI_INTR_DP;
        if (r->check_interrupts) r->check_interrupts();
        break;
    case CMD_NOOP: case CMD_SYNC_LOAD: case CMD_SYNC_PIPE: case CMD_SYNC_TILE:
        break;

    case CMD_SET_KEY_GB:
        r->key_width.g = (a[0] >> 12) & 0xfff;
        r->key_width.b = a[0] & 0xfff;
        r->key_center.g = (a[1] >> 24) & 0xff;
        r->key_scale.g = (a[1] >> 16) & 0xff;
        r->key_center.b = (a[1] >> 8) & 0xff;
        r->key_scale.b = a[1] & 0xff;
        break;
    case CMD_SET_KEY_R:
        r->key_width.r = (a[1] >> 16) & 0xfff;
        r->key_center.r = (a[1] >> 8) & 0xff;
        r->key_scale.r = a[1] & 0xff;
        break;
    case CMD_SET_CONVERT:  cmd_set_convert(r, a); break;
    case CMD_SET_SCISSOR:
        r->clip.xh = (a[0] >> 12) & 0xfff;
        r->clip.yh = (a[0] >>  0) & 0xfff;
        r->clip.xl = (a[1] >> 12) & 0xfff;
        r->clip.yl = (a[1] >>  0) & 0xfff;
        r->scfield = (a[1] >> 25) & 1;
        r->sckeepodd = (a[1] >> 24) & 1;
        break;
    case CMD_SET_PRIM_DEPTH:
        r->primitive_z = a[1] & (0x7fffu << 16);
        r->primitive_delta_z = (uint16_t)a[1];
        break;
    case CMD_SET_OTHER_MODES: cmd_set_other_modes(r, a); break;

    case CMD_LOAD_TLUT:  FL_LOAD(r); tmem_load_tlut(r, a); break;
    case CMD_LOAD_BLOCK: FL_LOAD(r); tmem_load_block(r, a); break;
    case CMD_LOAD_TILE:  FL_LOAD(r); tmem_load_tile(r, a); break;
    case CMD_SET_TILE_SIZE: cmd_set_tile_size(r, a); break;
    case CMD_SET_TILE:   cmd_set_tile(r, a); break;

    case CMD_SET_FILL_COLOR:  r->fill_color = a[1]; break;
    case CMD_SET_FOG_COLOR:   unpack_rgba(&r->fog_color, a[1]); break;
    case CMD_SET_BLEND_COLOR: unpack_rgba(&r->blend_color, a[1]); break;
    case CMD_SET_PRIM_COLOR:
        r->min_level = (a[0] >> 8) & 0x1f;
        r->primitive_lod_frac = a[0] & 0xff;
        unpack_rgba(&r->prim_color, a[1]);
        break;
    case CMD_SET_ENV_COLOR:   unpack_rgba(&r->env_color, a[1]); break;
    case CMD_SET_COMBINE:     cmd_set_combine(r, a); break;

    case CMD_SET_TEXTURE_IMAGE:
        r->ti_format = (a[0] >> 21) & 7;
        r->ti_size   = (a[0] >> 19) & 3;
        r->ti_width  = (a[0] & 0x3ff) + 1;
        r->ti_address = a[1] & 0x0ffffff;
        break;
    case CMD_SET_MASK_IMAGE:  r->zb_address = a[1] & 0x0ffffff; break;
    case CMD_SET_COLOR_IMAGE:
        r->fb_format  = (a[0] >> 21) & 7;
        r->fb_size    = (a[0] >> 19) & 3;
        r->fb_width   = (a[0] & 0x3ff) + 1;
        r->fb_address = a[1] & 0x0ffffff;
        break;
    default:
        break;
    }
}

void rdp_process_list(struct rdp *r)
{
    init_cmd_lengths();
    if (r->crashed || !r->dp_current || !r->dp_end) return;

    uint32_t cur = (*r->dp_current & ~7u) >> 2;
    uint32_t end = (*r->dp_end & ~7u) >> 2;
    if (end <= cur) return;

    bool xbus = r->dp_status && (*r->dp_status & DP_STATUS_XBUS_DMA);
    const uint32_t *dmem = (const uint32_t *)r->dmem;

    while (end > cur) {
        if (r->cmd_pos == 0) {
            uint32_t w0 = xbus ? dmem[cur & 0x3ff] : rdram_rd32(r, cur);
            cur++;
            r->cmd_buf[r->cmd_pos++] = w0;
            r->cmd_len = cmd_length_bytes[(w0 >> 24) & 0x3f] >> 2;
        }
        uint32_t toload = end - cur;
        if (toload > r->cmd_len - r->cmd_pos) toload = r->cmd_len - r->cmd_pos;
        for (uint32_t i = 0; i < toload; i++) {
            r->cmd_buf[r->cmd_pos++] = xbus ? dmem[cur & 0x3ff] : rdram_rd32(r, cur);
            cur++;
        }
        if (r->cmd_pos == r->cmd_len) {
            rdp_run_command(r, r->cmd_buf);
            r->cmd_pos = 0;
            if (r->crashed) break;
        }
    }

    *r->dp_start = *r->dp_current = *r->dp_end;
}

void rdp_init(struct rdp *r, const struct foir_config *cfg)
{
    memset(r, 0, sizeof(*r));
    init_cmd_lengths();
    r->cfg = *cfg;
    r->clip.xh = 0x2000;
    r->clip.yh = 0x2000;
    r->fb_size = PIXEL_SIZE_4BIT;
    r->kbase = foir_kernels_scalar;
    r->spec_ok_mask = 0;
    if (cfg->neon) {
        const struct foir_kernels *nk = foir_kernels_neon();
        if (nk) {
            r->kbase = *nk;
            r->spec_ok_mask = foir_kernels_neon_mask() & 0x1f;
            if (!cfg->fast_combiner) {
                r->kbase.combine = foir_kernels_scalar.combine;
                r->kbase.blend = foir_kernels_scalar.blend;
                r->spec_ok_mask &= ~(SPEC_BIT(SPEC_COMB) | SPEC_BIT(SPEC_BLEND));
            }
        }
    }
    foir_spec_init();
    r->ktab = r->kbase;
    r->k = &r->ktab;
    r->spec_dirty = true;
    for (int i = 0; i < 8; i++) tile_update_derived(&r->tile[i]);
    uint32_t zero[2] = { 0, 0 };
    cmd_set_other_modes(r, zero);
    cmd_set_combine(r, zero);
    rdp_update_derived(r);
    raster_init_tables();
}

void rdp_attach_memory(struct rdp *r, uint8_t *rdram, uint32_t size)
{
    free(r->hidden);
    r->rdram = rdram;
    r->rdram_size = size;
    r->hidden = (uint8_t *)calloc(((size_t)size >> 1) + 16, 1);
}

bool rdp_start_workers(struct rdp *r, int nworkers)
{
    if (r->mt) mt_stop(r);
    if (nworkers <= 0) return false;
    return mt_start(r, nworkers);
}

void rdp_sync(struct rdp *r)
{
    if (r->mt) mt_sync(r);
}

uint32_t rdp_pixels(struct rdp *r)
{
    return r->mt ? mt_pixels(r) : r->stat_pixels;
}

void rdp_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all)
{
    if (r->mt) { mt_coverage(r, pix, tex_all); return; }
    for (int k = 0; k < COV_N; k++) pix[k] = r->cov_pix[k];
    *tex_all = r->cov_tex_all;
}

void rdp_destroy(struct rdp *r)
{
    if (r->mt) mt_stop(r);
    free(r->hidden);
    r->hidden = NULL;
}
