#!/usr/bin/env python3
"""mtfuzz.py: randomised scenes that stress the worker pool's row-ownership and barrier logic.

Every scene is a random sequence of commands: fills, shaded / textured / Z-buffered triangles with random
render modes, switches of the colour image between buffers of different width (also overlapping ones),
loads of texture data straight out of a framebuffer (texture-from-framebuffer) and the textured draws that
use it. Each scene is replayed with the scalar kernels on the parser thread (the reference) and with the
default kernels on 1..N worker threads; the final RDRAM images must be byte-identical.

usage: mtfuzz.py [-n scenes] [-s seed] [--replay path] [--runner "qemu-aarch64-static -L ..."] [--threads 1,2,3,4]
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
import regress  # noqa: upload_tex() for the COPY-cycle rectangles

def load_variants():
    """combiner / blender selector sets of the specialised variants (src/spec_comb.def, spec_blend.def): random scenes
    take their modes from here half of the time so the variants (and their fall back to the generic kernels) get exercised"""
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
    """shapes of the shape kernels (src/spec_shape.def): (two, rgb0, alpha0, rgb1, alpha1, ext)"""
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
    """random selectors (A, B, multiply, add) of an equation half whose canonical shape is sh (see tools/shapegen.py)"""
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
    """a SET_COMBINE word whose shape is one of the shape kernels', with random operands"""
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
TEXC = 0x1b0000         # their texture images


def rgba16(rng):
    return (rng.randrange(32) << 11) | (rng.randrange(32) << 6) | (rng.randrange(32) << 1) | rng.randrange(2)


def rnd_modes(rng, cycle):
    k = dict(cycle=cycle, persp=rng.randrange(2), bilerp_sample=rng.randrange(2), bi0=0 if rng.randrange(10) == 0 else 1,
             bi1=0 if rng.randrange(10) == 0 else 1, sharpen=rng.randrange(6) == 0,
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


def rnd_tri(rng, w, h, textured, z):
    pts = [(rng.uniform(-10, w + 10), rng.uniform(-10, h + 10)) for _ in range(3)]
    shade = [tuple(rng.randrange(256) for _ in range(4)) for _ in range(3)]
    tex = None
    if textured:
        tex = [(rng.uniform(-8, 40), rng.uniform(-8, 40), rng.uniform(0.2, 1.0)) for _ in range(3)]
    zs = [rng.randrange(1000, 60000) for _ in range(3)] if z else None
    try:
        return triangle(pts, shade=shade, tex=tex, zs=zs)
    except Exception:
        return None


def make_scene(rng):
    s = Scene()
    # texture: 32x32 RGBA16 in RDRAM, loaded into TMEM tile 0
    for i in range(32 * 32):
        s.wr16(TEX + i * 2, rgba16(rng))
    widths = [17, 33, 64, 100, 128, 129, 200, 256, 320, 321]
    bpp32 = rng.randrange(5) == 0
    W = rng.choice(widths)
    H = rng.randrange(24, 90)
    size = 3 if bpp32 else 2
    s.add(set_color_image(0, size, W, BUF[0]), set_mask_image(ZB), set_scissor(0, 0, W, H))
    s.add(set_other_modes(cycle=3), set_fill_color(rng.randrange(1 << 32)), fill_rect(0, 0, W - 1, H - 1), sync_pipe())
    s.add(set_other_modes(cycle=3), set_color_image(0, 2, W, ZB), set_fill_color(0xfffcfffc),
          fill_rect(0, 0, W - 1, H - 1), sync_pipe(), set_color_image(0, size, W, BUF[0]))
    s.add(set_texture_image(0, 2, 32, TEX), set_tile(0, 2, 0, 0, 7), load_block(7, 0, 0, 1023, 0), sync_pipe(),
          set_tile(0, 2, 8, 0, 0, maskt=rng.choice([0, 5]), masks=rng.choice([0, 5])), set_tile_size(0, 0, 0, 31, 31))
    cur = 0
    curW = W
    for step in range(rng.randrange(12, 40)):
        op = rng.randrange(12)
        if op <= 4:   # triangles
            cyc = rng.randrange(2)
            textured = rng.randrange(3) != 0
            s.add(rnd_combine(rng, cyc), set_prim_color(rng.randrange(1 << 32)), set_env_color(rng.randrange(1 << 32)),
                  set_other_modes(**rnd_modes(rng, cyc)))
            for _ in range(rng.randrange(1, 4)):
                t = rnd_tri(rng, curW, H, textured, rng.randrange(2))
                if t:
                    s.add(t)
        elif op == 5:  # fill
            x0, y0 = rng.randrange(curW), rng.randrange(H)
            s.add(set_other_modes(cycle=3), set_fill_color(rng.randrange(1 << 32)),
                  fill_rect(x0, y0, min(curW - 1, x0 + rng.randrange(1, curW)), min(H - 1, y0 + rng.randrange(1, H))))
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
    runner = shlex.split(args.runner)
    bad = 0
    cov, ncov = [0.0] * 5, 0
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(args.n):
            seed = args.s + i
            rng = random.Random(seed)
            sc, vi = make_scene(rng)
            d = os.path.join(tmp, 's.fdump')
            sc.write(d, vi)
            ref = os.path.join(tmp, 'ref.rdram')
            subprocess.run(runner + [args.replay, d, '-o', ref, '--scalar'], capture_output=True, check=True)
            h0 = sha(ref)
            line = []
            for t in [0] + threads:
                out = os.path.join(tmp, 'c.rdram')
                res = subprocess.run(runner + [args.replay, d, '-o', out] + (['-t', str(t)] if t else []), capture_output=True, check=True, text=True, env=env)
                if t == 0:
                    m = re.search(r'specialised pixels: texture ([\d.]+)%.*?combine ([\d.]+)%, zcmp ([\d.]+)%, blend ([\d.]+)%, store ([\d.]+)%', res.stderr)
                    if m:
                        for i in range(5): cov[i] += float(m.group(i + 1))
                        ncov += 1
                ok = sha(out) == h0
                line.append('%s%s' % ('inline' if not t else '%dt' % t, '' if ok else ':DIFF'))
                if not ok:
                    bad += 1
            if any('DIFF' in x for x in line):
                print('seed %d: %s' % (seed, ' '.join(line)))
                sys.stdout.flush()
        if ncov:
            print('mtfuzz: mean share of the pixels in specialised variants: texture %.0f%%, combine %.0f%%, zcmp %.0f%%, blend %.0f%%, store %.0f%%' %
                  tuple(c / ncov for c in cov))
        print('mtfuzz: %d scene(s), %d difference(s)' % (args.n, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
