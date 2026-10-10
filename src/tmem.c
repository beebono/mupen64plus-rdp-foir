/*
 * mupen64plus-rdp-foir - tmem.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rdp.h"
#include "mt.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

/* TMEM is 4096 big-endian bytes. Odd texture rows have the two 32-bit halves of every 64-bit word swapped (byte address ^ 4, halfword
 * index ^ 2). RGBA32 and YUV use both 2 KiB banks (low RG / UV, high at +0x800 BA / Y). */

static uint8_t replicated_rgba[32];

static void init_expand(void)
{
    static bool done;
    if (done) return;
    for (int i = 0; i < 32; i++) replicated_rgba[i] = (uint8_t)((i << 3) | ((i >> 2) & 7));
    done = true;
}

void tile_update_derived(struct tile *t)
{
    init_expand();
    t->clampdiffs = ((t->sh >> 2) - (t->sl >> 2)) & 0x3ff;
    t->clampdifft = ((t->th >> 2) - (t->tl >> 2)) & 0x3ff;
    t->clampens = t->cs || !t->mask_s;
    t->clampent = t->ct || !t->mask_t;
    t->masksclamped = t->mask_s <= 10 ? t->mask_s : 10;
    t->masktclamped = t->mask_t <= 10 ? t->mask_t : 10;
}

FOIR_INLINE uint8_t tm8(const struct rdp *r, uint32_t a) { return r->tmem[a & 0xfff]; }
FOIR_INLINE uint16_t tm16(const struct rdp *r, uint32_t i)
{
    i &= 0x7ff;
    return (uint16_t)((r->tmem[i * 2] << 8) | r->tmem[i * 2 + 1]);
}

#define BSWAP(t) (((t) & 1) ? 4 : 0)
#define HSWAP(t) (((t) & 1) ? 2 : 0)

static inline void rgba16_to_color(uint16_t c, struct color *o)
{
    o->r = replicated_rgba[c >> 11];
    o->g = replicated_rgba[(c >> 6) & 0x1f];
    o->b = replicated_rgba[(c >> 1) & 0x1f];
    o->a = (c & 1) ? 0xff : 0;
}

