/*
 * mupen64plus-rdp-foir - gl_out.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dlfcn.h>
#include <stdio.h>
#include "gl_out.h"
#include "m64p_types.h"
#include "m64p_vidext.h"

/* GL entry points come from VidExt_GL_GetProcAddress at runtime: no link-time GL dependency, works with front-ends that override the video extension */
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef float GLfloat;
typedef void GLvoid;

#define GL_FALSE 0
#define GL_TRIANGLE_STRIP 5
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_FLOAT 0x1406
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_ARRAY_BUFFER 0x8892
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44

#define GLFN(ret, name, args) static ret (*name) args
GLFN(void, glGenTextures, (GLsizei, GLuint *));
GLFN(void, glDeleteTextures, (GLsizei, const GLuint *));
GLFN(void, glBindTexture, (GLenum, GLuint));
GLFN(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *));
GLFN(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *));
GLFN(void, glTexParameteri, (GLenum, GLenum, GLint));
GLFN(void, glPixelStorei, (GLenum, GLint));
GLFN(GLuint, glCreateShader, (GLenum));
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
GLFN(void, glCompileShader, (GLuint));
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint *));
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
GLFN(GLuint, glCreateProgram, (void));
GLFN(void, glAttachShader, (GLuint, GLuint));
GLFN(void, glLinkProgram, (GLuint));
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint *));
GLFN(void, glUseProgram, (GLuint));
GLFN(GLint, glGetAttribLocation, (GLuint, const GLchar *));
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar *));
GLFN(void, glUniform1i, (GLint, GLint));
GLFN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid *));
GLFN(void, glEnableVertexAttribArray, (GLuint));
GLFN(void, glDrawArrays, (GLenum, GLint, GLsizei));
GLFN(void, glViewport, (GLint, GLint, GLsizei, GLsizei));
GLFN(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
GLFN(void, glClear, (GLbitfield));
GLFN(void, glActiveTexture, (GLenum));
GLFN(void, glBindBuffer, (GLenum, GLuint));
GLFN(void, glDisable, (GLenum));

static ptr_VidExt_Init vx_Init;
static ptr_VidExt_Quit vx_Quit;
static ptr_VidExt_SetVideoMode vx_SetVideoMode;
static ptr_VidExt_SetCaption vx_SetCaption;
static ptr_VidExt_ToggleFullScreen vx_ToggleFullScreen;
static ptr_VidExt_ResizeWindow vx_ResizeWindow;
static ptr_VidExt_GL_GetProcAddress vx_GetProc;
static ptr_VidExt_GL_SetAttribute vx_SetAttr;
static ptr_VidExt_GL_SwapBuffers vx_Swap;

static struct {
    bool active;
    GLuint tex, prog;
    GLint loc_pos, loc_uv, loc_tex;
    int tex_w, tex_h;
    bool tex_raw16;
    int win_w, win_h;
    bool filter, widescreen;
} g;

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";

static bool load_gl(void)
{
#define L(n) do { n = (void *)vx_GetProc(#n); if (!n) { foir_log(FOIR_LOG_ERROR, "missing GL function " #n); return false; } } while (0)
    L(glGenTextures); L(glDeleteTextures); L(glBindTexture); L(glTexImage2D); L(glTexSubImage2D);
    L(glTexParameteri); L(glPixelStorei); L(glCreateShader); L(glShaderSource); L(glCompileShader);
    L(glGetShaderiv); L(glGetShaderInfoLog); L(glCreateProgram); L(glAttachShader); L(glLinkProgram);
    L(glGetProgramiv); L(glUseProgram); L(glGetAttribLocation); L(glGetUniformLocation); L(glUniform1i);
    L(glVertexAttribPointer); L(glEnableVertexAttribArray); L(glDrawArrays); L(glViewport);
    L(glClearColor); L(glClear); L(glActiveTexture); L(glBindBuffer); L(glDisable);
#undef L
    return true;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        foir_log(FOIR_LOG_ERROR, "shader compile failed: %s", log);
        return 0;
    }
    return s;
}

