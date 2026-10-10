/*
 * mupen64plus-rdp-foir - kernels_scalar.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kernels_priv.h"

uint32_t k_clamp9[512];
int32_t k_ext9[512];

static uint16_t z_com_table[0x40000];
static uint32_t z_complete_dec_table[0x4000];
static uint16_t deltaz_comparator_lut[0x10000];
uint8_t k_bldiv[0x8000];

static const uint8_t bayer_matrix[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };
static const uint8_t magic_matrix[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };

/* hash of the framebuffer index and a per-primitive seed: independent of batching and thread (the hardware LFSR is not reproduced) */
static inline uint32_t prand(uint32_t idx, uint32_t seed, uint32_t salt)
{
    uint32_t h = idx * 0x9e3779b1u ^ seed ^ (salt * 0x85ebca77u);
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}

void kernels_scalar_init_tables(void)
{
    static bool done;
    if (done) return;

    for (int i = 0; i < 0x200; i++) {
        switch ((i >> 7) & 3) {
        case 0: case 1: k_clamp9[i] = i & 0xff; break;
        case 2: k_clamp9[i] = 0xff; break;
        default: k_clamp9[i] = 0; break;
        }
        k_ext9[i] = ((i & 0x180) == 0x180) ? (i | ~0x1ff) : (i & 0x1ff);
    }

    for (int z = 0; z < 0x40000; z++) {
        uint16_t alt;
        int top = (z >> 11) & 0x7f;
        if (top < 0x40) alt = (z >> 4) & 0x1ffc;
        else if (top < 0x60) alt = ((z >> 3) & 0x1ffc) | 0x2000;
        else if (top < 0x70) alt = ((z >> 2) & 0x1ffc) | 0x4000;
        else if (top < 0x78) alt = ((z >> 1) & 0x1ffc) | 0x6000;
        else if (top < 0x7c) alt = (z & 0x1ffc) | 0x8000;
        else if (top < 0x7e) alt = ((z << 1) & 0x1ffc) | 0xa000;
        else if (top == 0x7e) alt = ((z << 2) & 0x1ffc) | 0xc000;
        else alt = ((z << 2) & 0x1ffc) | 0xe000;
        z_com_table[z] = alt;
    }

    static const struct { uint32_t shift, add; } z_dec[8] = {
        { 6, 0x00000 }, { 5, 0x20000 }, { 4, 0x30000 }, { 3, 0x38000 },
        { 2, 0x3c000 }, { 1, 0x3e000 }, { 0, 0x3f000 }, { 0, 0x3f800 },
    };
    for (int i = 0; i < 0x4000; i++) {
        uint32_t exponent = (i >> 11) & 7, mantissa = i & 0x7ff;
        z_complete_dec_table[i] = ((mantissa << z_dec[exponent].shift) + z_dec[exponent].add) & 0x3ffff;
    }

    deltaz_comparator_lut[0] = 0;
    for (int i = 1; i < 0x10000; i++)
        for (int k = 15; k >= 0; k--)
            if (i & (1 << k)) { deltaz_comparator_lut[i] = (uint16_t)(1 << k); break; }

    for (int i = 0; i < 0x8000; i++) {
        int res = 0, ps[9];
        int d = (i >> 11) & 0xf, n = i & 0x7ff, invd = (~d) & 0xf;
        int temp = invd + (n >> 8) + 1;
        ps[0] = temp & 7;
        for (int k = 0; k < 8; k++) {
            int nbit = (n >> (7 - k)) & 1;
            if (res & (0x100 >> k)) temp = invd + (ps[k] << 1) + nbit + 1;
            else temp = d + (ps[k] << 1) + nbit;
            ps[k + 1] = temp & 7;
            if (temp & 0x10) res |= (1 << (7 - k));
        }
        k_bldiv[i] = (uint8_t)res;
    }

    kernels_tex_init_tables();
    done = true;
}