void tmem_fetch_texel(const struct rdp *r, const struct tile *tl, int s, int tt, struct color *c)
{
    uint32_t tbase = (uint32_t)(tl->line * (tt & 0xff) + tl->tmem);
    int fmt = tl->format, size = tl->size;
    uint32_t a, i;

    switch (fmt) {
    case FORMAT_RGBA:
        switch (size) {
        case PIXEL_SIZE_4BIT: {
            a = (((tbase << 4) + s) >> 1) ^ BSWAP(tt);
            uint8_t v = tm8(r, a);
            v = (s & 1) ? (v & 0xf) : (v >> 4);
            v |= v << 4;
            c->r = c->g = c->b = c->a = v;
            return;
        }
        case PIXEL_SIZE_8BIT:
            a = ((tbase << 3) + s) ^ BSWAP(tt);
            c->r = c->g = c->b = c->a = tm8(r, a);
            return;
        case PIXEL_SIZE_16BIT:
            i = ((tbase << 2) + s) ^ HSWAP(tt);
            rgba16_to_color(tm16(r, i), c);
            return;
        default:
            i = (((tbase << 2) + s) ^ HSWAP(tt)) & 0x3ff;
            { uint16_t lo = tm16(r, i), hi = tm16(r, i | 0x400);
              c->r = lo >> 8; c->g = lo & 0xff; c->b = hi >> 8; c->a = hi & 0xff; }
            return;
        }
    case FORMAT_YUV:
        switch (size) {
        case PIXEL_SIZE_4BIT: {
            a = ((tbase << 3) + s) ^ BSWAP(tt);
            int32_t save = tm8(r, a) & 0xf0;
            save |= save >> 4;
            c->r = c->g = save - 0x80;
            c->b = c->a = save;
            return;
        }
        case PIXEL_SIZE_8BIT: {
            a = ((tbase << 3) + s) ^ BSWAP(tt);
            int32_t save = tm8(r, a);
            c->r = c->g = save - 0x80;
            c->b = c->a = save;
            return;
        }
        case PIXEL_SIZE_16BIT: {
            uint32_t ta = (tbase << 3) + s;
            uint32_t low = ((ta >> 1) ^ HSWAP(tt)) & 0x3ff;
            uint16_t uv = tm16(r, low);
            int32_t y = tm8(r, ((ta ^ BSWAP(tt)) & 0x7ff) | 0x800);
            c->r = (uv >> 8) - 0x80;
            c->g = (uv & 0xff) - 0x80;
            c->b = c->a = y;
            return;
        }
        default: {
            uint32_t ta = (tbase << 3) + s;
            uint32_t low = ((ta >> 1) ^ HSWAP(tt)) & 0x3ff;
            uint16_t uv = tm16(r, low);
            c->r = (uv >> 8) - 0x80;
            c->g = (uv & 0xff) - 0x80;
            if (s & 1) {
                int32_t y = tm8(r, ((ta ^ BSWAP(tt)) & 0x7ff) | 0x800);
                c->b = c->a = y;
            } else {
                uint16_t y = tm16(r, low | 0x400);
                c->b = y >> 8;
                c->a = ((y >> 8) & 0xf) | (y & 0xf0);
            }
            return;
        }
        }
    case FORMAT_CI:
        switch (size) {
        case PIXEL_SIZE_4BIT: {
            a = (((tbase << 4) + s) >> 1) ^ BSWAP(tt);
            uint8_t p = tm8(r, a);
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            p = (uint8_t)((tl->palette << 4) | p);
            c->r = c->g = c->b = c->a = p;
            return;
        }
        case PIXEL_SIZE_8BIT:
            a = ((tbase << 3) + s) ^ BSWAP(tt);
            c->r = c->g = c->b = c->a = tm8(r, a);
            return;
        default:
            i = ((tbase << 2) + s) ^ HSWAP(tt);
            { uint16_t v = tm16(r, i); c->r = c->b = v >> 8; c->g = c->a = v & 0xff; }
            return;
        }
    case FORMAT_IA:
        switch (size) {
        case PIXEL_SIZE_4BIT: {
            a = (((tbase << 4) + s) >> 1) ^ BSWAP(tt);
            uint8_t p = tm8(r, a);
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            uint8_t iv = p & 0xe;
            iv = (uint8_t)((iv << 4) | (iv << 1) | (iv >> 2));
            c->r = c->g = c->b = iv;
            c->a = (p & 1) ? 0xff : 0;
            return;
        }
        case PIXEL_SIZE_8BIT: {
            a = ((tbase << 3) + s) ^ BSWAP(tt);
            uint8_t p = tm8(r, a);
            uint8_t iv = p & 0xf0; iv |= iv >> 4;
            c->r = c->g = c->b = iv;
            c->a = ((p & 0xf) << 4) | (p & 0xf);
            return;
        }
        case PIXEL_SIZE_16BIT:
            i = ((tbase << 2) + s) ^ HSWAP(tt);
            { uint16_t v = tm16(r, i); c->r = c->g = c->b = v >> 8; c->a = v & 0xff; }
            return;
        default:
            i = ((tbase << 2) + s) ^ HSWAP(tt);
            { uint16_t v = tm16(r, i); c->r = c->b = v >> 8; c->g = c->a = v & 0xff; }
            return;
        }
    default: /* FORMAT_I and invalid formats */
        switch (size) {
        case PIXEL_SIZE_4BIT: {
            a = (((tbase << 4) + s) >> 1) ^ BSWAP(tt);
            uint8_t v = tm8(r, a);
            v = (s & 1) ? (v & 0xf) : (v >> 4);
            v |= v << 4;
            c->r = c->g = c->b = c->a = v;
            return;
        }
        case PIXEL_SIZE_8BIT:
            a = ((tbase << 3) + s) ^ BSWAP(tt);
            c->r = c->g = c->b = c->a = tm8(r, a);
            return;
        default:
            i = ((tbase << 2) + s) ^ HSWAP(tt);
            { uint16_t v = tm16(r, i); c->r = c->b = v >> 8; c->g = c->a = v & 0xff; }
            return;
        }
    }
}

