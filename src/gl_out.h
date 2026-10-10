#ifndef FOIR_GL_OUT_H
#define FOIR_GL_OUT_H

#include "foir.h"

struct gl_out_params {
    int win_width, win_height;
    bool fullscreen, vsync, filter, widescreen;
};

bool gl_out_init(void *core, const struct gl_out_params *p);
void gl_out_shutdown(void);
void gl_out_resize(int w, int h);
void gl_out_toggle_fullscreen(void);
void gl_out_present(const void *pixels, bool raw16, int w, int h, float screen_w, float screen_h, void (*pre_swap_cb)(int));
bool gl_out_active(void);
void gl_out_set_filter(bool on);
void gl_out_set_widescreen(bool on);

#endif
