/* GLES2 output through the core's video extension (no direct GL linking). */
#ifndef FOIR_GL_OUT_H
#define FOIR_GL_OUT_H

#include "foir.h"

struct gl_out_params {
    int win_width, win_height;
    bool fullscreen, vsync, filter, widescreen;
};

/* `core` is the dlopen handle given to PluginStartup */
bool gl_out_init(void *core, const struct gl_out_params *p);
void gl_out_shutdown(void);
void gl_out_resize(int w, int h);
void gl_out_toggle_fullscreen(void);
/* uploads the picture (w*h, tightly packed RGBA8888, or RGBA5551 halfwords with raw16) and swaps; pixels==NULL draws black */
void gl_out_present(const void *pixels, bool raw16, int w, int h, float screen_w, float screen_h, void (*pre_swap_cb)(int));
bool gl_out_active(void);

#endif
