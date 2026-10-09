/* Internal declarations shared between the scalar kernel translation units. */
#ifndef FOIR_KERNELS_PRIV_H
#define FOIR_KERNELS_PRIV_H
#include "rdp.h"

void scalar_texture(struct rdp *r, struct px_batch *b);
void scalar_copy_fetch_init(void);

/* texture coordinate helpers reused by the COPY kernel */
void tc_divide(const struct rdp *r, int32_t ss, int32_t st, int32_t sw, int32_t *sss, int32_t *sst);
void tc_clamp17(int32_t *sss, int32_t *sst);
void kernels_tex_init_tables(void);

/* Fixed shared tables */
extern uint32_t k_clamp9[512];
extern int32_t k_ext9[512];
extern uint8_t k_bldiv[0x8000];
extern int32_t k_tcdiv[0x8000];
extern int32_t k_maskbits[16];
#endif
