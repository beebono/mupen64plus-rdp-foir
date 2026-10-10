/*
 * mupen64plus-rdp-foir - vi.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vi.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

/* no VI filters (anti-aliasing, divot, gamma, dither): crop to the active region and expand to RGBA8888 */
static void row16(const struct rdp *r, uint32_t base, int w, uint32_t *dst)
{
    int x = 0;
#if defined(FOIR_NEON) && FOIR_HALF_XOR == 1
    base &= RDRAM_MASK >> 1;
    /* pixel i lives at host halfword i ^ 1; consume an even-aligned run with vector loads */
    if (((base + (uint32_t)w + 16) << 1) <= r->rdram_size) {
        const uint16_t *h = (const uint16_t *)r->rdram;
        for (; x < w && ((base + (uint32_t)x) & 1); x++) {
            uint16_t p = h[(base + (uint32_t)x) ^ 1];
            uint32_t cr = (p >> 11) & 31, cg = (p >> 6) & 31, cb = (p >> 1) & 31;
            cr = (cr << 3) | (cr >> 2); cg = (cg << 3) | (cg >> 2); cb = (cb << 3) | (cb >> 2);
            dst[x] = 0xff000000u | (cb << 16) | (cg << 8) | cr;
        }
        for (; x + 8 <= w; x += 8) {
            uint16x8_t p = vrev32q_u16(vld1q_u16(h + base + (uint32_t)x));
            uint16x8_t r5 = vshrq_n_u16(p, 11);
            uint16x8_t g5 = vandq_u16(vshrq_n_u16(p, 6), vdupq_n_u16(31));
            uint16x8_t b5 = vandq_u16(vshrq_n_u16(p, 1), vdupq_n_u16(31));
            uint8x8x4_t o;
            o.val[0] = vmovn_u16(vorrq_u16(vshlq_n_u16(r5, 3), vshrq_n_u16(r5, 2)));
            o.val[1] = vmovn_u16(vorrq_u16(vshlq_n_u16(g5, 3), vshrq_n_u16(g5, 2)));
            o.val[2] = vmovn_u16(vorrq_u16(vshlq_n_u16(b5, 3), vshrq_n_u16(b5, 2)));
            o.val[3] = vdup_n_u8(0xff);
            vst4_u8((uint8_t *)(dst + x), o);
        }
    }
#endif
    for (; x < w; x++) {
        uint16_t p = rdram_rd16(r, base + (uint32_t)x);
        uint32_t cr = (p >> 11) & 31, cg = (p >> 6) & 31, cb = (p >> 1) & 31;
        cr = (cr << 3) | (cr >> 2); cg = (cg << 3) | (cg >> 2); cb = (cb << 3) | (cb >> 2);
        dst[x] = 0xff000000u | (cb << 16) | (cg << 8) | cr;
    }
}

static void row16_raw(const struct rdp *r, uint32_t base, int w, uint16_t *dst)
{
    int x = 0;
    base &= RDRAM_MASK >> 1;
#if defined(FOIR_NEON) && FOIR_HALF_XOR == 1
    if (((base + (uint32_t)w + 16) << 1) <= r->rdram_size) {
        const uint16_t *h = (const uint16_t *)r->rdram;
        for (; x < w && ((base + (uint32_t)x) & 1); x++) dst[x] = h[(base + (uint32_t)x) ^ 1];
        for (; x + 8 <= w; x += 8) vst1q_u16(dst + x, vrev32q_u16(vld1q_u16(h + base + (uint32_t)x)));
    }
#endif
    for (; x < w; x++) dst[x] = rdram_rd16(r, base + (uint32_t)x);
}

static void row32(const struct rdp *r, uint32_t base, int w, uint32_t *dst)
{
    int x = 0;
#if defined(FOIR_NEON)
    base &= RDRAM_MASK >> 2;
    if (((base + (uint32_t)w + 4) << 2) <= r->rdram_size) {
        const uint32_t *p = (const uint32_t *)r->rdram + base;
        const uint8x16_t alpha = vreinterpretq_u8_u32(vdupq_n_u32(0xff000000u));
        for (; x + 4 <= w; x += 4) {
            uint8x16_t v = vrev32q_u8(vreinterpretq_u8_u32(vld1q_u32(p + x)));
            vst1q_u32(dst + x, vreinterpretq_u32_u8(vorrq_u8(v, alpha)));
        }
    }
#endif
    for (; x < w; x++) {
        uint32_t p = rdram_rd32(r, base + (uint32_t)x);
        dst[x] = 0xff000000u | (((p >> 8) & 0xff) << 16) | (((p >> 16) & 0xff) << 8) | (p >> 24);
    }
}

void vi_render(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out)
{
    vi_render_ex(r, regs, out, false);
}

void vi_render_ex(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out, bool allow_raw16)
{
    int type = regs->status & 3;
    out->valid = false;
    out->raw16 = false;
    out->width = out->height = 0;
    out->screen_w = out->screen_h = 1.0f;

    int hstart = (regs->h_start >> 16) & 0x3ff, hend = regs->h_start & 0x3ff;
    int vstart = (regs->v_start >> 16) & 0x3ff, vend = regs->v_start & 0x3ff;
    int xscale = regs->x_scale & 0xfff, yscale = regs->y_scale & 0xfff;
    int stride = (int)(regs->width & 0xfff);

    if (type < 2 || hend <= hstart || vend <= vstart || xscale == 0 || yscale == 0 || stride == 0)
        return;

    int w = ((hend - hstart) * xscale) >> 10;
    int h = (((vend - vstart) >> 1) * yscale) >> 10;
    if (w <= 0 || h <= 0) return;
    {
        int hscr = hend - hstart, vlines = (vend - vstart) >> 1;
        float fw = hscr / 640.0f, fh = vlines / (vlines > 250 ? 288.0f : 240.0f);
        out->screen_w = fw >= 0.94f ? 1.0f : (fw < 0.25f ? 0.25f : fw);
        out->screen_h = fh >= 0.94f ? 1.0f : (fh < 0.25f ? 0.25f : fh);
    }
    if (w > VI_MAX_W) w = VI_MAX_W;
    if (h > VI_MAX_H) h = VI_MAX_H;

    uint32_t origin = regs->origin & 0xffffff;
    /* InterlaceHack: the buffer holds one fresh field; weaving it would mix in the previous one, so show its rows twice */
    bool fieldhack = r->cfg.interlace_hack && (regs->status & 0x40) && r->scfield && h >= 2;
    int field = r->sckeepodd & 1;
    const bool raw = allow_raw16 && type == 2;
    for (int y = 0; y < h; y++) {
        int sy = fieldhack ? ((y & ~1) | field) : y;
        if (raw) {
            uint16_t *d16 = (uint16_t *)out->pixels + (size_t)y * (size_t)w;
            if (fieldhack && (y & 1)) { memcpy(d16, d16 - w, (size_t)w * 2); continue; }
            row16_raw(r, (origin >> 1) + (uint32_t)(sy * stride), w, d16);
            continue;
        }
        uint32_t *dst = &out->pixels[y * w];
        if (fieldhack && (y & 1)) { memcpy(dst, dst - w, (size_t)w * 4); continue; }
        if (type == 2) row16(r, (origin >> 1) + (uint32_t)(sy * stride), w, dst);
        else           row32(r, (origin >> 2) + (uint32_t)(sy * stride), w, dst);
    }
    out->raw16 = raw;
    out->width = w;
    out->height = h;
    out->valid = true;
}
