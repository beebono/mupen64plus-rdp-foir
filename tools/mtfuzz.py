#!/usr/bin/env python3
"""mtfuzz.py: random scenes stressing the worker pool's row ownership and barriers; each is replayed with the scalar kernels (reference) and the default
kernels on 1..N worker threads, and the final RDRAM images must be byte-identical.

--stress: long, order-sensitive scenes (many small jobs, overlapping blended / Z-tested / filled draws, frequent state changes, syncs, render-target switches).
--lag: unbalanced stripe classes (a few stripes get heavy work while every iteration changes TMEM and the primitive colour): a lagging class keeps ~3000
snapshots alive, so a pool floor that moves past it lets the parser overwrite one the class still needs.

usage: mtfuzz.py [-n scenes] [-s seed] [--replay path] [--runner "qemu-aarch64-static -L ..."] [--threads 1,2,3,4]
                 [--stress] [--lag] [--variants default,nohelper,g1,g3,g4]
"""
import argparse
import hashlib
import os
import random
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from rdpgen import *  # noqa
import regress  # noqa

def load_variants():
    """combiner / blender selector sets of the specialised variants: scenes take their modes from here half of the time"""
    import re
    comb, blend = [], []
    for fn, pool, pat in (('spec_comb.def', comb, r'COMBX?\((\d+), (0x[0-9a-f]+), (0x[0-9a-f]+)'),
                          ('spec_blend.def', blend, r'BLENDX?\((\d+), (\d+),(\d+),(\d+),(\d+), (\d+),(\d+),(\d+),(\d+)')):
        p = os.path.join(ROOT, 'src', fn)
        if os.path.exists(p):
            for line in open(p):
                m = re.match(r'\s*' + pat, line)
                if m:
                    pool.append([int(g, 0) for g in m.groups()])
    return comb, blend


VAR_COMB, VAR_BLEND = load_variants()


def load_shapes():
    out = []
    p = os.path.join(ROOT, 'src', 'spec_shape.def')
    if os.path.exists(p):
        for line in open(p):
            m = re.match(r'\s*SHAPE\((\d), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (\d)', line)
            if m:
                out.append([int(g, 0) for g in m.groups()])
    return out


VAR_SHAPE = load_shapes()


def shape_sels(rng, sh, half, combact):
    import shapegen as sg
    nsel = [8, 8, 8, 8] if half else [16, 16, 32, 8]
    cls = sg.al_reg if half else sg.rgb_reg
    let = lambda p, sel: sg.letter(cls(p, sel), combact)
    want = [(sh >> (2 * p)) & 3 for p in range(4)]
    pick = lambda p, l: rng.choice([x for x in range(nsel[p]) if let(p, x) == l] or [0])
    anyp = lambda p: rng.randrange(nsel[p])
    if want[0] == want[1] == want[2] == 0:           # D only: multiply zero, or A and B the same constant
        pairs = [l for l in (0, 1, 2) if any(let(0, x) == l for x in range(nsel[0])) and any(let(1, x) == l for x in range(nsel[1]))]
        if pairs and rng.randrange(2):
            l = rng.choice(pairs)
            return [pick(0, l), pick(1, l), anyp(2), pick(3, want[3])]
        return [anyp(0), anyp(1), pick(2, 0), pick(3, want[3])]
    return [pick(p, want[p]) for p in range(4)]


def shape_combine(rng, cycle=None):
    pool = [v for v in VAR_SHAPE if cycle is None or v[0] == (1 if cycle else 0)]
    v = rng.choice(pool)
    two = v[0]
    c = []
    for cy in (0, 1):
        if cy == 0 and not two:
            r = [rng.randrange(16), rng.randrange(16), rng.randrange(32), rng.randrange(8)]
            a = [rng.randrange(8) for _ in range(4)]
        else:
            r = shape_sels(rng, v[1 + 2 * cy], 0, bool(two) and cy == 1)
            a = shape_sels(rng, v[2 + 2 * cy], 1, bool(two) and cy == 1)
        c.append((r, a))
    (r0, a0), (r1, a1) = c
    return set_combine(r0[0], r0[1], r0[2], r0[3], a0[0], a0[1], a0[2], a0[3], r1[0], r1[1], r1[2], r1[3], a1[0], a1[1], a1[2], a1[3])