static void get_dither_noise(struct rdp *r, int idx, uint32_t fbidx, int32_t noise, int *cdith, int *adith)
{
    switch (r->om.rgb_alpha_dither) {
    case 0:  *adith = *cdith = magic_matrix[idx]; break;
    case 1:  *cdith = magic_matrix[idx]; *adith = (~*cdith) & 7; break;
    case 2:  *cdith = magic_matrix[idx]; *adith = (noise >> 6) & 7; break;
    case 3:  *cdith = magic_matrix[idx]; *adith = 0; break;
    case 4:  *adith = *cdith = bayer_matrix[idx]; break;
    case 5:  *cdith = bayer_matrix[idx]; *adith = (~*cdith) & 7; break;
    case 6:  *cdith = bayer_matrix[idx]; *adith = (noise >> 6) & 7; break;
    case 7:  *cdith = bayer_matrix[idx]; *adith = 0; break;
    case 8:  *cdith = (int)(prand(fbidx, r->pc.seed, 2) & 0x7fff); *adith = magic_matrix[idx]; break;
    case 9:  *cdith = (int)(prand(fbidx, r->pc.seed, 2) & 0x7fff); *adith = (~magic_matrix[idx]) & 7; break;
    case 10: *cdith = (int)(prand(fbidx, r->pc.seed, 2) & 0x7fff); *adith = (noise >> 6) & 7; break;
    case 11: *cdith = (int)(prand(fbidx, r->pc.seed, 2) & 0x7fff); *adith = 0; break;
    case 12: *cdith = 7; *adith = bayer_matrix[idx]; break;
    case 13: *cdith = 7; *adith = (~bayer_matrix[idx]) & 7; break;
    case 14: *cdith = 7; *adith = (noise >> 6) & 7; break;
    default: *cdith = 7; *adith = 0; break;
    }
}

static void rgb_dither(int sel, int32_t *r, int32_t *g, int32_t *b, int dith)
{
    int32_t newr = *r, newg = *g, newb = *b, rcomp, gcomp, bcomp;
    newr = newr > 247 ? 255 : (newr & 0xf8) + 8;
    newg = newg > 247 ? 255 : (newg & 0xf8) + 8;
    newb = newb > 247 ? 255 : (newb & 0xf8) + 8;
    if (sel != 2) { rcomp = gcomp = bcomp = dith; }
    else { rcomp = dith & 7; gcomp = (dith >> 3) & 7; bcomp = (dith >> 6) & 7; }

    int32_t replace = (rcomp - (*r & 7)) >> 31;
    *r += (newr - *r) & replace;
    replace = (gcomp - (*g & 7)) >> 31;
    *g += (newg - *g) & replace;
    replace = (bcomp - (*b & 7)) >> 31;
    *b += (newb - *b) & replace;
}

static inline int32_t ccomb(int32_t a, int32_t b, int32_t c, int32_t d)
{
    a = k_ext9[a & 0x1ff];
    b = k_ext9[b & 0x1ff];
    c = (c & 0x1ff) | -(c & 0x100);
    d = k_ext9[d & 0x1ff];
    return (((a - b) * c) + (d << 8) + 0x80) & 0x1ffff;
}

static inline int32_t acomb(int32_t a, int32_t b, int32_t c, int32_t d)
{
    a = k_ext9[a & 0x1ff];
    b = k_ext9[b & 0x1ff];
    c = (c & 0x1ff) | -(c & 0x100);
    d = k_ext9[d & 0x1ff];
    return ((((a - b) * c) + (d << 8) + 0x80) >> 8) & 0x1ff;
}

static int32_t chroma_key_min(const struct rdp *r, const int32_t rgb[3])
{
    int32_t key[3];
    const int32_t width[3] = { r->key_width.r, r->key_width.g, r->key_width.b };
    for (int i = 0; i < 3; i++) {
        int32_t k = SIGN(rgb[i], 17);
        if (k > 0) k = ((k & 0xf) == 8) ? (-k + 0x10) : (-k);
        key[i] = (width[i] << 4) + k;
    }
    int32_t ka = key[0] < key[1] ? key[0] : key[1];
    ka = key[2] < ka ? key[2] : ka;
    return foir_clamp(ka, 0, 0xff);
}

/* rgb gets 17-bit results, alpha 9-bit */
static void comb_eval(const struct rdp *r, const int32_t *R, int cy, int32_t rgb[3], int32_t *alpha)
{
    for (int ch = 0; ch < 3; ch++) {
        const uint8_t *s0 = &r->comb.rgb[cy][0][ch];
        (void)s0;
        int ia = r->comb.rgb[cy][0][ch], ib = r->comb.rgb[cy][1][ch];
        int im = r->comb.rgb[cy][2][ch], id = r->comb.rgb[cy][3][ch];
        if (im != REG_ZERO)
            rgb[ch] = ccomb(R[ia], R[ib], R[im], R[id]);
        else
            rgb[ch] = ((k_ext9[R[id] & 0x1ff] << 8) + 0x80) & 0x1ffff;
    }
    int ia = r->comb.alpha[cy][0], ib = r->comb.alpha[cy][1];
    int im = r->comb.alpha[cy][2], id = r->comb.alpha[cy][3];
    if (im != REG_ZERO)
        *alpha = acomb(R[ia], R[ib], R[im], R[id]);
    else
        *alpha = k_ext9[R[id] & 0x1ff] & 0x1ff;
}