bool gl_out_init(void *core, const struct gl_out_params *p)
{
#define V(var, type, name) var = (type)dlsym(core, name)
    V(vx_Init, ptr_VidExt_Init, "VidExt_Init");
    V(vx_Quit, ptr_VidExt_Quit, "VidExt_Quit");
    V(vx_SetVideoMode, ptr_VidExt_SetVideoMode, "VidExt_SetVideoMode");
    V(vx_SetCaption, ptr_VidExt_SetCaption, "VidExt_SetCaption");
    V(vx_ToggleFullScreen, ptr_VidExt_ToggleFullScreen, "VidExt_ToggleFullScreen");
    V(vx_ResizeWindow, ptr_VidExt_ResizeWindow, "VidExt_ResizeWindow");
    V(vx_GetProc, ptr_VidExt_GL_GetProcAddress, "VidExt_GL_GetProcAddress");
    V(vx_SetAttr, ptr_VidExt_GL_SetAttribute, "VidExt_GL_SetAttribute");
    V(vx_Swap, ptr_VidExt_GL_SwapBuffers, "VidExt_GL_SwapBuffers");
#undef V
    if (!vx_Init || !vx_SetVideoMode || !vx_GetProc || !vx_SetAttr || !vx_Swap) {
        foir_log(FOIR_LOG_ERROR, "core video extension functions not found");
        return false;
    }
    memset(&g, 0, sizeof(g));
    g.win_w = p->win_width; g.win_h = p->win_height;
    g.filter = p->filter; g.widescreen = p->widescreen;

    if (vx_Init() != M64ERR_SUCCESS) { foir_log(FOIR_LOG_ERROR, "VidExt_Init failed"); return false; }
    if (vx_SetCaption) vx_SetCaption(FOIR_NAME);
    vx_SetAttr(M64P_GL_CONTEXT_PROFILE_MASK, M64P_GL_CONTEXT_PROFILE_ES);
    vx_SetAttr(M64P_GL_CONTEXT_MAJOR_VERSION, 2);
    vx_SetAttr(M64P_GL_CONTEXT_MINOR_VERSION, 0);
    vx_SetAttr(M64P_GL_DOUBLEBUFFER, 1);
    vx_SetAttr(M64P_GL_SWAP_CONTROL, p->vsync ? 1 : 0);
    if (vx_SetVideoMode(g.win_w, g.win_h, 0, p->fullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED,
                        M64VIDEOFLAG_SUPPORT_RESIZING) != M64ERR_SUCCESS) {
        foir_log(FOIR_LOG_ERROR, "VidExt_SetVideoMode failed");
        return false;
    }
    if (!load_gl()) return false;

    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return false;
    g.prog = glCreateProgram();
    glAttachShader(g.prog, vs);
    glAttachShader(g.prog, fs);
    glLinkProgram(g.prog);
    GLint ok = 0;
    glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) { foir_log(FOIR_LOG_ERROR, "program link failed"); return false; }
    g.loc_pos = glGetAttribLocation(g.prog, "a_pos");
    g.loc_uv = glGetAttribLocation(g.prog, "a_uv");
    g.loc_tex = glGetUniformLocation(g.prog, "u_tex");

    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.active = true;
    return true;
}

void gl_out_shutdown(void)
{
    if (!g.active) return;
    if (vx_Quit) vx_Quit();
    g.active = false;
}

bool gl_out_active(void) { return g.active; }
void gl_out_set_filter(bool on) { g.filter = on; }
void gl_out_set_widescreen(bool on) { g.widescreen = on; }

void gl_out_resize(int w, int h)
{
    g.win_w = w; g.win_h = h;
}

void gl_out_toggle_fullscreen(void)
{
    if (vx_ToggleFullScreen) vx_ToggleFullScreen();
}

void gl_out_present(const void *rgba, bool raw16, int w, int h, float screen_w, float screen_h, void (*pre_swap_cb)(int))
{
    if (!g.active) return;
    int ww = g.win_w > 0 ? g.win_w : 640, wh = g.win_h > 0 ? g.win_h : 480;

    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (rgba && w > 0 && h > 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        GLenum type = raw16 ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_BYTE;
        if (w != g.tex_w || h != g.tex_h || raw16 != g.tex_raw16) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, type, rgba);
            g.tex_w = w; g.tex_h = h; g.tex_raw16 = raw16;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, type, rgba);
        }
        GLint f = g.filter ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);

        int vx = 0, vy = 0, vw = ww, vh = wh;
        if (!g.widescreen) {
            if (ww * 3 > wh * 4) { vw = wh * 4 / 3; vx = (ww - vw) / 2; }
            else { vh = ww * 3 / 4; vy = (wh - vh) / 2; }
        }
        {
            int cw = (int)(vw * screen_w), ch = (int)(vh * screen_h);
            if (cw < 1) cw = 1;
            if (ch < 1) ch = 1;
            vx += (vw - cw) / 2; vy += (vh - ch) / 2; vw = cw; vh = ch;
        }
        glViewport(vx, vy, vw, vh);
        glUseProgram(g.prog);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        static const GLfloat quad[] = {
            /* x, y, u, v; texture row 0 is the top */
            -1.f, -1.f, 0.f, 1.f,
             1.f, -1.f, 1.f, 1.f,
            -1.f,  1.f, 0.f, 0.f,
             1.f,  1.f, 1.f, 0.f,
        };
        glUniform1i(g.loc_tex, 0);
        glVertexAttribPointer((GLuint)g.loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
        glVertexAttribPointer((GLuint)g.loc_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
        glEnableVertexAttribArray((GLuint)g.loc_pos);
        glEnableVertexAttribArray((GLuint)g.loc_uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glViewport(0, 0, ww, wh);
        glUseProgram(0);
    }
    if (pre_swap_cb) pre_swap_cb(1);
    vx_Swap();
}