BUF = [0x200000, 0x300000, 0x500000]
ZB = 0x400000
TEX = 0x180000
PAL = 0x1a0000          # palette of the COPY-cycle rectangles
TEXC = 0x1b0000


def rgba16(rng):
    return (rng.randrange(32) << 11) | (rng.randrange(32) << 6) | (rng.randrange(32) << 1) | rng.randrange(2)


def rnd_modes(rng, cycle):
    k = dict(cycle=cycle, persp=rng.randrange(2), bilerp_sample=rng.randrange(2), bi0=0 if rng.randrange(10) == 0 else 1,
             bi1=0 if rng.randrange(10) == 0 else 1, sharpen=rng.randrange(6) == 0,
             lod=rng.randrange(3) == 0, detail=rng.randrange(6) == 0,       # TEX_LOD_EN / detail: tiles 0 .. 3 form a mip chain
             mid_texel=rng.randrange(4) == 0,
             rgb_dither=rng.choice([0, 1, 2, 3, 3]), alpha_dither=rng.choice([0, 1, 2, 3, 3]),
             aa=rng.randrange(2), z_compare=rng.randrange(2), z_update=rng.randrange(2), zmode=rng.randrange(4),
             force_blend=rng.randrange(2), cvg_dest=rng.randrange(4), color_on_cvg=rng.randrange(2),
             image_read=rng.randrange(2), alpha_cvg=rng.randrange(2), cvg_times_alpha=rng.randrange(2),
             alpha_compare=rng.randrange(2), dither_alpha_en=rng.randrange(4) == 0)
    for n in ('m1a0', 'm1b0', 'm2a0', 'm2b0', 'm1a1', 'm1b1', 'm2a1', 'm2b1'):
        k[n] = rng.randrange(4)
    pool = [v for v in VAR_BLEND if v[0] == (1 if cycle else 0)]
    if pool and rng.randrange(2):
        v = rng.choice(pool)
        for n, x in zip(('m1a0', 'm1b0', 'm2a0', 'm2b0', 'm1a1', 'm1b1', 'm2a1', 'm2b1'), v[1:]):
            k[n] = x
    return k


def rnd_combine(rng, cycle=None):
    pool = [v for v in VAR_COMB if cycle is None or v[0] == (1 if cycle else 0)]
    if VAR_SHAPE and rng.randrange(3) == 0:
        return shape_combine(rng, cycle)
    if pool and rng.randrange(2):
        v = rng.choice(pool)
        return w((0x3C << 24) | v[1], v[2])
    f = lambda hi: rng.randrange(hi)
    return set_combine(f(8), f(8), f(16), f(8), f(8), f(8), f(8), f(8),
                       f(8), f(8), f(16), f(8), f(8), f(8), f(8), f(8))


def rnd_tri(rng, w, h, textured, z, level=0):
    pts = [(rng.uniform(-10, w + 10), rng.uniform(-10, h + 10)) for _ in range(3)]
    shade = [tuple(rng.randrange(256) for _ in range(4)) for _ in range(3)]
    tex = None
    if textured:
        tex = [(rng.uniform(-8, 40), rng.uniform(-8, 40), rng.uniform(0.2, 1.0)) for _ in range(3)]
    zs = [rng.randrange(1000, 60000) for _ in range(3)] if z else None
    try:
        return triangle(pts, shade=shade, tex=tex, zs=zs, level=level)
    except Exception:
        return None


def small_tri(rng, w, h, textured, z, level=0):
    """a triangle inside a box of a few rows (one or two stripes, so a job has a single stripe class)"""
    y0 = rng.uniform(-2, h)
    x0 = rng.uniform(-10, w)
    bw, bh = rng.uniform(2, max(3, w / 2)), rng.choice([1.5, 3.0, 5.0, 9.0])
    pts = [(x0 + rng.uniform(0, bw), y0 + rng.uniform(0, bh)) for _ in range(3)]
    shade = [tuple(rng.randrange(256) for _ in range(4)) for _ in range(3)]
    tex = [(rng.uniform(-8, 40), rng.uniform(-8, 40), rng.uniform(0.2, 1.0)) for _ in range(3)] if textured else None
    zs = [rng.randrange(1000, 60000) for _ in range(3)] if z else None
    try:
        return triangle(pts, shade=shade, tex=tex, zs=zs, level=level)
    except Exception:
        return None


