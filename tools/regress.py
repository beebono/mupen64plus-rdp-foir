#!/usr/bin/env python3
"""regress.py: build synthetic RDP scenes, run them through foir-replay and the
angrylion reference tool, and compare the resulting framebuffers.

usage: regress.py [outdir] [scene ...]   (run from projects/unix after `make replay ref`)
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from rdpgen import *  # noqa

FB = 0x200000
ZB = 0x400000
TEXMEM = 0x100000


def rgba16(r, g, b, a=1):
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | a


def prologue(s, bpp=16, w=320, h=240, z=False):
    size = 2 if bpp == 16 else 3
    s.add(set_color_image(0, size, w, FB))
    if z:
        s.add(set_mask_image(ZB))
    s.add(set_scissor(0, 0, w, h))


def clear(s, color32, bpp=16, w=320, h=240):
    s.add(set_other_modes(cycle=3), set_fill_color(color32), fill_rect(0, 0, w, h), sync_pipe())


def put_texture_rgba16(s, addr, w, h, fn):
    for y in range(h):
        for x in range(w):
            r, g, b, a = fn(x, y)
            s.wr16(addr + (y * w + x) * 2, rgba16(r, g, b, a))


def checker_rgba(x, y):
    c = ((x // 4) + (y // 4)) & 1
    return (255, 64, 32, 1) if c else (32, 160, 255, 1)


def gradient_rgba(x, y):
    return ((x * 8) & 255, (y * 8) & 255, 128, 1)


def scene_fill():
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(16, 16, 48))
    for i, (c, r) in enumerate([((255, 0, 0), (20, 20, 100, 80)), ((0, 255, 0), (60.5, 50.25, 200, 140.75)),
                                 ((0, 0, 255), (150, 100, 300, 230))]):
        col = rgba16(*c)
        s.add(set_other_modes(cycle=3), set_fill_color((col << 16) | col), fill_rect(*r))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_shaded():
    s = Scene()
    prologue(s, z=True)
    clear(s, 0x00010001 * rgba16(0, 0, 0))
    # shade * 1: COMBINED = shade (a=0,b=0,c=0,d=SHADE)
    s.add(set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4))
    s.add(set_other_modes(cycle=0, aa=1, z_compare=1, z_update=1, zmode=0, color_on_cvg=0,
                          m1a0=0, m1b0=0, m2a0=1, m2b0=0, force_blend=0, cvg_dest=0, alpha_cvg=0, rgb_dither=3, alpha_dither=3,
                          m1a1=0, m1b1=0, m2a1=1, m2b1=0))
    s.add(triangle([(40, 30), (280, 60), (120, 200)], shade=[(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)],
                   zs=[20000, 20000, 20000]))
    s.add(triangle([(200, 20), (300, 200), (80, 160)], shade=[(255, 255, 0, 255), (0, 255, 255, 255), (255, 0, 255, 255)],
                   zs=[10000, 30000, 50000]))
    s.add(triangle([(10.5, 220.3), (150.7, 100.1), (310, 230)], shade=[(255, 255, 255, 255), (128, 128, 128, 255), (0, 0, 0, 255)],
                   zs=[5000, 5000, 5000]))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_tex_rect_copy():
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(40, 40, 40))
    put_texture_rgba16(s, TEXMEM, 32, 32, checker_rgba)
    put_texture_rgba16(s, TEXMEM + 0x1000, 32, 32, gradient_rgba)
    s.add(set_texture_image(0, 2, 32, TEXMEM))
    s.add(set_tile(0, 2, 0, 0, 7))
    s.add(load_block(7, 0, 0, 32 * 32 - 1, 256))
    s.add(sync_pipe())
    s.add(set_tile(0, 2, 8, 0, 0))
    s.add(set_tile_size(0, 0, 0, 31, 31))
    s.add(set_other_modes(cycle=2, alpha_compare=0))
    s.add(tex_rect(0, 20, 20, 52, 52, 0, 0, 1.0, 1.0))
    # tile 1 gets the gradient
    s.add(set_texture_image(0, 2, 32, TEXMEM + 0x1000))
    s.add(set_tile(0, 2, 0, 0x40, 7))
    s.add(load_block(7, 0, 0, 32 * 32 - 1, 256))
    s.add(sync_pipe())
    s.add(set_tile(0, 2, 8, 0x40, 1))
    s.add(set_tile_size(1, 0, 0, 31, 31))
    s.add(tex_rect(1, 100, 100, 132, 132, 0, 0, 1.0, 1.0))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_tex_1cycle():
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(0, 0, 64))
    put_texture_rgba16(s, TEXMEM, 32, 32, gradient_rgba)
    s.add(set_texture_image(0, 2, 32, TEXMEM))
    s.add(set_tile(0, 2, 0, 0, 7))
    s.add(load_block(7, 0, 0, 32 * 32 - 1, 256))
    s.add(sync_pipe())
    s.add(set_tile(0, 2, 8, 0, 0, cs=0, ms=0))
    s.add(set_tile_size(0, 0, 0, 31, 31))
    # TEXEL0 * SHADE  -> (T0 - 0) * SHADE + 0
    s.add(set_combine(1, 8, 4, 7, 1, 7, 4, 7, 1, 8, 4, 7, 1, 7, 4, 7))
    s.add(set_other_modes(cycle=0, bilerp_sample=1, bi0=1, bi1=1, aa=1, m1a0=0, m1b0=0, m2a0=1, m2b0=0,
                          m1a1=0, m1b1=0, m2a1=1, m2b1=0, rgb_dither=3, alpha_dither=3))
    s.add(tex_rect(0, 16, 16, 112, 112, 0, 0, 1 / 3.0, 1 / 3.0))
    s.add(triangle([(150, 40), (300, 80), (200, 220)], shade=[(255, 255, 255, 255), (200, 200, 255, 255), (255, 200, 200, 255)],
                   tex=[(0, 0, 1), (31, 0, 1), (0, 31, 1)]))
    # point sampled
    s.add(set_other_modes(cycle=0, bilerp_sample=0, bi0=1, bi1=1, aa=1, m1a0=0, m1b0=0, m2a0=1, m2b0=0,
                          m1a1=0, m1b1=0, m2a1=1, m2b1=0, rgb_dither=3, alpha_dither=3))
    s.add(tex_rect(0, 16, 130, 80, 194, 0, 0, 0.5, 0.5))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_blend_alpha():
    s = Scene()
    prologue(s, z=False)
    clear(s, 0x00010001 * rgba16(200, 200, 200))
    s.add(set_prim_color(0xFF000080))
    s.add(set_combine(0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 3))   # PRIM
    # standard alpha blend: pixel * a + mem * (1-a); needs image_read
    s.add(set_other_modes(cycle=0, aa=1, image_read=1, force_blend=1, m1a0=0, m1b0=0, m2a0=1, m2b0=0, cvg_dest=1,
                          m1a1=0, m1b1=0, m2a1=1, m2b1=0, rgb_dither=3, alpha_dither=3))
    s.add(triangle([(60, 40), (260, 100), (100, 210)], shade=[(0, 0, 0, 0)] * 3))
    s.add(set_prim_color(0x0000FF40))
    s.add(triangle([(200, 30), (300, 150), (150, 150)], shade=[(0, 0, 0, 0)] * 3))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_two_cycle_fog():
    s = Scene()
    prologue(s, z=False)
    clear(s, 0x00010001 * rgba16(0, 80, 0))
    s.add(set_fog_color(0xC0C0FF00))
    s.add(set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0))   # cycle0: SHADE, cycle1: COMBINED (d=0 -> combined)
    s.add(set_other_modes(cycle=1, aa=1, m1a0=0, m1b0=2, m2a0=3, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0, rgb_dither=3, alpha_dither=3))
    s.add(triangle([(30, 30), (290, 60), (140, 220)], shade=[(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 0)]))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_32bit():
    s = Scene()
    prologue(s, bpp=32)
    s.add(set_color_image(0, 3, 320, FB))
    s.add(set_other_modes(cycle=3), set_fill_color(0x203040FF), fill_rect(0, 0, 320, 240))
    s.add(set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4))
    s.add(set_other_modes(cycle=0, aa=1, m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0, rgb_dither=3, alpha_dither=3))
    s.add(triangle([(40, 30), (280, 60), (120, 200)], shade=[(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)]))
    s.add(sync_full())
    return s, vi_320x240(FB, True)


def scene_dither_noaa():
    s = Scene()
    prologue(s)
    clear(s, 0)
    s.add(set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4))
    s.add(set_other_modes(cycle=0, aa=0, m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0, rgb_dither=0, alpha_dither=1))
    s.add(triangle([(0, 0), (320, 0), (0, 240)], shade=[(255, 255, 255, 255), (0, 64, 0, 255), (0, 0, 128, 255)]))
    s.add(triangle([(320, 0), (320, 240), (0, 240)], shade=[(0, 64, 0, 255), (255, 255, 0, 255), (0, 0, 128, 255)]))
    s.add(sync_full())
    return s, vi_320x240(FB)


# ---- texture format coverage -------------------------------------------------------------

def tex_pixel(x, y):
    return ((x * 9 + y * 3) & 255, (y * 13) & 255, ((x ^ y) * 7) & 255, 255 if (x + y) % 5 else 0)


def put_texture(s, fmt, size, w, h, addr):
    """write a test pattern in the given N64 format; returns bytes written"""
    off = addr
    for y in range(h):
        for x in range(w):
            r, g, b, a = tex_pixel(x, y)
            i = (r + g + b) // 3
            if size == 3:
                s.wr32(off, (r << 24) | (g << 16) | (b << 8) | a); off += 4
            elif size == 2:
                if fmt == 0: s.wr16(off, rgba16(r, g, b, 1 if a else 0))
                elif fmt == 3: s.wr16(off, (i << 8) | a)
                else: s.wr16(off, (x * 16 + y) & 0xFFFF)
                off += 2
            elif size == 1:
                if fmt == 3: s.wr8(off, (i & 0xF0) | (a >> 4))
                else: s.wr8(off, (x * 5 + y * 3) & 255)
                off += 1
            else:
                if x & 1 == 0:
                    n0 = ((i >> 4) & 0xE) | (1 if a else 0) if fmt == 3 else (x + y) & 15
                    n1 = None
                else:
                    pass
        if size == 0:
            for x in range(0, w, 2):
                def nib(xx):
                    r, g, b, a = tex_pixel(xx, y)
                    i = (r + g + b) // 3
                    return (((i >> 4) & 0xE) | (1 if a else 0)) if fmt == 3 else (((i >> 4)) if fmt == 4 else ((xx + y) & 15))
                s.wr8(off, (nib(x) << 4) | nib(x + 1)); off += 1
    return off - addr


def upload_tex(s, fmt, size, w, h, addr, tmem, tile=0, loadtile=7, **kw):
    put_texture(s, fmt, size, w, h, addr)
    bits = {0: 4, 1: 8, 2: 16, 3: 32}[size]
    qwords_row = max(1, (w * bits + 63) // 64)
    lsize = 2 if size == 0 else size
    ltexels = {0: 4, 1: 8, 2: 4, 3: 2}[size]  # texels per qword
    n = (w * h + ltexels - 1) // ltexels
    if size == 3:
        n_texels_load = w * h
        dxt = (2048 + qwords_row - 1) // qwords_row
        line = max(1, w // 4)
    else:
        n_texels_load = n * (4 if lsize == 2 else 8)
        dxt = (2048 + qwords_row - 1) // qwords_row
        line = qwords_row
    s.add(set_texture_image(fmt if size != 0 else 0, lsize, 1, addr))
    s.add(set_tile(fmt if size != 0 else 0, lsize, 0, tmem, loadtile))
    s.add(load_block(loadtile, 0, 0, n_texels_load - 1, dxt))
    s.add(sync_pipe())
    s.add(set_tile(fmt, size, line, tmem, tile, **kw))
    s.add(set_tile_size(tile, 0, 0, w - 1, h - 1))


def scene_tex_formats():
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(30, 0, 30))
    s.add(set_combine(1, 8, 4, 7, 1, 7, 4, 7, 1, 8, 4, 7, 1, 7, 4, 7))  # T0 * shade (with shade = 255 via prim?)
    s.add(set_combine(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1))  # COMBINED = TEXEL0
    om = dict(cycle=0, bilerp_sample=1, bi0=1, bi1=1, aa=0, m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0)
    cases = [(0, 2, 32, 32), (0, 3, 16, 16), (3, 2, 16, 16), (3, 1, 16, 16), (3, 0, 16, 16),
             (4, 1, 16, 16), (4, 0, 16, 16), (2, 1, 16, 16), (2, 0, 16, 16)]
    x = 4
    y = 4
    for i, (fmt, size, w, h) in enumerate(cases):
        addr = TEXMEM + i * 0x2000
        kw = {}
        if fmt == 2:
            # palette
            pal = 0x180000
            for k in range(256):
                s.wr16(pal + 2 * k, rgba16((k * 3) & 255, (k * 7) & 255, (255 - k) & 255, 1))
            s.add(set_texture_image(0, 2, 1, pal), set_tile(0, 0, 0, 0x100, 5), load_tlut(5, 0, 0, 255, 0), sync_pipe())
            kw = {'pal': 0}
            s.add(set_other_modes(tlut=1, bilerp_sample=1, **{k: v for k, v in om.items() if k != 'bilerp_sample'}))
        else:
            s.add(set_other_modes(**om))
        upload_tex(s, fmt, size, w, h, addr, 0, **kw)
        s.add(tex_rect(0, x, y, x + w * 2, y + h * 2, 0, 0, 0.5, 0.5))
        x += w * 2 + 6
        if x > 270:
            x = 4
            y += 80
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_tex_wrap():
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(0, 0, 0))
    s.add(set_combine(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1))
    om = dict(cycle=0, bilerp_sample=1, bi0=1, bi1=1, aa=0, m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0)
    s.add(set_other_modes(**om))
    x, y = 4, 4
    for i, (cs, ms, ct, mt, masks, maskt) in enumerate([(0, 0, 0, 0, 4, 4), (0, 1, 0, 1, 4, 4), (1, 0, 1, 0, 0, 0), (0, 0, 0, 0, 0, 0)]):
        upload_tex(s, 0, 2, 16, 16, TEXMEM, 0, cs=cs, ms=ms, ct=ct, mt=mt, masks=masks, maskt=maskt)
        s.add(tex_rect(0, x, y, x + 96, y + 96, -8, -8, 0.25, 0.25))
        x += 100
        if x > 220:
            x = 4; y += 100
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_persp_lod(one_cycle=False):
    s = Scene()
    prologue(s, z=True)
    clear(s, 0x00010001 * rgba16(10, 10, 40))
    s.add(set_other_modes(cycle=3), set_fill_color(0xFFFCFFFC), set_color_image(0, 2, 320, ZB), fill_rect(0, 0, 320, 240),
          set_color_image(0, 2, 320, FB), sync_pipe())
    # mip chain: level0 32x32, level1 16x16, level2 8x8 at different tmem offsets
    sizes = [(32, 0x000), (16, 0x100), (8, 0x140)]
    for lvl, (w, tm) in enumerate(sizes):
        put_texture(s, 0, 2, w, w, TEXMEM + lvl * 0x2000)
        s.add(set_texture_image(0, 2, 1, TEXMEM + lvl * 0x2000), set_tile(0, 2, 0, tm, 7),
              load_block(7, 0, 0, w * w - 1, max(1, 2048 // (w // 4))), sync_pipe(),
              set_tile(0, 2, w // 4, tm, lvl), set_tile_size(lvl, 0, 0, w - 1, w - 1))
    s.add(set_combine(1, 2, 14, 1, 1, 2, 6, 1, 0, 0, 0, 0, 0, 0, 0, 0))   # lerp T0/T1 by lod frac? fields arbitrary
    cyc = 0 if one_cycle else 1
    s.add(set_combine(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 if one_cycle else 0, 0, 0, 0, 1 if one_cycle else 0))
    s.add(set_other_modes(cycle=cyc, persp=1, lod=1, bilerp_sample=1, bi0=1, bi1=1, aa=1, z_compare=1, z_update=1,
                          m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0))
    # ground-plane-like quad: near edge larger s,t gradient
    s.add(triangle([(20, 230), (300, 230), (150, 40)], shade=[(255, 255, 255, 255)] * 3,
                   tex=[(0, 0, 1.0), (32, 0, 1.0), (0, 32, 0.15)], zs=[10000, 10000, 40000]))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_persp_1cycle():
    return scene_persp_lod(one_cycle=True)


def scene_zmodes():
    s = Scene()
    prologue(s, z=True)
    clear(s, 0x00010001 * rgba16(0, 0, 0))
    s.add(set_other_modes(cycle=3), set_fill_color(0xFFFCFFFC), set_color_image(0, 2, 320, ZB), fill_rect(0, 0, 320, 240),
          set_color_image(0, 2, 320, FB), sync_pipe())
    s.add(set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4))
    tri_shade = lambda c: [c] * 3
    for zmode, ox in enumerate([0, 80, 160, 240]):
        s.add(set_other_modes(cycle=0, aa=1, z_compare=1, z_update=1, zmode=zmode, m1a0=0, m1b0=0, m2a0=1, m2b0=0,
                              m1a1=0, m1b1=0, m2a1=1, m2b1=0, force_blend=0))
        s.add(triangle([(ox + 10, 20), (ox + 70, 60), (ox + 10, 110)], shade=tri_shade((255, 0, 0, 255)), zs=[30000, 30000, 30000]))
        s.add(triangle([(ox + 5, 40), (ox + 75, 40), (ox + 40, 130)], shade=tri_shade((0, 255, 0, 255)), zs=[20000, 45000, 20000]))
        s.add(triangle([(ox + 20, 90), (ox + 70, 100), (ox + 30, 180)], shade=tri_shade((0, 0, 255, 255)), zs=[31000, 29000, 30000]))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_alphacmp_cvg():
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(80, 80, 80))
    s.add(set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4))
    s.add(set_blend_color(0x00000080))
    s.add(set_other_modes(cycle=0, aa=1, alpha_compare=1, image_read=1, cvg_dest=0, color_on_cvg=0, force_blend=0,
                          m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0))
    s.add(triangle([(40, 20), (280, 100), (60, 220)], shade=[(255, 0, 0, 255), (0, 255, 0, 40), (0, 0, 255, 255)]))
    s.add(set_other_modes(cycle=0, aa=1, cvg_times_alpha=1, alpha_cvg=1, image_read=1, cvg_dest=1, force_blend=1,
                          m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0))
    s.add(triangle([(200, 10), (310, 120), (140, 80)], shade=[(255, 255, 0, 255), (0, 255, 255, 255), (255, 0, 255, 128)]))
    s.add(sync_full())
    return s, vi_320x240(FB)


def scene_copy_tlut():
    """COPY-cycle texture rectangles of CI8 / CI4 textures through a TLUT (what Paper Mario, Mischief Makers, Hydro Thunder
    draw), RGBA16 textures, with and without alpha compare, with texture masks and at odd / even screen positions"""
    s = Scene()
    prologue(s)
    clear(s, 0x00010001 * rgba16(40, 40, 40))
    pal = 0x180000
    for k in range(256):
        s.wr16(pal + 2 * k, rgba16((k * 3) & 255, (k * 7) & 255, (255 - k) & 255, 1 if (k % 5) else 0))
    s.add(set_texture_image(0, 2, 1, pal), set_tile(0, 0, 0, 0x100, 5), load_tlut(5, 0, 0, 255, 0), sync_pipe())
    x, y = 3, 3
    for i, (fmt, size, w, h, kw, ac) in enumerate([(2, 1, 32, 32, {}, 0), (2, 1, 32, 32, {}, 1), (2, 0, 32, 32, {'pal': 3}, 1),
                                                   (2, 1, 32, 32, {'masks': 4, 'maskt': 4}, 1), (0, 2, 32, 32, {}, 1)]):
        s.add(set_other_modes(cycle=2, tlut=int(fmt == 2), alpha_compare=ac))
        upload_tex(s, fmt, size, w, h, TEXMEM + i * 0x2000, 0, **kw)
        s.add(tex_rect(0, x, y, x + 61, y + 31, 0, 0, 4.0, 1.0))
        x += 63
        if x > 250:
            x, y = 4, y + 40
    s.add(sync_full())
    return s, vi_320x240(FB)


SCENES = {
    'fill': scene_fill,
    'shaded': scene_shaded,
    'copy': scene_tex_rect_copy,
    'tex1cycle': scene_tex_1cycle,
    'blend': scene_blend_alpha,
    'twocycle': scene_two_cycle_fog,
    'fb32': scene_32bit,
    'dither': scene_dither_noaa,
    'texfmt': scene_tex_formats,
    'texwrap': scene_tex_wrap,
    'persp': scene_persp_lod,
    'persp1': scene_persp_1cycle,
    'zmodes': scene_zmodes,
    'alphacvg': scene_alphacmp_cvg,
    'copytlut': scene_copy_tlut,
}


# FILL/COPY primitives must be counted in foir-replay's "N pixels" statistic (Hydro Thunder draws only
# COPY-mode texrects and fills and used to report "0 pixels"); lower bound = full clear + the sprites.
MIN_PIXELS = {'fill': 76800 + 80 * 60, 'copy': 76800 + 2 * 32 * 32}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '../../test-output/synthetic'
    names = sys.argv[2:] or list(SCENES)
    os.makedirs(out, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    for name in names:
        s, vi = SCENES[name]()
        dump = os.path.join(out, name + '.fdump')
        s.write(dump, vi)
        prefix = os.path.join(out, name)
        r1 = subprocess.run(['./foir-replay', dump, '-o', prefix + '_foir.rdram', '-p', prefix + '_foir_vi.ppm'],
                            capture_output=True, text=True)
        r2 = subprocess.run(['./ref-angrylion', dump, '-o', prefix + '_angry.rdram'], capture_output=True, text=True)
        print('== %s' % name)
        if r1.returncode:
            print('foir-replay failed:', r1.stderr)
            continue
        if r2.returncode:
            print('angrylion failed:', r2.stderr)
            continue
        print(r1.stderr.strip())
        if name in MIN_PIXELS:
            m = re.search(r'(\d+) pixels', r1.stderr)
            npix = int(m.group(1)) if m else 0
            if npix < MIN_PIXELS[name]:
                print('FAIL: %s drew %d counted pixels, expected >= %d' % (name, npix, MIN_PIXELS[name]))
                sys.exit(1)
        subprocess.run([sys.executable, os.path.join(here, 'compare.py'), dump, prefix + '_foir.rdram',
                        prefix + '_angry.rdram', prefix])
        for ext in ('_foir.rdram', '_angry.rdram'):
            os.remove(prefix + ext) if os.environ.get('KEEP_RDRAM') is None else None
        os.remove(dump) if os.environ.get('KEEP_DUMPS') is None else None


if __name__ == '__main__':
    main()
