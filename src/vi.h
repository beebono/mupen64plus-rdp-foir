/* Minimal VI model: crop/scale the active framebuffer to one RGBA texture. */
#ifndef FOIR_VI_H
#define FOIR_VI_H

#include "rdp.h"

#define VI_MAX_W 1024
#define VI_MAX_H 768

struct vi_regs {
    uint32_t status, origin, width, h_start, v_start, x_scale, y_scale;
};

struct vi_frame {
    uint32_t pixels[VI_MAX_W * VI_MAX_H]; /* RGBA bytes in memory order R,G,B,A */
    int width, height;
    bool valid;      /* false when the VI is blank */
    bool raw16;      /* pixels hold w*h RGBA5551 halfwords (GL_UNSIGNED_SHORT_5_5_5_1) instead of RGBA8888 */
    /* fraction of the nominal 640x480 (NTSC) / 640x576 (PAL) screen the picture covers (0..1) */
    float screen_w, screen_h;
};

/* Reads the framebuffer described by `regs` out of RDRAM into `out`. */
/* `interlace_hack`: for interlaced output of field-rendered frames show only the freshest field, line doubled */
void vi_render(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out);
/* Like vi_render; with `allow_raw16` a 16-bit framebuffer is passed through as RGBA5551 (half the memory traffic,
 * converted by the GPU) -- for presentation only. */
void vi_render_ex(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out, bool allow_raw16);

#endif