uint16_t tmem_raw16(const struct rdp *r, const struct tile *tl, int s, int tt)
{
    uint32_t tbase = (uint32_t)(tl->line * (tt & 0xff) + tl->tmem);
    return tm16(r, ((tbase << 2) + s) ^ HSWAP(tt));
}

uint16_t tmem_tlut_raw(const struct rdp *r, uint32_t idx)
{
    return tm16(r, (0x800 >> 1) + ((idx & 0xff) << 2));
}

void tmem_fetch_tlut(const struct rdp *r, uint32_t idx, struct color *c)
{
    uint16_t v = tmem_tlut_raw(r, idx);
    if (!r->om.tlut_type) {
        rgba16_to_color(v, c);
    } else {
        c->r = c->g = c->b = v >> 8;
        c->a = v & 0xff;
    }
}

enum { LOAD_RAW = 0, LOAD_RGBA32, LOAD_YUV };

static int load_kind(const struct tile *t)
{
    if (t->format == FORMAT_YUV) return LOAD_YUV;
    if (t->format == FORMAT_RGBA && t->size == PIXEL_SIZE_32BIT) return LOAD_RGBA32;
    return LOAD_RAW;
}

/* whole 32-bit words from RDRAM to TMEM with the odd-row word swap; needs a 4-byte aligned source, an 8-byte aligned TMEM destination and no wrap-around */
static bool load_raw_fast(struct rdp *r, uint32_t src, uint32_t dst, int odd, uint32_t n)
{
    src &= RDRAM_MASK;
    if ((src & 3) || (dst & 7) || dst + n > 0x1000 || src + n > r->rdram_size || (src + n) > (RDRAM_MASK + 1)) return false;
    const uint32_t *w = (const uint32_t *)r->rdram + (src >> 2);
    uint8_t *d = r->tmem + dst;
    uint32_t i = 0, nw = n >> 2;
#if defined(FOIR_NEON)
    for (; i + 4 <= nw; i += 4) {
        uint32x4_t v = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(vld1q_u32(w + i))));
        if (odd) v = vrev64q_u32(v);
        vst1q_u32((uint32_t *)(d + 4 * i), v);
    }
#endif
    for (; i < nw; i++) {
        uint32_t off = (4 * i) ^ (odd ? 4u : 0u);
        uint32_t be = w[i];                       /* host word: address bytes A0 A1 A2 A3 from MSB to LSB */
        d[off + 0] = (uint8_t)(be >> 24); d[off + 1] = (uint8_t)(be >> 16);
        d[off + 2] = (uint8_t)(be >> 8);  d[off + 3] = (uint8_t)be;
    }
    for (uint32_t j = nw * 4; j < n; j++) {
        uint32_t off = j ^ (odd ? 4u : 0u);
        d[off] = rdram_rd8(r, src + j);
    }
    return true;
}

static void put_raw_bytes(struct rdp *r, uint32_t dst_byte_base, int odd, const uint8_t *src, int n)
{
    for (int j = 0; j < n; j++) {
        uint32_t a = (dst_byte_base + (uint32_t)j) ^ (odd ? 4u : 0u);
        r->tmem[a & 0xfff] = src[j];
    }
}

