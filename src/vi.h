#ifndef FOIR_VI_H
#define FOIR_VI_H

#include "rdp.h"

#define VI_MAX_W 1024
#define VI_MAX_H 768

/* Overscan crop (CropOverscanH / CropOverscanV), per edge, in VI pixels of a low-resolution (<= 320 wide / 240 line) image.
 * A hi-res image (width >= 512) crops twice as many columns, a frame of >= 360 lines twice as many lines, so the same
 * part of the picture goes.  An axis whose image would drop below 32 pixels is left alone. */
#define VI_CROP_H 8
#define VI_CROP_V 12

struct vi_regs {
    uint32_t status, origin, width, h_start, v_start, x_scale, y_scale;
};

struct vi_frame {
    uint32_t pixels[VI_MAX_W * VI_MAX_H];
    int width, height;
    bool valid;
    bool raw16;      /* RGBA5551 halfwords instead of RGBA8888 */
    float screen_w, screen_h;
};

/* VI filter pass (VIFilters): AA from coverage, divot, dither restore and gamma, as angrylion-rdp-plus does at an exact source pixel */
struct vi_filter_ctx {
    uint32_t base, stride;     /* first pixel (in 16 or 32-bit units) and the line width */
    bool t32, divot, gamma, dither;
    int aa;                    /* VI_STATUS aa_mode */
    uint32_t gen;              /* one per setup (per frame): invalidates the converted-line cache */
};
bool vi_filter_setup(const struct rdp *r, const struct vi_regs *regs, struct vi_filter_ctx *c);
/* n output pixels of source line sy starting at source column x0 */
void vi_filter_row(const struct rdp *r, const struct vi_filter_ctx *c, int sy, int x0, int n, uint32_t *dst);
void vi_filter_row_ref(const struct rdp *r, const struct vi_filter_ctx *c, int sy, int x0, int n, uint32_t *dst);

void vi_render(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out);
void vi_render_ex(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out, bool allow_raw16);

#endif