static void comb_finish(const struct rdp *r, const int32_t *R, int cy, int adseed,
                        int32_t rgb[3], int32_t a9, uint8_t *cvg, struct color *pix, int32_t *comb_rgb9)
{
    const struct other_modes *om = &r->om;
    int32_t chromabypass[3] = { 0, 0, 0 };
    int32_t temp = 0, keyalpha = 0;

    if (om->key_en)
        for (int ch = 0; ch < 3; ch++) chromabypass[ch] = R[r->comb.rgb[cy][0][ch]];

    pix->a = (int32_t)k_clamp9[a9 & 0x1ff];
    if (pix->a == 0xff) pix->a = 0x100;

    if (!om->key_en) {
        for (int ch = 0; ch < 3; ch++) comb_rgb9[ch] = rgb[ch] >> 8;
        pix->r = (int32_t)k_clamp9[comb_rgb9[0] & 0x1ff];
        pix->g = (int32_t)k_clamp9[comb_rgb9[1] & 0x1ff];
        pix->b = (int32_t)k_clamp9[comb_rgb9[2] & 0x1ff];
    } else {
        keyalpha = chroma_key_min(r, rgb);
        pix->r = (int32_t)k_clamp9[chromabypass[0] & 0x1ff];
        pix->g = (int32_t)k_clamp9[chromabypass[1] & 0x1ff];
        pix->b = (int32_t)k_clamp9[chromabypass[2] & 0x1ff];
        for (int ch = 0; ch < 3; ch++) comb_rgb9[ch] = rgb[ch] >> 8;
    }

    if (om->cvg_times_alpha) {
        temp = (pix->a * (*cvg) + 4) >> 3;
        *cvg = (uint8_t)((temp >> 5) & 0xf);
    }

    if (!om->alpha_cvg_select) {
        if (!om->key_en) {
            pix->a += adseed;
            if (pix->a & 0x100) pix->a = 0xff;
        } else {
            pix->a = keyalpha;
        }
    } else {
        pix->a = om->cvg_times_alpha ? temp : (*cvg) << 5;
        if (pix->a > 0xff) pix->a = 0xff;
    }
}

