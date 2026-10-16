/*
 * mupen64plus-rdp-foir - vi_filter.c
 *
 * The VI filter pass: coverage anti-aliasing, divot, dither restore ("dither filter") and gamma, bit-exact against
 * angrylion-rdp-plus (n64video/vi/) for the pixels the VI reads at an exact source position.  What it leaves out:
 * the VI's own resampling between source pixels (the scale to the window does that), the random noise of
 * gamma_dither_enable (as for the RDP's noise, it does not follow the hardware sequence, so it is not added), and the
 * hardware glitch angrylion models for aa_mode 3 with a small H_START.
 *
 * vi_filter_row_ref() is a straight port of the oracle (it reads RDRAM through the same accessors); vi_filter_row()
 * is the NEON version for 16-bit frame buffers and falls back to the reference for 32-bit ones.  Output pixels are
 * RGBA8888.  Intentional divergence from angrylion: for 16-bit buffers the 8-bit result of the whole pass (after AA, divot,
 * dither restore and gamma, which all work on the 5-bit values shifted up by 3 as in the oracle) is expanded to full range as
 * v + (v >> 5), clamped to 255, the same mapping the plain conversion has (5-bit replication, white = 255), where angrylion leaves
 * white at 248.  32-bit buffers already hold 8-bit values and are output as they are.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vi.h"
#if defined(FOIR_NEON)
#include <arm_neon.h>
#endif

enum { AA_RESAMP_EXTRA_ALWAYS, AA_RESAMP_EXTRA, AA_RESAMP_ONLY, AA_REPLICATE };

struct px { int r, g, b; uint32_t a; };

static uint8_t g_gamma[256];
static bool g_gamma_ready;

static uint32_t isqrt_u32(uint32_t a)
{
    unsigned long op = a, res = 0, one = 1ul << 30;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) { op -= res + one; res += one << 1; }
        res >>= 1;
        one >>= 2;
    }
    return (uint32_t)res;
}

static void gamma_init(void)
{
    if (g_gamma_ready) return;
    for (int i = 0; i < 256; i++) g_gamma[i] = (uint8_t)(isqrt_u32((uint32_t)i << 6) << 1);
    g_gamma_ready = true;
}

bool vi_filter_setup(const struct rdp *r, const struct vi_regs *regs, struct vi_filter_ctx *c)
{
    uint32_t st = regs->status;
    c->t32 = (st & 3) == 3;
    c->stride = regs->width & 0xfff;
    c->base = (regs->origin & 0xffffff) >> (c->t32 ? 2 : 1);
    /* a filter runs when the game enables it and the option allows it; AA off reads the buffer as aa_mode 2 (full coverage) */
    c->aa = (int)((st >> 8) & 3);
    if (!r->cfg.vi_aa && c->aa <= AA_RESAMP_EXTRA) c->aa = AA_RESAMP_ONLY;
    c->divot = ((st >> 4) & 1) && r->cfg.vi_divot;
    c->gamma = ((st >> 3) & 1) && r->cfg.vi_gamma;
    c->dither = ((st >> 16) & 1) && r->cfg.vi_dedither;
    /* nothing left to do: the caller takes the plain conversion */
    if (c->aa > AA_RESAMP_EXTRA && !c->divot && !c->gamma && !c->dither) return false;
    static uint32_t gen;
    c->gen = ++gen;
    gamma_init();
    return true;
}

/* ---- reference: the oracle's code, pixel by pixel ---- */

static const int *restore_tab(void)
{
    static int t[0x400];
    static bool ready;
    if (!ready) {
        for (int i = 0; i < 0x400; i++) {
            int a = (i >> 5) & 0x1f, b = i & 0x1f;
            t[i] = a < b ? 1 : (a > b ? -1 : 0);
        }
        ready = true;
    }
    return t;
}

static void max_opt(const uint32_t *pixels, uint32_t *penumin, uint32_t *penumax, int n)
{
    int posmax = 0, posmin = 0;
    uint32_t curpenmax = pixels[0], curpenmin = pixels[0];
    for (int i = 1; i < n; i++) {
        if (pixels[i] > pixels[posmax]) { curpenmax = pixels[posmax]; posmax = i; }
        else if (pixels[i] < pixels[posmin]) { curpenmin = pixels[posmin]; posmin = i; }
    }
    uint32_t max = pixels[posmax], min = pixels[posmin];
    if (curpenmax != max)
        for (int i = posmax + 1; i < n; i++) if (pixels[i] > curpenmax) curpenmax = pixels[i];
    if (curpenmin != min)
        for (int i = posmin + 1; i < n; i++) if (pixels[i] < curpenmin) curpenmin = pixels[i];
    *penumax = curpenmax;
    *penumin = curpenmin;
}