static void put_rgba32_texel(struct rdp *r, uint32_t hwidx, int odd, const uint8_t *px)
{
    uint32_t i = (hwidx ^ (odd ? 2u : 0u)) & 0x3ff;
    r->tmem[i * 2] = px[0]; r->tmem[i * 2 + 1] = px[1];
    r->tmem[0x800 + i * 2] = px[2]; r->tmem[0x800 + i * 2 + 1] = px[3];
}

static void put_yuv_pair(struct rdp *r, uint32_t hwidx, int odd, const uint8_t *px)
{
    /* px = u y0 v y1 */
    uint32_t i = (hwidx ^ (odd ? 2u : 0u)) & 0x3ff;
    r->tmem[i * 2] = px[0]; r->tmem[i * 2 + 1] = px[2];
    r->tmem[0x800 + i * 2] = px[1]; r->tmem[0x800 + i * 2 + 1] = px[3];
}

FOIR_INLINE int ti_bytes(const struct rdp *r) { return (1 << r->ti_size) >> 1; }

static void copy_src(const struct rdp *r, uint32_t addr, uint8_t *dst, int n)
{
    for (int i = 0; i < n; i++) dst[i] = rdram_rd8(r, addr + (uint32_t)i);
}

void tmem_load_tile(struct rdp *r, const uint32_t *a)
{
    struct tile *t = &r->tile[(a[1] >> 24) & 7];
    t->sl = (a[0] >> 12) & 0xfff; t->tl = a[0] & 0xfff;
    t->sh = (a[1] >> 12) & 0xfff; t->th = a[1] & 0xfff;
    tile_update_derived(t);

    int bpp = ti_bytes(r);
    if (r->ti_size == PIXEL_SIZE_4BIT || r->ti_width <= 0) return;

    int s0 = t->sl >> 2, s1 = t->sh >> 2, t0 = t->tl >> 2, t1 = t->th >> 2;
    if (s1 < s0 || t1 < t0) return;
    int n = s1 - s0 + 1;
    if (n > 2048) n = 2048;
    int kind = load_kind(t);
    uint8_t buf[4096 + 16];
    if (r->mt)
        mt_sync_range(r, r->ti_address + (uint32_t)((t0 * r->ti_width + s0) * bpp),
                      r->ti_address + (uint32_t)(((t1 * r->ti_width + s1) + 1) * bpp));
    r->tmem_dirty = true;

    for (int y = t0; y <= t1; y++) {
        int row = y - t0;
        int odd = row & 1;
        uint32_t src = r->ti_address + (uint32_t)((y * r->ti_width + s0) * bpp);
        uint32_t dstbase = (uint32_t)(t->tmem + row * t->line);
        if (kind == LOAD_RAW) {
            int nb = n * bpp;
            if (nb > 4096) nb = 4096;
            if (load_raw_fast(r, src, (dstbase * 8) & 0xfff, odd, (uint32_t)nb) && (dstbase * 8) < 0x1000) continue;
            copy_src(r, src, buf, nb);
            put_raw_bytes(r, dstbase * 8, odd, buf, nb);
        } else if (kind == LOAD_RGBA32) {
            if (bpp != 4) continue;
            for (int i = 0; i < n; i++) {
                copy_src(r, src + (uint32_t)i * 4, buf, 4);
                put_rgba32_texel(r, dstbase * 4 + (uint32_t)i, odd, buf);
            }
        } else {
            if (bpp != 2) continue;
            for (int p = 0; p < (n + 1) / 2; p++) {
                copy_src(r, src + (uint32_t)p * 4, buf, 4);
                put_yuv_pair(r, dstbase * 4 + (uint32_t)p, odd, buf);
            }
        }
    }
}

