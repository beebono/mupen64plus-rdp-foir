/*
 * mupen64plus-rdp-foir - kneon.h
 * Shared helpers for the NEON kernel set. Everything here operates on 8 pixels
 * in 16-bit lanes (int32 only where a multiply needs the headroom).
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_KNEON_H
#define FOIR_KNEON_H

#include "kernels_priv.h"
#include <arm_neon.h>

/* the scalar kernel table entries, used as fallbacks for configurations a NEON kernel does not handle */
extern const struct foir_kernels foir_kernels_scalar;

/* NEON kernel entry points (one translation unit each) */
void neon_texture(struct rdp *r, struct px_batch *b);
void neon_combine(struct rdp *r, struct px_batch *b);
void neon_zcmp(struct rdp *r, struct px_batch *b);
void neon_blend(struct rdp *r, struct px_batch *b);
void neon_store(struct rdp *r, struct px_batch *b);
void neon_fill(struct rdp *r, int y, int x0, int x1);
void neon_copy(struct rdp *r, int y, const struct span *sp, int flip);

void neon_tables_init(void);

/* debug counters of scalar fallbacks and handled batches (build with -DFOIR_STATS) */
#ifdef FOIR_STATS
extern unsigned long neon_stat_fb[9], neon_stat_ok[9];     /* 0..4 stages, 5 fill, 6 / 7 zcmp fast path / dead Z groups, 8 copy */
#define NEON_FALLBACK(i) (neon_stat_fb[i]++)
#define NEON_HANDLED(i)  (neon_stat_ok[i]++)
#else
#define NEON_FALLBACK(i) ((void)0)
#define NEON_HANDLED(i)  ((void)0)
#endif

#define NGROUPS(n) (((n) + 7) >> 3)

/* sign-extend the low 9 bits of every lane */
FOIR_INLINE int16x8_t k_sext9(int16x8_t x) { return vshrq_n_s16(vshlq_n_s16(x, 7), 7); }

/* k_ext9[] of the RDP (combiner A, B, D inputs): 9-bit values where 0x180..0x1ff are negative but
 * 0x100..0x17f stay positive (range -128..383) */
FOIR_INLINE int16x8_t k_ext9v(int16x8_t x)
{
    int16x8_t v = vandq_s16(x, vdupq_n_s16(0x1ff));
    uint16x8_t neg = vceqq_s16(vandq_s16(v, vdupq_n_s16(0x180)), vdupq_n_s16(0x180));
    return vbslq_s16(neg, vsubq_s16(v, vdupq_n_s16(0x200)), v);
}

/* k_clamp9[] of the RDP: 0..255 pass, 256..383 -> 255, 384..511 -> 0 (input masked to 9 bits) */
FOIR_INLINE int16x8_t k_clamp9v(int16x8_t v)
{
    v = vandq_s16(v, vdupq_n_s16(0x1ff));
    uint16x8_t u = vreinterpretq_u16_s16(v);
    uint16x8_t lt256 = vcltq_u16(u, vdupq_n_u16(256));
    uint16x8_t lt384 = vcltq_u16(u, vdupq_n_u16(384));
    return vbslq_s16(lt256, v, vreinterpretq_s16_u16(vandq_u16(lt384, vdupq_n_u16(255))));
}

/* 8 lanes of (((a - b) * c + (d << 8) + 0x80) >> 8) & 0x1ff with 9-bit sign extended operands */
FOIR_INLINE int16x8_t k_comb9(int16x8_t a, int16x8_t b, int16x8_t c, int16x8_t d)
{
    a = k_ext9v(a); b = k_ext9v(b); c = k_sext9(c); d = k_ext9v(d);
    int16x8_t ab = vsubq_s16(a, b);
    int32x4_t rnd = vdupq_n_s32(0x80);
    int32x4_t lo = vaddq_s32(vaddq_s32(vmull_s16(vget_low_s16(ab), vget_low_s16(c)), vshll_n_s16(vget_low_s16(d), 8)), rnd);
    int32x4_t hi = vaddq_s32(vaddq_s32(vmull_high_s16(ab, c), vshll_high_n_s16(d, 8)), rnd);
    int16x8_t res = vcombine_s16(vshrn_n_s32(lo, 8), vshrn_n_s32(hi, 8));
    return vandq_s16(res, vdupq_n_s16(0x1ff));
}

/* same hash as prand() in kernels_scalar.c, four lanes */
FOIR_INLINE uint32x4_t k_prand4(uint32x4_t idx, uint32_t seed, uint32_t salt)
{
    uint32x4_t h = veorq_u32(vmulq_n_u32(idx, 0x9e3779b1u), vdupq_n_u32(seed ^ (salt * 0x85ebca77u)));
    h = veorq_u32(h, vshrq_n_u32(h, 15)); h = vmulq_n_u32(h, 0x85ebca6bu);
    h = veorq_u32(h, vshrq_n_u32(h, 13)); h = vmulq_n_u32(h, 0xc2b2ae35u);
    return veorq_u32(h, vshrq_n_u32(h, 16));
}

FOIR_INLINE uint16x8_t k_widen_mask8(uint8x8_t m)    /* 0/non-0 bytes -> 0/0xffff lanes */
{
    return vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vcgt_u8(m, vdup_n_u8(0)))));
}

FOIR_INLINE int16x8_t k_ld8u(const uint8_t *p) { return vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p))); }

#endif