static void aa_filter(int *endr, int *endg, int *endb, const uint32_t dirs[6], const uint32_t pixr[6], const uint32_t pixg[6],
                      const uint32_t pixb[6], const bool full[6], uint32_t centercvg)
{
    uint32_t backr[7], backg[7], backb[7], n = 1;
    uint32_t r = (uint32_t)*endr, g = (uint32_t)*endg, b = (uint32_t)*endb;
    (void)dirs;
    backr[0] = r; backg[0] = g; backb[0] = b;
    for (int i = 0; i < 6; i++)
        if (full[i]) { backr[n] = pixr[i]; backg[n] = pixg[i]; backb[n] = pixb[i]; n++; }
    uint32_t minr, maxr, ming, maxg, minb, maxb;
    max_opt(backr, &minr, &maxr, (int)n);
    max_opt(backg, &ming, &maxg, (int)n);
    max_opt(backb, &minb, &maxb, (int)n);
    uint32_t coeff = 7 - centercvg;
    uint32_t colr = minr + maxr - (r << 1), colg = ming + maxg - (g << 1), colb = minb + maxb - (b << 1);
    colr = (((colr * coeff) + 4) >> 3) + r;
    colg = (((colg * coeff) + 4) >> 3) + g;
    colb = (((colb * coeff) + 4) >> 3) + b;
    *endr = (int)(colr & 0xff); *endg = (int)(colg & 0xff); *endb = (int)(colb & 0xff);
}

/* pos = pixel index relative to the buffer start (row * stride + x), wraps like the oracle's uint32 arithmetic */
static void fetch_ref(const struct rdp *r, const struct vi_filter_ctx *c, uint32_t pos, struct px *res)
{
    const uint32_t hres = c->stride;
    const uint32_t idx = c->base + pos;
    int cr, cg, cb;
    uint32_t cvg;
    uint32_t dirs[8];
    const uint32_t leftup = idx - hres - 1;
    const uint32_t toleftpix = idx - 1;
    if (!c->t32) {
        uint16_t pix;
        uint8_t hval;
        if (c->aa <= AA_RESAMP_EXTRA) { rdram_rd_pair16(r, idx, &pix, &hval); cvg = ((uint32_t)(pix & 1) << 2) | hval; }
        else { pix = rdram_rd16(r, idx); cvg = 7; }
        cr = (pix >> 8) & 0xf8; cg = (pix & 0x7c0) >> 3; cb = (pix & 0x3e) << 2;
        if (cvg == 7) {
            if (c->dither) {
                const int *tab = restore_tab();
                const uint32_t ld = idx + hres - 1, mx = idx + hres + 1;
                dirs[0] = leftup; dirs[1] = leftup + 1; dirs[2] = leftup + 2; dirs[3] = ld;
                dirs[4] = ld + 1; dirs[5] = mx; dirs[6] = toleftpix; dirs[7] = toleftpix + 2;
                int re = cr, ge = cg, be = cb;
                const int *rp = &tab[(cr << 2) & 0x3e0], *gp = &tab[(cg << 2) & 0x3e0], *bp = &tab[(cb << 2) & 0x3e0];
                for (int i = 0; i < 8; i++) {
                    uint16_t p = rdram_rd16(r, dirs[i]);
                    re += rp[(p >> 11) & 0x1f]; ge += gp[(p >> 6) & 0x1f]; be += bp[(p >> 1) & 0x1f];
                }
                cr = re; cg = ge; cb = be;
            }
        } else {
            const uint32_t toleft = idx - 2, toright = idx + 2;
            dirs[0] = leftup; dirs[1] = idx - hres + 1; dirs[2] = toleft; dirs[3] = toright;
            dirs[4] = idx + hres - 1; dirs[5] = idx + hres + 1;
            uint32_t pr[6], pg[6], pb[6];
            bool full[6];
            for (int i = 0; i < 6; i++) {
                uint16_t p; uint8_t hv;
                rdram_rd_pair16(r, dirs[i], &p, &hv);
                full[i] = hv == 3 && (p & 1);
                pr[i] = (uint32_t)((p >> 8) & 0xf8); pg[i] = (uint32_t)((p & 0x7c0) >> 3); pb[i] = (uint32_t)((p & 0x3e) << 2);
            }
            aa_filter(&cr, &cg, &cb, dirs, pr, pg, pb, full, cvg);
        }
    } else {
        uint32_t pix = rdram_rd32(r, idx);
        cvg = c->aa <= AA_RESAMP_EXTRA ? (pix >> 5) & 7 : 7;
        cr = (int)((pix >> 24) & 0xff); cg = (int)((pix >> 16) & 0xff); cb = (int)((pix >> 8) & 0xff);
        if (cvg == 7) {
            if (c->dither) {
                const int *tab = restore_tab();
                const uint32_t ld = idx + hres - 1, mx = idx + hres + 1;
                dirs[0] = leftup; dirs[1] = leftup + 1; dirs[2] = leftup + 2; dirs[3] = ld;
                dirs[4] = ld + 1; dirs[5] = mx; dirs[6] = toleftpix; dirs[7] = toleftpix + 2;
                int re = cr, ge = cg, be = cb;
                const int *rp = &tab[(cr << 2) & 0x3e0], *gp = &tab[(cg << 2) & 0x3e0], *bp = &tab[(cb << 2) & 0x3e0];
                for (int i = 0; i < 8; i++) {
                    uint32_t p = rdram_rd32(r, dirs[i]);
                    re += rp[(p >> 27) & 0x1f]; ge += gp[(p >> 19) & 0x1f]; be += bp[(p >> 11) & 0x1f];
                }
                cr = re; cg = ge; cb = be;
            }
        } else {
            const uint32_t toleft = idx - 2, toright = idx + 2;
            dirs[0] = leftup; dirs[1] = idx - hres + 1; dirs[2] = toleft; dirs[3] = toright;
            dirs[4] = idx + hres - 1; dirs[5] = idx + hres + 1;
            uint32_t pr[6], pg[6], pb[6];
            bool full[6];
            for (int i = 0; i < 6; i++) {
                uint32_t p = rdram_rd32(r, dirs[i]);
                full[i] = ((p >> 5) & 7) == 7;
                pr[i] = (p >> 24) & 0xff; pg[i] = (p >> 16) & 0xff; pb[i] = (p >> 8) & 0xff;
            }
            aa_filter(&cr, &cg, &cb, dirs, pr, pg, pb, full, cvg);
        }
    }
    res->r = cr & 0xff; res->g = cg & 0xff; res->b = cb & 0xff; res->a = cvg;
}