static void scalar_combine(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    const struct prim_ctx *pc = &r->pc;
    int two = om->cycle_type == CYCLE_TYPE_2;
    int need_noise = om->uses_noise || om->alpha_dither_sel == 2;
    int32_t R[REG_COUNT];
    memset(R, 0, sizeof(R));
    R[REG_ONE] = 0x100;
    R[REG_K4] = r->k4; R[REG_K5] = r->k5;
    R[REG_KEYC + 0] = r->key_center.r; R[REG_KEYC + 1] = r->key_center.g; R[REG_KEYC + 2] = r->key_center.b;
    R[REG_KEYS + 0] = r->key_scale.r;  R[REG_KEYS + 1] = r->key_scale.g;  R[REG_KEYS + 2] = r->key_scale.b;
    R[REG_PLODF] = r->primitive_lod_frac;
    R[REG_PRIM + 0] = r->prim_color.r; R[REG_PRIM + 1] = r->prim_color.g;
    R[REG_PRIM + 2] = r->prim_color.b; R[REG_PRIM + 3] = r->prim_color.a;
    R[REG_ENV + 0] = r->env_color.r; R[REG_ENV + 1] = r->env_color.g;
    R[REG_ENV + 2] = r->env_color.b; R[REG_ENV + 3] = r->env_color.a;
    const bool use_shade_rgb = r->pf.shade_rgb, use_shade_a = r->pf.shade_a;

    for (int i = 0; i < b->n; i++) {
        uint8_t cvg = b->cvg[i];
        int offx = b->offx[i], offy = b->offy[i];

        struct color shade = { 0, 0, 0, 0 };
        if (use_shade_rgb || use_shade_a) {
            int32_t sr = b->cr[i] >> 14, sg = b->cg[i] >> 14, sb = b->cb[i] >> 14, sa = b->ca[i] >> 14;
            if (cvg == 8) {
                sr >>= 2; sg >>= 2; sb >>= 2; sa >>= 2;
            } else {
                sr = ((sr << 2) + offx * pc->cdr + offy * pc->drdy) >> 4;
                sg = ((sg << 2) + offx * pc->cdg + offy * pc->dgdy) >> 4;
                sb = ((sb << 2) + offx * pc->cdb + offy * pc->dbdy) >> 4;
                sa = ((sa << 2) + offx * pc->cda + offy * pc->dady) >> 4;
            }
            shade.r = (int32_t)k_clamp9[sr & 0x1ff];
            shade.g = (int32_t)k_clamp9[sg & 0x1ff];
            shade.b = (int32_t)k_clamp9[sb & 0x1ff];
            shade.a = (int32_t)k_clamp9[sa & 0x1ff];
        }

        int32_t noise = 0;
        if (need_noise) {
            noise = ((int32_t)(prand(b->fbidx[i], r->pc.seed, 1) & 7) << 6) | 0x20;
            R[REG_NOISE] = noise;
        }
        int cdith, adith;
        get_dither_noise(r, b->ditidx[i], b->fbidx[i], noise, &cdith, &adith);
        b->cdith[i] = (int16_t)cdith; b->adith[i] = (int8_t)adith;

        R[REG_SHADE + 0] = shade.r; R[REG_SHADE + 1] = shade.g;
        R[REG_SHADE + 2] = shade.b; R[REG_SHADE + 3] = shade.a;
        R[REG_LODF] = b->lodfrac[i];
        for (int c = 0; c < 4; c++) { R[REG_TEX0 + c] = b->tex0[c][i]; R[REG_TEX1 + c] = b->tex1[c][i]; }

        int32_t rgb[3], a9, comb9[3];
        int32_t acalpha = 0;
        if (two) {
            /* COMBINED is undefined in the first cycle (the hardware feeds back the previous pixel's); 0 keeps the result independent of pixel order */
            R[REG_COMB + 0] = R[REG_COMB + 1] = R[REG_COMB + 2] = R[REG_COMB + 3] = 0;
            comb_eval(r, R, 0, rgb, &a9);
            if (om->alpha_compare_en) {
                int32_t pre = (int32_t)k_clamp9[a9 & 0x1ff];
                if (pre == 0xff) pre = 0x100;
                if (!om->alpha_cvg_select) {
                    pre += adith;
                    if (pre & 0x100) pre = 0xff;
                } else {
                    if (om->cvg_times_alpha) pre = (pre * cvg + 4) >> 3;
                    else pre = cvg << 5;
                    if (pre > 0xff) pre = 0xff;
                }
                acalpha = pre;
            }
            R[REG_COMB + 0] = rgb[0] >> 8; R[REG_COMB + 1] = rgb[1] >> 8;
            R[REG_COMB + 2] = rgb[2] >> 8; R[REG_COMB + 3] = a9;
            /* cycle 1 sees TEXEL0 = cycle-0 TEXEL1 and TEXEL1 = next pixel's TEXEL0 */
            for (int c = 0; c < 4; c++) { R[REG_TEX0 + c] = b->tex1[c][i]; R[REG_TEX1 + c] = b->tex0n[c][i]; }
        }
        comb_eval(r, R, 1, rgb, &a9);
        struct color pix;
        comb_finish(r, R, 1, adith, rgb, a9, &cvg, &pix, comb9);
        b->cvg[i] = cvg;
        b->pixel[CH_R][i] = (int16_t)pix.r; b->pixel[CH_G][i] = (int16_t)pix.g;
        b->pixel[CH_B][i] = (int16_t)pix.b; b->pixel[CH_A][i] = (int16_t)pix.a;
        b->acalpha[i] = (int16_t)(two ? acalpha : pix.a);

        int32_t sha = shade.a + adith;
        if (sha & 0x100) sha = 0xff;
        b->shade_alpha[i] = (int16_t)sha;
        (void)comb9;
    }
}

static void fb_read(const struct rdp *r, int32_t idx, struct color *mem, uint8_t *memcvg)
{
    const struct other_modes *om = &r->om;
    mem->r = mem->g = mem->b = 0;
    *memcvg = 7;
    mem->a = 0xe0;
    switch (r->fb_size) {
    case PIXEL_SIZE_8BIT:
        if (om->image_read_en)
            mem->r = mem->g = mem->b = rdram_rd8(r, r->fb_address + (uint32_t)idx);
        break;
    case PIXEL_SIZE_16BIT:
        if (om->image_read_en) {
            uint16_t fword;
            uint8_t hbyte, lowbits;
            rdram_rd_pair16(r, (r->fb_address >> 1) + (uint32_t)idx, &fword, &hbyte);
            if (r->fb_format == FORMAT_RGBA) {
                mem->r = (fword >> 8) & 0xf8;
                mem->g = (fword & 0x7c0) >> 3;
                mem->b = (fword & 0x3e) << 2;
                lowbits = (uint8_t)(((fword & 1) << 2) | hbyte);
            } else {
                mem->r = mem->g = mem->b = fword >> 8;
                lowbits = (fword >> 5) & 7;
            }
            *memcvg = lowbits;
            mem->a = lowbits << 5;
        }
        break;
    case PIXEL_SIZE_32BIT:
        if (om->image_read_en) {
            uint32_t m = rdram_rd32(r, (r->fb_address >> 2) + (uint32_t)idx);
            mem->r = (m >> 24) & 0xff; mem->g = (m >> 16) & 0xff; mem->b = (m >> 8) & 0xff;
            *memcvg = (m >> 5) & 7;
            mem->a = m & 0xe0;
        }
        break;
    default:
        break;
    }
}