void tmem_load_block(struct rdp *r, const uint32_t *a)
{
    struct tile *t = &r->tile[(a[1] >> 24) & 7];
    uint32_t sl = (a[0] >> 12) & 0xfff, tl = a[0] & 0xfff;
    uint32_t sh = (a[1] >> 12) & 0xfff, dxt = a[1] & 0xfff;
    t->sl = sl; t->tl = tl; t->sh = sh; t->th = dxt;
    tile_update_derived(t);

    int bpp = ti_bytes(r);
    if (r->ti_size == PIXEL_SIZE_4BIT || sh < sl) return;
    uint32_t texels = sh - sl + 1;
    uint32_t nq = (texels * (uint32_t)bpp + 7) >> 3;
    if (nq > 512) nq = 512; /* TMEM is only 512 qwords */
    uint32_t src = r->ti_address + (uint32_t)((((int)tl & 0x3ff) * r->ti_width + (int)sl) * bpp);
    int kind = load_kind(t);
    uint8_t buf[8];
    if (r->mt) mt_sync_range(r, src, src + nq * 8);
    r->tmem_dirty = true;

    uint32_t kk = 0;
    if (kind == LOAD_RAW) {
        while (kk < nq) {
            uint32_t x = kk * dxt;
            int odd = (int)((x >> 11) & 1);
            uint32_t k1 = nq;
            if (dxt) {
                uint32_t boundary = ((x >> 11) + 1) << 11;
                uint32_t kb = (boundary + dxt - 1) / dxt;
                if (kb < k1) k1 = kb;
            }
            if (k1 <= kk) k1 = kk + 1;
            uint32_t dstb = ((uint32_t)t->tmem + kk) * 8;
            if (dstb >= 0x1000 || !load_raw_fast(r, src + kk * 8, dstb, odd, (k1 - kk) * 8)) break;
            kk = k1;
        }
    }
    for (uint32_t k = kk; k < nq; k++) {
        int odd = (int)(((k * dxt) >> 11) & 1);
        copy_src(r, src + k * 8, buf, 8);
        if (kind == LOAD_RAW) {
            put_raw_bytes(r, ((uint32_t)t->tmem + k) * 8, odd, buf, 8);
        } else if (kind == LOAD_RGBA32) {
            put_rgba32_texel(r, (uint32_t)t->tmem * 4 + k * 2, odd, buf);
            put_rgba32_texel(r, (uint32_t)t->tmem * 4 + k * 2 + 1, odd, buf + 4);
        } else {
            put_yuv_pair(r, (uint32_t)t->tmem * 4 + k * 2, odd, buf);
            put_yuv_pair(r, (uint32_t)t->tmem * 4 + k * 2 + 1, odd, buf + 4);
        }
    }
}

void tmem_load_tlut(struct rdp *r, const uint32_t *a)
{
    struct tile *t = &r->tile[(a[1] >> 24) & 7];
    t->sl = (a[0] >> 12) & 0xfff; t->tl = a[0] & 0xfff;
    t->sh = (a[1] >> 12) & 0xfff; t->th = a[1] & 0xfff;
    tile_update_derived(t);

    if (r->ti_size != PIXEL_SIZE_16BIT) return; /* TLUT entries are 16-bit */
    int s0 = t->sl >> 2, s1 = t->sh >> 2, y = t->tl >> 2;
    if (s1 < s0) return;
    int n = s1 - s0 + 1;
    uint32_t src = r->ti_address + (uint32_t)((y * r->ti_width + s0) * 2);
    if (r->mt) mt_sync_range(r, src, src + (uint32_t)n * 2);
    r->tmem_dirty = true;
    for (int i = 0; i < n; i++) {
        uint8_t hi = rdram_rd8(r, src + (uint32_t)i * 2), lo = rdram_rd8(r, src + (uint32_t)i * 2 + 1);
        uint32_t base = ((uint32_t)t->tmem * 8 + (uint32_t)i * 8) & 0xfff;
        for (int c = 0; c < 4; c++) {
            r->tmem[(base + (uint32_t)c * 2) & 0xfff] = hi;
            r->tmem[(base + (uint32_t)c * 2 + 1) & 0xfff] = lo;
        }
    }
}