static void divot_one(struct px *f, const struct px *c, const struct px *l, const struct px *rt)
{
    *f = *c;
    if ((c->a & l->a & rt->a) == 7) return;
#define CH(ch) do { \
        if ((l->ch >= c->ch && rt->ch >= l->ch) || (l->ch >= rt->ch && c->ch >= l->ch)) f->ch = l->ch; \
        else if ((rt->ch >= c->ch && l->ch >= rt->ch) || (rt->ch >= l->ch && c->ch >= rt->ch)) f->ch = rt->ch; \
    } while (0)
    CH(r); CH(g); CH(b);
#undef CH
}

static void row_out(const struct vi_filter_ctx *c, const struct px *p, int n, uint32_t *dst)
{
    for (int i = 0; i < n; i++) {
        uint32_t rr = (uint32_t)p[i].r, gg = (uint32_t)p[i].g, bb = (uint32_t)p[i].b;
        if (c->gamma) { rr = g_gamma[rr]; gg = g_gamma[gg]; bb = g_gamma[bb]; }
        if (!c->t32) {   /* full-range expansion of 16-bit buffers, see the header */
            rr = rr + (rr >> 5); gg = gg + (gg >> 5); bb = bb + (bb >> 5);
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
        }
        dst[i] = 0xff000000u | (bb << 16) | (gg << 8) | rr;
    }
}

void vi_filter_row_ref(const struct rdp *r, const struct vi_filter_ctx *c, int sy, int x0, int n, uint32_t *dst)
{
    struct px f[VI_MAX_W + 2], out[VI_MAX_W];
    const uint32_t row = (uint32_t)sy * c->stride;
    if (n > VI_MAX_W) n = VI_MAX_W;
    if (c->divot) {
        for (int i = 0; i < n + 2; i++) fetch_ref(r, c, row + (uint32_t)(x0 + i - 1), &f[i]);
        for (int i = 0; i < n; i++) divot_one(&out[i], &f[i + 1], &f[i], &f[i + 2]);
    } else {
        for (int i = 0; i < n; i++) fetch_ref(r, c, row + (uint32_t)(x0 + i), &out[i]);
    }
    row_out(c, out, n, dst);
}