static void scalar_zcmp(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    const struct prim_ctx *pc = &r->pc;
    uint32_t dzpix = (uint16_t)pc->dzpix;
    int dzpixenc = pc->dzpixenc;

    for (int i = 0; i < b->n; i++) {
        uint8_t cvg = b->cvg[i];
        int offx = b->offx[i], offy = b->offy[i];
        struct color mem;
        fb_read(r, (int32_t)b->fbidx[i], &mem, &b->memcvg[i]);
        b->mem[CH_R][i] = (int16_t)mem.r; b->mem[CH_G][i] = (int16_t)mem.g;
        b->mem[CH_B][i] = (int16_t)mem.b; b->mem[CH_A][i] = (int16_t)mem.a;
        uint32_t memcvg = b->memcvg[i];

        int32_t sz = (b->cz[i] >> 10) & 0x3fffff;
        if (cvg == 8) sz >>= 3;
        else sz = ((sz << 2) + offx * pc->cdz + offy * pc->dzdy) >> 5;
        switch ((sz & 0x60000) >> 17) {
        case 2: sz = 0x3ffff; break;
        case 3: sz = 0; break;
        default: sz &= 0x3ffff; break;
        }
        b->sz[i] = sz;

        int wen;
        uint32_t blend_en, prewrap;
        b->blshifta[i] = 0; b->blshiftb[i] = 0;

        if (om->z_compare_en) {
            uint16_t zval; uint8_t hval;
            uint32_t zidx = (r->zb_address >> 1) + b->fbidx[i];
            rdram_rd_pair16(r, zidx, &zval, &hval);
            uint32_t oz = z_complete_dec_table[(zval >> 2) & 0x3fff];
            int32_t rawdzmem = ((zval & 3) << 2) | hval;
            uint32_t dzmem = 1u << rawdzmem;
            int force_coplanar = 0;

            b->blshifta[i] = (int8_t)foir_clamp(dzpixenc - rawdzmem, 0, 4);
            b->blshiftb[i] = (int8_t)foir_clamp(rawdzmem - dzpixenc, 0, 4);

            int precision_factor = (zval >> 13) & 0xf;
            if (precision_factor < 3) {
                if (dzmem != 0x8000) {
                    uint32_t dzmemmodifier = 16 >> precision_factor;
                    dzmem <<= 1;
                    if (dzmem < dzmemmodifier) dzmem = dzmemmodifier;
                } else {
                    force_coplanar = 1;
                    dzmem = 0xffff;
                }
            }
            uint32_t dznew = deltaz_comparator_lut[(dzpix | dzmem) & 0xffff];
            uint32_t dznotshift = dznew;
            dznew <<= 3;

            uint32_t farther = force_coplanar || (((uint32_t)sz + dznew) >= oz);
            int overflow = (memcvg + cvg) & 8;
            blend_en = om->force_blend || (!overflow && om->antialias_en && farther);
            prewrap = (uint32_t)overflow;

            uint32_t infront = (uint32_t)sz < oz;
            int32_t diff = sz - (int32_t)dznew;
            uint32_t nearer = force_coplanar || (diff <= (int32_t)oz);
            uint32_t max = (oz == 0x3ffff);
            switch (om->z_mode) {
            case ZMODE_OPAQUE:
                wen = max || (overflow ? infront : nearer);
                break;
            case ZMODE_INTERPENETRATING:
                if (!infront || !farther || !overflow) {
                    wen = max || (overflow ? infront : nearer);
                } else {
                    uint32_t dzenc = dz_compress(dznotshift & 0xffff);
                    int cvgcoeff = (int)(((oz >> dzenc) - ((uint32_t)sz >> dzenc)) & 0xf);
                    cvg = (uint8_t)(((cvgcoeff * cvg) >> 3) & 0xf);
                    wen = 1;
                }
                break;
            case ZMODE_TRANSPARENT:
                wen = infront || max;
                break;
            default:
                wen = farther && nearer && !max;
                break;
            }
        } else {
            b->blshifta[i] = 0;
            b->blshiftb[i] = (int8_t)(dzpixenc < 0xb ? 4 : 0xf - dzpixenc);
            int overflow = (memcvg + cvg) & 8;
            blend_en = om->force_blend || (!overflow && om->antialias_en);
            prewrap = (uint32_t)overflow;
            wen = 1;
        }
        b->cvg[i] = cvg;
        b->blend_en[i] = (uint8_t)blend_en;
        b->prewrap[i] = (uint8_t)prewrap;
        b->wen[i] = (uint8_t)wen;
    }
}

