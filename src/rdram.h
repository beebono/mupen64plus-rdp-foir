#ifndef FOIR_RDRAM_H
#define FOIR_RDRAM_H

#define RDRAM_MASK 0x00ffffffu

FOIR_INLINE uint8_t rdram_rd8(const struct rdp *r, uint32_t a)
{
    a &= RDRAM_MASK;
    return a < r->rdram_size ? r->rdram[a ^ FOIR_BYTE_XOR] : 0;
}
FOIR_INLINE void rdram_wr8(struct rdp *r, uint32_t a, uint8_t v)
{
    a &= RDRAM_MASK;
    if (a < r->rdram_size) r->rdram[a ^ FOIR_BYTE_XOR] = v;
}
FOIR_INLINE uint16_t rdram_rd16(const struct rdp *r, uint32_t i)
{
    i &= RDRAM_MASK >> 1;
    return (i << 1) < r->rdram_size ? ((const uint16_t *)r->rdram)[i ^ FOIR_HALF_XOR] : 0;
}
FOIR_INLINE void rdram_wr16(struct rdp *r, uint32_t i, uint16_t v)
{
    i &= RDRAM_MASK >> 1;
    if ((i << 1) < r->rdram_size) ((uint16_t *)r->rdram)[i ^ FOIR_HALF_XOR] = v;
}
FOIR_INLINE uint32_t rdram_rd32(const struct rdp *r, uint32_t i)
{
    i &= RDRAM_MASK >> 2;
    return (i << 2) < r->rdram_size ? ((const uint32_t *)r->rdram)[i] : 0;
}
FOIR_INLINE void rdram_wr32(struct rdp *r, uint32_t i, uint32_t v)
{
    i &= RDRAM_MASK >> 2;
    if ((i << 2) < r->rdram_size) ((uint32_t *)r->rdram)[i] = v;
}

/* hidden byte holds the pixel LSB it was written with; if the CPU rewrote the pixel, fall back to the "lsb replicated" convention */
FOIR_INLINE void rdram_rd_pair16(const struct rdp *r, uint32_t i, uint16_t *px, uint8_t *hv)
{
    i &= RDRAM_MASK >> 1;
    if ((i << 1) >= r->rdram_size) { *px = 0; *hv = 0; return; }
    uint16_t p = ((const uint16_t *)r->rdram)[i ^ FOIR_HALF_XOR];
    uint8_t h = r->hidden[i];
    *px = p;
    if ((h & 8) && (((h >> 2) & 1) == (p & 1)))
        *hv = h & 3;
    else
        *hv = (p & 1) ? 3 : 0;
}
FOIR_INLINE void rdram_wr_pair16(struct rdp *r, uint32_t i, uint16_t px, uint8_t hv)
{
    i &= RDRAM_MASK >> 1;
    if ((i << 1) >= r->rdram_size) return;
    ((uint16_t *)r->rdram)[i ^ FOIR_HALF_XOR] = px;
    r->hidden[i] = 8 | ((px & 1) << 2) | (hv & 3);
}

#endif