#if defined(FOIR_NEON) && FOIR_HALF_XOR == 1
/* ---- NEON, 16-bit frame buffers ---- */

#define M 3                       /* margin of a scratch line: the AA filter reaches two pixels, the divot one more */
#define SW (VI_MAX_W + 64 + 2 * M)

/* one source line as planes of 5-bit channels plus the coverage; index M + j is the pixel at source column x0 - 1 + j */
struct vline {
    uint8_t pl[3][SW];            /* r, g, b */
    uint8_t cv[SW];               /* coverage 0..7 as the VI sees it (7 when aa_mode >= 2) */
    int line;
};

/* the last four lines converted: successive output lines need lines sy-1, sy, sy+1, so each line is converted once per frame */
static struct { struct vline l[4]; uint32_t gen; int x0, span; } g_cache;

/* pixels idx0 .. idx0 + n - 1 of the buffer (uint32 wrap and RDRAM limits as in rdram_rd16) into the line's planes */
static void load_span(const struct rdp *r, const struct vi_filter_ctx *c, uint32_t idx0, int n, struct vline *L)
{
    const bool wantcvg = c->aa <= AA_RESAMP_EXTRA;
    uint8_t *const pr = L->pl[0], *const pg = L->pl[1], *const pb = L->pl[2], *const cv = L->cv;
    uint32_t i0 = idx0 & (RDRAM_MASK >> 1);
    const uint16_t *h = (const uint16_t *)r->rdram;
    if (((i0 + (uint32_t)n) << 1) <= r->rdram_size && i0 + (uint32_t)n <= (RDRAM_MASK >> 1) + 1) {
        const uint8_t *hid = r->hidden + i0;
        const uint8x8_t m31 = vdup_n_u8(31), one = vdup_n_u8(1);
        int k = 0;
#define SCALAR_PX(k) do { \
            uint16_t p = h[(i0 + (uint32_t)(k)) ^ 1]; \
            pr[k] = (uint8_t)(p >> 11); pg[k] = (uint8_t)((p >> 6) & 31); pb[k] = (uint8_t)((p >> 1) & 31); \
            if (wantcvg) { \
                uint8_t hb = hid[k], hv = ((hb & 8) && (((hb >> 2) & 1) == (p & 1))) ? (hb & 3) : ((p & 1) ? 3 : 0); \
                cv[k] = (uint8_t)(((p & 1) << 2) | hv); \
            } else cv[k] = 7; \
        } while (0)
        /* an odd first pixel, then even-aligned runs of 8 (the XOR of the halfword index is a swap inside each pair) */
        for (; k < n && ((i0 + (uint32_t)k) & 1); k++) SCALAR_PX(k);
        for (; k + 8 <= n; k += 8) {
            uint16x8_t p = vrev32q_u16(vld1q_u16(h + i0 + (uint32_t)k));
            vst1_u8(pr + k, vmovn_u16(vshrq_n_u16(p, 11)));
            vst1_u8(pg + k, vand_u8(vmovn_u16(vshrq_n_u16(p, 6)), m31));
            vst1_u8(pb + k, vand_u8(vmovn_u16(vshrq_n_u16(p, 1)), m31));
            if (wantcvg) {
                uint8x8_t h8 = vld1_u8(hid + k);
                uint8x8_t lsb = vand_u8(vmovn_u16(p), one);
                uint8x8_t valid = vand_u8(vtst_u8(h8, vdup_n_u8(8)), vceq_u8(vand_u8(vshr_n_u8(h8, 2), one), lsb));
                uint8x8_t hv = vbsl_u8(valid, vand_u8(h8, vdup_n_u8(3)), vmul_u8(lsb, vdup_n_u8(3)));
                vst1_u8(cv + k, vorr_u8(vshl_n_u8(lsb, 2), hv));
            } else vst1_u8(cv + k, vdup_n_u8(7));
        }
        for (; k < n; k++) SCALAR_PX(k);
#undef SCALAR_PX
        return;
    }
    for (int k = 0; k < n; k++) {
        uint16_t p; uint8_t hv;
        rdram_rd_pair16(r, idx0 + (uint32_t)k, &p, &hv);
        pr[k] = (uint8_t)(p >> 11); pg[k] = (uint8_t)((p >> 6) & 31); pb[k] = (uint8_t)((p >> 1) & 31);
        cv[k] = wantcvg ? (uint8_t)(((p & 1) << 2) | hv) : 7;
    }
}