static inline void bl_color_sel(const struct rdp *r, const struct px_batch *b, int i, int sel,
                                const struct color *pix, int32_t out[3])
{
    switch (sel & 3) {
    case 0: out[0] = pix->r; out[1] = pix->g; out[2] = pix->b; break;
    case 1: out[0] = b->mem[CH_R][i]; out[1] = b->mem[CH_G][i]; out[2] = b->mem[CH_B][i]; break;
    case 2: out[0] = r->blend_color.r; out[1] = r->blend_color.g; out[2] = r->blend_color.b; break;
    default: out[0] = r->fog_color.r; out[1] = r->fog_color.g; out[2] = r->fog_color.b; break;
    }
}

static inline int32_t bl_m1b(const struct rdp *r, const struct px_batch *b, int i, int sel, const struct color *pix)
{
    switch (sel & 3) {
    case 0: return pix->a;
    case 1: return r->fog_color.a;
    case 2: return b->shade_alpha[i];
    default: return 0;
    }
}

static inline int32_t bl_m2b(const struct px_batch *b, int i, int sel, int32_t inv_pixel_a)
{
    switch (sel & 3) {
    case 0: return inv_pixel_a;
    case 1: return b->mem[CH_A][i];
    case 2: return 0xff;
    default: return 0;
    }
}

/* `divide` selects the exact division table (final cycle) */
static void blend_equation(const struct rdp *r, const struct px_batch *b, int i, int cyc,
                           const struct color *pix, int divide, int32_t out[3])
{
    const struct other_modes *om = &r->om;
    int32_t c1[3], c2[3];
    bl_color_sel(r, b, i, om->blend_m1a[cyc], pix, c1);
    bl_color_sel(r, b, i, om->blend_m2a[cyc], pix, c2);
    int32_t m1b = bl_m1b(r, b, i, om->blend_m1b[cyc], pix);
    int32_t inv = (~m1b) & 0xff;
    int32_t m2b = bl_m2b(b, i, om->blend_m2b[cyc], inv);

    int blend1a = m1b >> 3, blend2a = m2b >> 3;
    if (om->blend_m2b[cyc] == 1) {
        blend1a = (blend1a >> b->blshifta[i]) & 0x3c;
        blend2a = (blend2a >> b->blshiftb[i]) | 3;
    }
    int mulb = blend2a + 1;
    for (int ch = 0; ch < 3; ch++) {
        int bl = c1[ch] * blend1a + c2[ch] * mulb;
        if (divide && !om->force_blend) {
            int sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
            out[ch] = k_bldiv[sum | ((bl >> 2) & 0x7ff)];
        } else {
            out[ch] = (bl >> 5) & 0xff;
        }
    }
}

static bool alpha_compare(struct rdp *r, int32_t a, uint32_t fbidx)
{
    if (!r->om.alpha_compare_en) return true;
    int32_t threshold = r->om.dither_alpha_en ? (int32_t)(prand(fbidx, r->pc.seed, 3) & 0xff) : r->blend_color.a;
    return a >= threshold;
}

static void scalar_blend(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    int two = om->cycle_type == CYCLE_TYPE_2;

    for (int i = 0; i < b->n; i++) {
        if (!b->wen[i]) continue;
        struct color pix = { b->pixel[CH_R][i], b->pixel[CH_G][i], b->pixel[CH_B][i], b->pixel[CH_A][i] };
        int cyc = two ? 1 : 0;
        int32_t rgb[3];

        if (!two) {
            if (!alpha_compare(r, pix.a, b->fbidx[i])) { b->wen[i] = 0; continue; }
            if (!(om->antialias_en ? b->cvg[i] : b->cvbit[i])) { b->wen[i] = 0; continue; }
        } else {
            if (!(om->antialias_en ? b->cvg[i] : b->cvbit[i])) { b->wen[i] = 0; continue; }
            int32_t c0[3];
            blend_equation(r, b, i, 0, &pix, 0, c0);
            if (!alpha_compare(r, b->acalpha[i], b->fbidx[i])) { b->wen[i] = 0; continue; }
            pix.r = c0[0]; pix.g = c0[1]; pix.b = c0[2];
            /* pix.a stays the combiner alpha: m1b / inverse alpha come from it */
        }

        if (!om->color_on_cvg || b->prewrap[i]) {
            int dontblend = om->partialreject[cyc] && b->pixel[CH_A][i] >= 0xff;
            if (!b->blend_en[i] || dontblend) {
                int32_t c1[3];
                bl_color_sel(r, b, i, om->blend_m1a[cyc], &pix, c1);
                rgb[0] = c1[0]; rgb[1] = c1[1]; rgb[2] = c1[2];
            } else {
                struct color pc = pix;
                pc.a = b->pixel[CH_A][i];
                blend_equation(r, b, i, cyc, &pc, 1, rgb);
            }
        } else {
            int32_t c2[3];
            bl_color_sel(r, b, i, om->blend_m2a[cyc], &pix, c2);
            rgb[0] = c2[0]; rgb[1] = c2[1]; rgb[2] = c2[2];
        }
        if (om->rgb_dither_sel != 3)
            rgb_dither(om->rgb_dither_sel, &rgb[0], &rgb[1], &rgb[2], b->cdith[i]);
        b->fcol[0][i] = (int16_t)rgb[0]; b->fcol[1][i] = (int16_t)rgb[1]; b->fcol[2][i] = (int16_t)rgb[2];
    }
}