def make_scene(rng, stress=False):
    s = Scene()
    for i in range(32 * 32):
        s.wr16(TEX + i * 2, rgba16(rng))
    widths = [17, 33, 64, 100, 128, 129, 200, 256, 320, 321]
    bpp32 = rng.randrange(5) == 0
    W = rng.choice(widths)
    H = rng.randrange(24, 90) if not stress else rng.randrange(40, 200)
    size = 3 if bpp32 else 2
    s.add(set_color_image(0, size, W, BUF[0]), set_mask_image(ZB), set_scissor(0, 0, W, H))
    s.add(set_other_modes(cycle=3), set_fill_color(rng.randrange(1 << 32)), fill_rect(0, 0, W - 1, H - 1), sync_pipe())
    s.add(set_other_modes(cycle=3), set_color_image(0, 2, W, ZB), set_fill_color(0xfffcfffc),
          fill_rect(0, 0, W - 1, H - 1), sync_pipe(), set_color_image(0, size, W, BUF[0]))
    s.add(set_texture_image(0, 2, 32, TEX), set_tile(0, 2, 0, 0, 7), load_block(7, 0, 0, 1023, 0), sync_pipe(),
          set_tile(0, 2, 8, 0, 0, maskt=rng.choice([0, 5]), masks=rng.choice([0, 5])), set_tile_size(0, 0, 0, 31, 31))
    for lvl, (wd, tm) in enumerate([(16, 256), (8, 320), (4, 336)], 1):
        s.add(set_texture_image(0, 2, 32, TEX), set_tile(0, 2, 0, tm, 7), load_block(7, 0, 0, wd * wd - 1, 0), sync_pipe(),
              set_tile(0, 2, max(1, wd // 4), tm, lvl, maskt=rng.choice([0, wd.bit_length() - 1]), masks=rng.choice([0, wd.bit_length() - 1]),
                       ms=rng.randrange(2), mt=rng.randrange(2), cs=rng.randrange(4) == 0), set_tile_size(lvl, 0, 0, wd - 1, wd - 1))
    cur = 0
    curW = W
    for step in range(rng.randrange(12, 40) if not stress else rng.randrange(150, 450)):
        op = rng.randrange(12)
        if stress and op == 8 and rng.randrange(3):
            op = 4                  # fewer full syncs than draws
        if stress and op in (9, 11):
            op = 5                  # more fills: order-dependent overwrites
        if op <= 4:   # triangles
            cyc = rng.randrange(2)
            textured = rng.randrange(3) != 0
            s.add(rnd_combine(rng, cyc), set_prim_color(rng.randrange(1 << 32)), set_env_color(rng.randrange(1 << 32)),
                  set_other_modes(**rnd_modes(rng, cyc)))
            for _ in range(rng.randrange(1, 4)):
                t = small_tri(rng, curW, H, textured, rng.randrange(2), level=rng.randrange(4)) if stress and rng.randrange(3) else \
                    rnd_tri(rng, curW, H, textured, rng.randrange(2), level=rng.randrange(4))
                if t:
                    s.add(t)
        elif op == 5:  # fill
            x0, y0 = rng.randrange(curW), rng.randrange(H)
            hh = rng.randrange(1, H) if not (stress and rng.randrange(2)) else rng.randrange(0, 5)
            s.add(set_other_modes(cycle=3), set_fill_color(rng.randrange(1 << 32)),
                  fill_rect(x0, y0, min(curW - 1, x0 + rng.randrange(1, curW)), min(H - 1, y0 + hh)))
        elif op == 6:  # switch render target, possibly overlapping with a different width
            nb = rng.randrange(3)
            nw = rng.choice(widths)
            base = BUF[nb] + rng.choice([0, 0, 2 * 64, 4 * 1024])
            s.add(sync_pipe(), set_color_image(0, size, nw, base), set_scissor(0, 0, nw, H))
            cur, curW = nb, nw
        elif op == 7:  # texture straight from a framebuffer region
            nb = rng.randrange(3)
            s.add(sync_pipe(), set_texture_image(0, 2, rng.choice(widths), BUF[nb]), set_tile(0, 2, 0, 0, 7),
                  load_block(7, 0, 0, rng.choice([255, 511, 1023]), 0), sync_pipe(),
                  set_tile(0, 2, 8, 0, 0, maskt=0, masks=0), set_tile_size(0, 0, 0, 31, 31))
        elif op >= 10:  # COPY-cycle texture rectangle: RGBA16, CI8 / CI4 with TLUT, 16-bit CI / IA with TLUT, I4 / IA8 without
            fmt, size = rng.choice([(0, 2), (0, 2), (2, 1), (2, 1), (2, 0), (2, 0), (3, 2), (2, 2), (4, 0), (3, 1), (0, 3)])
            tlut = fmt == 2
            kw = dict(masks=rng.choice([0, 0, 4, 5]), maskt=rng.choice([0, 0, 4, 5]), cs=rng.randrange(2), ms=rng.randrange(2),
                      shifts=rng.choice([0, 0, 0, 1, 12]), shiftt=rng.choice([0, 0, 0, 2, 11]))
            if tlut:
                for k in range(256):
                    s.wr16(PAL + 2 * k, rgba16(rng))
                s.add(sync_pipe(), set_texture_image(0, 2, 1, PAL), set_tile(0, 0, 0, 0x100, 5), load_tlut(5, 0, 0, 255, 0), sync_pipe())
                kw['pal'] = rng.randrange(16) if size == 0 else 0
            s.add(sync_pipe())
            regress.upload_tex(s, fmt, size, 32, 32, TEXC, rng.choice([0, 0, 0x40]), **kw)
            s.add(set_other_modes(cycle=2, tlut=int(tlut), tlut_ia=rng.randrange(2) if tlut else 0, persp=rng.randrange(2),
                                  alpha_compare=rng.randrange(2)))
            x0 = rng.randrange(-6, curW)
            x1 = min(curW + 3, x0 + rng.choice([rng.randrange(1, 12), rng.randrange(1, 140), curW]))
            y0 = rng.randrange(-4, H)
            y1 = min(H + 3, y0 + rng.randrange(1, 24))
            s.add(tex_rect(0, x0, y0, x1, y1, rng.randrange(-4, 30), rng.randrange(-4, 30), rng.choice([4.0, 4.0, 4.0, 2.0, 8.0, 3.5]),
                           rng.choice([1.0, 1.0, 0.5, 2.0, 0.0]), flip=rng.randrange(8) == 0))
        elif op == 8:  # sync (full)
            s.add(sync_full())
        else:          # re-set the scissor
            s.add(set_scissor(rng.randrange(0, 8), rng.randrange(0, 8), curW - rng.randrange(0, 8), H - rng.randrange(0, 8)))
    s.add(sync_full())
    return s, (3 if bpp32 else 2, BUF[0], W, 0x006C02EC, 0x002E020E, 0x200, 0x400)


VARIANTS = {
    'default': {},
    'nohelper': {'FOIR_HELPER': '0'},
    'g1': {'FOIR_MT_CLASSES': '1'},
    'g3': {'FOIR_MT_CLASSES': '3'},
    'g4': {'FOIR_MT_CLASSES': '4'},
}


def make_lag_scene(rng):
    s = Scene()
    W, H = 320, 64
    NT = 48
    for i in range(NT * 32 * 32):
        s.wr16(TEX + i * 2, rgba16(rng))
    s.add(set_color_image(0, 2, W, BUF[0]), set_mask_image(ZB), set_scissor(0, 0, W, H))
    s.add(set_other_modes(cycle=3), set_fill_color(rng.randrange(1 << 32)), fill_rect(0, 0, W - 1, H - 1), sync_pipe())
    s.add(set_color_image(0, 2, W, ZB), set_fill_color(0xfffcfffc), fill_rect(0, 0, W - 1, H - 1), sync_pipe(), set_color_image(0, 2, W, BUF[0]))
    # one cycle, TEXEL0 * PRIM, Z-buffered: a pixel keeps the colour of the nearest triangle, so a job rendered with the wrong snapshot shows in the final image
    s.add(set_other_modes(cycle=0, z_compare=1, z_update=1, zmode=0), set_combine(1, 8, 3, 7, 1, 7, 7, 1, 1, 8, 3, 7, 1, 7, 7, 1))
    s.add(set_tile(0, 2, 0, 0, 7), set_tile(0, 2, 8, 0, 0), set_tile_size(0, 0, 0, 31, 31))
    heavy = rng.sample(range(H // 4), rng.choice([1, 2, 3, 4]))
    ring = rng.randrange(3) == 0       # no state / TMEM change between the draws: the job ring runs out, not the snapshots
    n = rng.randrange(3800, 4600) if ring else rng.randrange(2800, 3500)
    cur = 0

    def tri(y0, x0, x1, hh):
        pts = [(x0 + rng.uniform(0, 4), y0 + rng.uniform(0, 0.5)), (x1 - rng.uniform(0, 4), y0 + rng.uniform(0, hh)), (x0 + rng.uniform(0, 4), y0 + hh)]
        z = rng.randrange(1000, 60000)
        t = triangle(pts, tex=[(rng.uniform(0, 31), rng.uniform(0, 31), 1.0) for _ in range(3)], zs=[z, z, z])
        return t

    def load():
        return [set_texture_image(0, 2, 32, TEX + rng.randrange(NT) * 0x800), load_block(7, 0, 0, 1023, 0), sync_pipe(), set_prim_color(rng.randrange(1 << 32))]

    if ring:
        s.add(*load())
    for i in range(n):
        if i % (n // 8 + 1) == 0:        # another set of stripes gets the work (so every class lags in some phase)
            heavy = rng.sample(range(H // 4), rng.choice([1, 2, 3]))
        if not ring:
            s.add(*load())
        hs = rng.choice(heavy)
        x0 = rng.randrange(0, 20)
        s.add(tri(4 * hs, x0, x0 + rng.randrange(240, 300), 3.9))
        for _ in range(rng.randrange(1, 3)):
            ls = rng.randrange(H // 4)
            lx = rng.randrange(0, W - 40)
            if not ring:
                s.add(*load())
            s.add(tri(4 * ls, lx, lx + rng.randrange(8, 40), rng.choice([1.0, 2.0, 3.9])))
        r = rng.randrange(120) if not ring else (0 if rng.randrange(400) == 0 else 99)       # (the ring scenes have no barrier before the end)
        if r == 0:           # a tall triangle: every class
            s.add(tri(0, 0, W - 1, H - 1))
        elif r in (1, 2):    # the frame buffer as a texture, read from a lagging stripe's rows: the parser must wait for the classes (hazard barrier)
            if r == 2:
                s.add(sync_full())
            s.add(sync_pipe(), set_texture_image(0, 2, W, BUF[cur] + 4 * W * 2 * rng.choice(heavy)), load_block(7, 0, 0, 1023, 0), sync_pipe())
            if ring:         # drawn back where it will be overwritten anyway
                s.add(tri(8, 8, 60, 3.9), set_texture_image(0, 2, 32, TEX), load_block(7, 0, 0, 1023, 0), sync_pipe())
            else:            # drawn into the third buffer without Z test, at a place nothing else touches: the copy stays in the final image
                ys = 4 * rng.randrange(H // 4)
                xs = rng.randrange(0, W - 120)
                s.add(set_color_image(0, 2, W, BUF[2]), set_other_modes(cycle=0), tri(ys, xs, xs + 100, 3.9), sync_pipe(),
                      set_color_image(0, 2, W, BUF[cur]), set_other_modes(cycle=0, z_compare=1, z_update=1, zmode=0))
        elif r == 3:         # the other buffer
            cur ^= 1
            s.add(sync_pipe(), set_color_image(0, 2, W, BUF[cur]), set_scissor(0, 0, W, H))
    s.add(sync_full())
    return s, (2, BUF[0], W, 0x006C02EC, 0x002E020E, 0x200, 0x400)


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-n', type=int, default=60)
    ap.add_argument('-s', type=int, default=1)
    ap.add_argument('--replay', default=os.path.join(ROOT, 'projects', 'unix', '_out_AARCH', 'foir-replay'))
    ap.add_argument('--runner', default='')
    ap.add_argument('--threads', default='1,2,3,4')
    ap.add_argument('--stress', action='store_true', help='long, order-sensitive scenes with many small jobs (see above)')
    ap.add_argument('--lag', action='store_true', help='unbalanced stripe classes and snapshot / job-ring pressure (see above)')
    ap.add_argument('--variants', default='', help='comma list of pool settings every scene is also run under: ' + ','.join(VARIANTS))
    ap.add_argument('--comb', default='', help='FOIR_SPEC_COMB for the candidate runs (auto, exact, shape, generic)')
    ap.add_argument('--tex', default='', help='FOIR_SPEC_TEX for the candidate runs (auto, exact, shape, generic)')
    args = ap.parse_args()
    env = dict(os.environ)
    if args.comb: env['FOIR_SPEC_COMB'] = args.comb
    if args.tex: env['FOIR_SPEC_TEX'] = args.tex
    env = env if (args.comb or args.tex) else None
    if not os.path.exists(args.replay):
        args.replay = os.path.join(ROOT, 'projects', 'unix', 'foir-replay')
    threads = [int(t) for t in args.threads.split(',') if t]
    variants = [v for v in args.variants.split(',') if v and v != 'default']
    for v in variants:
        if v not in VARIANTS: ap.error('unknown variant ' + v)
    runner = shlex.split(args.runner)
    bad = 0
    bad_scenes = 0
    nruns = 0
    cov, ncov = [0.0] * 5, 0
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(args.n):
            seed = args.s + i
            rng = random.Random(seed)
            sc, vi = make_lag_scene(rng) if args.lag else make_scene(rng, args.stress)
            d = os.path.join(tmp, 's.fdump')
            sc.write(d, vi)
            ref = os.path.join(tmp, 'ref.rdram')
            subprocess.run(runner + [args.replay, d, '-o', ref, '--scalar'], capture_output=True, check=True)
            h0 = sha(ref)
            line = []
            runs = [(t, 'default') for t in [0] + threads] + [(t, v) for v in variants for t in threads]
            for t, var in runs:
                out = os.path.join(tmp, 'c.rdram')
                renv = dict(env or os.environ)
                renv.update(VARIANTS[var])
                res = subprocess.run(runner + [args.replay, d, '-o', out] + (['-t', str(t)] if t else []), capture_output=True, check=True, text=True, env=renv)
                if t == 0:
                    m = re.search(r'specialised pixels: texture ([\d.]+)%.*?combine ([\d.]+)%, zcmp ([\d.]+)%, blend ([\d.]+)%, store ([\d.]+)%', res.stderr)
                    if m:
                        for i in range(5): cov[i] += float(m.group(i + 1))
                        ncov += 1
                ok = sha(out) == h0
                line.append('%s%s%s' % ('inline' if not t else '%dt' % t, '' if var == 'default' else '/' + var, '' if ok else ':DIFF'))
                if not ok:
                    bad += 1
            nruns += len(line)
            if any('DIFF' in x for x in line):
                bad_scenes += 1
                print('seed %d: %s' % (seed, ' '.join(line)))
                sys.stdout.flush()
        if ncov:
            print('mtfuzz: mean share of the pixels in specialised variants: texture %.0f%%, combine %.0f%%, zcmp %.0f%%, blend %.0f%%, store %.0f%%' %
                  tuple(c / ncov for c in cov))
        print('mtfuzz: %d scene(s), %d difference(s)%s' % (args.n, bad, ' (%d of %d scenes, %d of %d runs differ)' % (bad_scenes, args.n, bad, nruns) if bad else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