static const struct vline *get_line(const struct rdp *r, const struct vi_filter_ctx *c, int line, int x0, int span)
{
    if (g_cache.gen != c->gen || g_cache.x0 != x0 || g_cache.span != span) {
        for (int i = 0; i < 4; i++) g_cache.l[i].line = INT32_MIN;
        g_cache.gen = c->gen; g_cache.x0 = x0; g_cache.span = span;
    }
    struct vline *L = &g_cache.l[(unsigned)line & 3];
    if (L->line != line) {
        load_span(r, c, c->base + (uint32_t)line * c->stride + (uint32_t)(x0 - 1 - M), span, L);
        L->line = line;
    }
    return L;
}

/* the AA filter of a partially covered pixel at index k from the converted lines (the same neighbours, in the same order, as fetch_ref) */
static void aa_lane(const struct vline *up, const struct vline *mid, const struct vline *dn, int k, uint8_t out[3])
{
    const struct vline *rows[6] = { up, up, mid, mid, dn, dn };
    static const int offs[6] = { -1, 1, -2, 2, -1, 1 };
    uint32_t pr[6], pg[6], pb[6];
    bool full[6];
    for (int i = 0; i < 6; i++) {
        const int q = k + offs[i];
        full[i] = rows[i]->cv[q] == 7;
        pr[i] = (uint32_t)rows[i]->pl[0][q] << 3; pg[i] = (uint32_t)rows[i]->pl[1][q] << 3; pb[i] = (uint32_t)rows[i]->pl[2][q] << 3;
    }
    int cr = mid->pl[0][k] << 3, cg = mid->pl[1][k] << 3, cb = mid->pl[2][k] << 3;
    aa_filter(&cr, &cg, &cb, NULL, pr, pg, pb, full, mid->cv[k]);
    out[0] = (uint8_t)cr; out[1] = (uint8_t)cg; out[2] = (uint8_t)cb;
}

/* 16 fetch results at scratch index k.  The dither restore runs on every lane as if it were fully covered (it only means something for those);
 * lanes with coverage below 7 are then redone with the AA filter.  lim = the number of lanes that matter. */
static void fetch_group(const struct vi_filter_ctx *c, const struct vline *up, const struct vline *mid, const struct vline *dn, int k,
                        uint8_t *fr, uint8_t *fg, uint8_t *fb, uint8_t *fa, int lim)
{
    uint8x16_t cvg = vld1q_u8(mid->cv + k);
    uint8x16_t chv[3];
    for (int q = 0; q < 3; q++) {
        uint8x16_t c5 = vld1q_u8(mid->pl[q] + k);
        uint8x16_t v8 = vshlq_n_u8(c5, 3);
        if (c->dither) {
            int8x16_t acc = vdupq_n_s8(0);
#define NB(L, off) do { \
                uint8x16_t n = vld1q_u8((L)->pl[q] + k + (off)); \
                acc = vsubq_s8(acc, vreinterpretq_s8_u8(vcltq_u8(c5, n))); \
                acc = vaddq_s8(acc, vreinterpretq_s8_u8(vcgtq_u8(c5, n))); \
            } while (0)
            NB(up, -1); NB(up, 0); NB(up, 1); NB(dn, -1); NB(dn, 0); NB(dn, 1); NB(mid, -1); NB(mid, 1);
#undef NB
            v8 = vaddq_u8(v8, vreinterpretq_u8_s8(acc));
        }
        chv[q] = v8;
    }
    vst1q_u8(fr, chv[0]); vst1q_u8(fg, chv[1]); vst1q_u8(fb, chv[2]);
    vst1q_u8(fa, cvg);
    if (vminvq_u8(cvg) != 7) {
        uint8_t cvs[16];
        vst1q_u8(cvs, cvg);
        for (int i = 0; i < 16 && i < lim; i++) {
            if (cvs[i] == 7) continue;
            uint8_t o[3];
            aa_lane(up, mid, dn, k + i, o);
            fr[i] = o[0]; fg[i] = o[1]; fb[i] = o[2];
        }
    }
}