static int finalize_spanalpha(int cvg_dest, uint32_t blend_en, uint32_t cvg, uint32_t memcvg)
{
    int f = 0;
    switch (cvg_dest) {
    case CVG_CLAMP:
        f = blend_en ? (int)(cvg + memcvg) : (int)cvg - 1;
        f = (f & 8) ? 7 : (f & 7);
        break;
    case CVG_WRAP: f = (cvg + memcvg) & 7; break;
    case CVG_ZAP:  f = 7; break;
    default:       f = (int)memcvg; break;
    }
    return f;
}

static void scalar_store(struct rdp *r, struct px_batch *b)
{
    const struct other_modes *om = &r->om;
    for (int i = 0; i < b->n; i++) {
        if (!b->wen[i]) continue;
        uint32_t idx = (uint32_t)b->fbidx[i];
        int32_t fr = b->fcol[0][i], fg = b->fcol[1][i], fb = b->fcol[2][i];
        int finalcvg;

        switch (r->fb_size) {
        case PIXEL_SIZE_4BIT:
            rdram_wr8(r, r->fb_address + idx, 0);
            break;
        case PIXEL_SIZE_8BIT: {
            uint32_t a = r->fb_address + idx;
            rdram_wr8(r, a, (uint8_t)((a & 1) ? fg : fr));
            break;
        }
        case PIXEL_SIZE_16BIT: {
            uint16_t color;
            finalcvg = finalize_spanalpha(om->cvg_dest, b->blend_en[i], b->cvg[i], b->memcvg[i]);
            if (r->fb_format == FORMAT_RGBA) {
                color = (uint16_t)(((fr & ~7) << 8) | ((fg & ~7) << 3) | ((fb & ~7) >> 2));
            } else {
                color = (uint16_t)((fr << 8) | (finalcvg << 5));
                finalcvg = 0;
            }
            rdram_wr_pair16(r, (r->fb_address >> 1) + idx, (uint16_t)(color | (finalcvg >> 2)), (uint8_t)(finalcvg & 3));
            break;
        }
        default: {
            finalcvg = finalize_spanalpha(om->cvg_dest, b->blend_en[i], b->cvg[i], b->memcvg[i]);
            uint32_t c = ((uint32_t)fr << 24) | ((uint32_t)fg << 16) | ((uint32_t)fb << 8) | ((uint32_t)finalcvg << 5);
            rdram_wr32(r, (r->fb_address >> 2) + idx, c);
            break;
        }
        }

        if (om->z_update_en) {
            uint32_t enc = (uint32_t)r->pc.dzpixenc;
            uint16_t zval = (uint16_t)(z_com_table[b->sz[i] & 0x3ffff] | (uint16_t)(enc >> 2));
            rdram_wr_pair16(r, (r->zb_address >> 1) + idx, zval, (uint8_t)(enc & 3));
        }
    }
}

static void scalar_fill(struct rdp *r, int y, int x0, int x1)
{
    uint32_t base = (uint32_t)(r->fb_width * y);
    for (int x = x0; x <= x1; x++) {
        uint32_t idx = base + (uint32_t)x;
        switch (r->fb_size) {
        case PIXEL_SIZE_8BIT: {
            uint32_t a = r->fb_address + idx;
            rdram_wr8(r, a, (uint8_t)(r->fill_color >> (((a & 3) ^ 3) * 8)));
            break;
        }
        case PIXEL_SIZE_16BIT: {
            uint32_t hi = (r->fb_address >> 1) + idx;
            uint16_t val = (hi & 1) ? (uint16_t)(r->fill_color & 0xffff) : (uint16_t)(r->fill_color >> 16);
            rdram_wr_pair16(r, hi, val, (uint8_t)(((val & 1) << 1) | (val & 1)));
            break;
        }
        default:
            rdram_wr32(r, (r->fb_address >> 2) + idx, r->fill_color);
            break;
        }
    }
}

