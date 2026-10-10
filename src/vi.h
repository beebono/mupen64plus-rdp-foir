#ifndef FOIR_VI_H
#define FOIR_VI_H

#include "rdp.h"

#define VI_MAX_W 1024
#define VI_MAX_H 768

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

void vi_render(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out);
void vi_render_ex(const struct rdp *r, const struct vi_regs *regs, struct vi_frame *out, bool allow_raw16);

#endif