static inline uint8x16_t divot_ch(uint8x16_t c, uint8x16_t l, uint8x16_t rt)
{
    uint8x16_t c1 = vorrq_u8(vandq_u8(vcgeq_u8(l, c), vcgeq_u8(rt, l)), vandq_u8(vcgeq_u8(l, rt), vcgeq_u8(c, l)));
    uint8x16_t c2 = vorrq_u8(vandq_u8(vcgeq_u8(rt, c), vcgeq_u8(l, rt)), vandq_u8(vcgeq_u8(rt, l), vcgeq_u8(c, rt)));
    return vbslq_u8(c1, l, vbslq_u8(c2, rt, c));
}

void vi_filter_row(const struct rdp *r, const struct vi_filter_ctx *c, int sy, int x0, int n, uint32_t *dst)
{
    if (c->t32 || !r->cfg.neon || n <= 0 || n > VI_MAX_W) { vi_filter_row_ref(r, c, sy, x0, n, dst); return; }
    /* fetch values for positions x0-1 .. x0+n in groups of 16 (fetch index j is position x0 - 1 + j), plus one group so that the last output vector
     * can read its right neighbour */
    uint8_t fr[VI_MAX_W + 64], fg[VI_MAX_W + 64], fb[VI_MAX_W + 64], fa[VI_MAX_W + 64];
    const int ng = ((n + 15) >> 4) + 1;
    const int span = ng * 16 + 2 * M;
    const struct vline *up = get_line(r, c, sy - 1, x0, span), *mid = get_line(r, c, sy, x0, span), *dn = get_line(r, c, sy + 1, x0, span);
    /* get_line may have recycled a slot that an earlier call returned: slots are line & 3, and sy-1, sy, sy+1 differ in that, so all three are valid */
    for (int g = 0; g < ng; g++)
        fetch_group(c, up, mid, dn, M + 16 * g, fr + 16 * g, fg + 16 * g, fb + 16 * g, fa + 16 * g, n + 2 - 16 * g);
    uint8x16x4_t t0 = vld1q_u8_x4(g_gamma), t1 = vld1q_u8_x4(g_gamma + 64), t2 = vld1q_u8_x4(g_gamma + 128), t3 = vld1q_u8_x4(g_gamma + 192);
#define GAMMA(v) vorrq_u8(vorrq_u8(vqtbl4q_u8(t0, v), vqtbl4q_u8(t1, vsubq_u8(v, vdupq_n_u8(64)))), \
                          vorrq_u8(vqtbl4q_u8(t2, vsubq_u8(v, vdupq_n_u8(128))), vqtbl4q_u8(t3, vsubq_u8(v, vdupq_n_u8(192)))))
    for (int i = 0; i < n; i += 16) {
        uint8x16_t cr = vld1q_u8(fr + i + 1), cg = vld1q_u8(fg + i + 1), cb = vld1q_u8(fb + i + 1);
        if (c->divot) {
            uint8x16_t ca = vld1q_u8(fa + i + 1), la = vld1q_u8(fa + i), ra = vld1q_u8(fa + i + 2);
            uint8x16_t full = vceqq_u8(vandq_u8(vandq_u8(ca, la), ra), vdupq_n_u8(7));
            uint8x16_t dr = divot_ch(cr, vld1q_u8(fr + i), vld1q_u8(fr + i + 2));
            uint8x16_t dg = divot_ch(cg, vld1q_u8(fg + i), vld1q_u8(fg + i + 2));
            uint8x16_t db = divot_ch(cb, vld1q_u8(fb + i), vld1q_u8(fb + i + 2));
            cr = vbslq_u8(full, cr, dr); cg = vbslq_u8(full, cg, dg); cb = vbslq_u8(full, cb, db);
        }
        if (c->gamma) { cr = GAMMA(cr); cg = GAMMA(cg); cb = GAMMA(cb); }
        cr = vqaddq_u8(cr, vshrq_n_u8(cr, 5)); cg = vqaddq_u8(cg, vshrq_n_u8(cg, 5)); cb = vqaddq_u8(cb, vshrq_n_u8(cb, 5));   /* full range, as row_out() */
        uint8x16x4_t o = { { cr, cg, cb, vdupq_n_u8(0xff) } };
        if (i + 16 <= n) vst4q_u8((uint8_t *)(dst + i), o);
        else {
            uint32_t tmp[16];
            vst4q_u8((uint8_t *)tmp, o);
            memcpy(dst + i, tmp, (size_t)(n - i) * 4);
        }
    }
#undef GAMMA
}
#else
void vi_filter_row(const struct rdp *r, const struct vi_filter_ctx *c, int sy, int x0, int n, uint32_t *dst)
{
    vi_filter_row_ref(r, c, sy, x0, n, dst);
}
#endif