static void copy_texel16(const struct rdp *r, const struct tile *tl, int s, int tt, uint16_t *out, bool *opaque)
{
    struct color c;
    if (r->om.en_tlut) {
        uint32_t idx;
        if (tl->size == PIXEL_SIZE_4BIT || tl->size == PIXEL_SIZE_8BIT) {
            struct tile tmp = *tl; tmp.format = FORMAT_CI;
            tmem_fetch_texel(r, &tmp, s, tt, &c);
            idx = (uint32_t)c.r & 0xff;
        } else {
            idx = tmem_raw16(r, tl, s, tt) >> 8;
        }
        *out = tmem_tlut_raw(r, idx);
        *opaque = (*out & 1) != 0;
    } else if (tl->size == PIXEL_SIZE_16BIT) {
        *out = tmem_raw16(r, tl, s, tt);
        *opaque = (*out & 1) != 0;
    } else {
        tmem_fetch_texel(r, tl, s, tt, &c);
        /* best effort: pack the filtered colour as RGBA5551 */
        *out = (uint16_t)(((c.r & 0xf8) << 8) | ((c.g & 0xf8) << 3) | ((c.b & 0xf8) >> 2) | (c.a >= 0x80 ? 1 : 0));
        *opaque = c.a >= 0x80;
    }
}

static void scalar_copy(struct rdp *r, int y, const struct span *sp, int flip)
{
    const struct prim_ctx *pc = &r->pc;
    int xstart = sp->lx, xendsc = sp->rx;
    int length = flip ? (xstart - xendsc) : (xendsc - xstart);
    if (length < 0) return;
    int xinc = flip ? 1 : -1;
    int32_t dsinc = flip ? pc->ds : -pc->ds, dtinc = flip ? pc->dt : -pc->dt, dwinc = flip ? pc->dw : -pc->dw;
    int32_t s = sp->s, t = sp->t, w = sp->w;
    int adv = r->fb_size == PIXEL_SIZE_8BIT ? 8 : 4;
    int tilenum = pc->tilenum;
    uint32_t base = (uint32_t)(r->fb_width * y);
    int x = xendsc;

    for (int j = 0; j <= length; j += adv, x += xinc * adv) {
        int32_t sss, sst;
        tc_divide(r, s >> 16, t >> 16, w >> 16, &sss, &sst);
        tc_clamp17(&sss, &sst);
        const struct tile *tl = &r->tile[tilenum];

        int32_t cs = SIGN16(sss), ct = SIGN16(sst);
        int sh = tl->shift_s, th = tl->shift_t;
        cs = sh < 11 ? (cs >> sh) : SIGN16(cs << (16 - sh));
        ct = th < 11 ? (ct >> th) : SIGN16(ct << (16 - th));
        cs = TRELATIVE(cs, tl->sl) >> 5;
        ct = TRELATIVE(ct, tl->tl) >> 5;

        for (int q = 0; q < adv && j + q <= length; q++) {
            int32_t ss = cs + q, tt = ct;
            if (tl->mask_s) ss &= (1 << (tl->mask_s > 10 ? 10 : tl->mask_s)) - 1;
            if (tl->mask_t) tt &= (1 << (tl->mask_t > 10 ? 10 : tl->mask_t)) - 1;
            uint16_t px; bool opaque;
            copy_texel16(r, tl, ss, tt, &px, &opaque);
            if (r->om.alpha_compare_en && !opaque) continue;
            int xp = x + xinc * q;
            if (xp >= r->fb_width) continue;
            uint32_t idx = base + (uint32_t)xp;
            if (r->fb_size == PIXEL_SIZE_16BIT)
                rdram_wr_pair16(r, (r->fb_address >> 1) + idx, px, (uint8_t)((px & 1) ? 3 : 0));
            else
                rdram_wr8(r, r->fb_address + idx, (uint8_t)(px >> 8));
        }
        s += dsinc; t += dtinc; w += dwinc;
    }
}

const struct foir_kernels foir_kernels_scalar = {
    .name = "scalar",
    .texture = scalar_texture,
    .combine = scalar_combine,
    .zcmp = scalar_zcmp,
    .blend = scalar_blend,
    .store = scalar_store,
    .fill = scalar_fill,
    .copy = scalar_copy,
};
