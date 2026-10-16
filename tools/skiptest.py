#!/usr/bin/env python3
"""skiptest.py [--replay ./foir-replay] [--runner CMD]: FrameSkip against the unskipped run, on synthetic multi-frame scenes.

Each scene is a list of frames (a SYNC_FULL ends each); foir-replay --skip N --scan a,b,c plays them as a game would: after each frame the VI scans out
the next of its buffers (a hash of what it shows, and whether FrameSkip would hold the previous picture instead, is printed per frame).  Required:
  * (the VI shows the buffer drawn one frame earlier, as with triple buffering)
  * every frame the VI would show (not held) is identical to the same frame of the run without FrameSkip;
  * the frames FrameSkip skips are the ones it holds (every frame from the second one on that is skipped is reported stale);
  * the picture keeps up: with FrameSkip 1 at least 30 % of the frames are shown (a skip pattern locked onto one of two syncs per frame shows almost none).
Scenes (each is a way games break a naive "drop every primitive of a skipped frame"):
  offscreen   a 32x32 target is rendered every frame and textured into the screen buffer (Paper Mario, Mario Golf: the targets must be drawn);
  border      the screen buffers start with noise and each gets its border cleared once by a fill rectangle (Star Fox 64: fills must be drawn);
  history     three screen buffers and three off-screen targets per frame, more colour images than the old four-entry history held (Mario Golf);
  double      two sync points per frame, an off-screen target first (the skip pattern must not lock onto one of them);
  overlay     a static scene, then frames that only draw an overlay on top of the buffer (a pause menu; a skipped scene frame must be refilled).
Mutations that the scenes catch (--expect-fail with --flags): '--skip-legacy' (the behaviour before these rules) fails offscreen, border, history and double; '--skip-opts 1' (no refill) overlay; '--skip-opts 9' (no refill, fills skipped too) border; '--skip-opts 4' (off-screen targets skipped too) offscreen.
Run from projects/unix after `make replay`."""
import argparse, os, random, re, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rdpgen import *  # noqa

BUFS = [0x200000, 0x300000, 0x400000]
OFF = [0x500000, 0x510000, 0x520000]
Z = 0x600000
NF = 12


def rgba16(r, g, b, a=1):
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | a


def colour(k, salt=0):
    r = random.Random(k * 7919 + salt)
    return rgba16(r.randrange(256), r.randrange(256), r.randrange(256))


def shade_modes():
    return [set_combine(0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4),
            set_other_modes(cycle=0, rgb_dither=3, alpha_dither=3, m1a0=0, m1b0=0, m2a0=1, m2b0=0, m1a1=0, m1b1=0, m2a1=1, m2b1=0)]


def tris(s, seed, n=130, box=(0, 0, 320, 240), size=14):
    """n small opaque shaded triangles in the box"""
    s.add(*shade_modes())
    r = random.Random(seed)
    x0, y0, x1, y1 = box
    for _ in range(n):
        x, y = r.uniform(x0, x1 - size), r.uniform(y0, y1 - size)
        c = [(r.randrange(256), r.randrange(256), r.randrange(256), 255) for _ in range(3)]
        s.add(triangle([(x, y), (x + size, y + r.uniform(0, size)), (x + r.uniform(0, size), y + size)], shade=c))


def to_buffer(s, addr):
    s.add(set_color_image(0, 2, 320, addr), set_scissor(0, 0, 320, 240))


def clear(s, c):
    s.add(set_other_modes(cycle=3), set_fill_color(0x00010001 * c), fill_rect(0, 0, 320, 240), sync_pipe())


def frames(name):
    s = Scene()
    for k in range(18 if name == 'overlay' else NF):
        b = BUFS[k % 3]
        if name == 'offscreen':
            if k % 3 == 1:    # the target is rendered now and then and textured every frame (a cached backdrop)
                s.add(set_color_image(0, 2, 32, OFF[0]), set_scissor(0, 0, 32, 32))
                s.add(*shade_modes())
                c = [(40 * (k % 6), 255 - 40 * (k % 6), 90 * (k % 3), 255)] * 3
                s.add(triangle([(0, 0), (32, 0), (0, 32)], shade=c), triangle([(32, 0), (32, 32), (0, 32)], shade=c), sync_pipe())
            to_buffer(s, b)
            clear(s, rgba16(0, 0, 0))
            tris(s, 1000 + k % 3)
            s.add(set_texture_image(0, 2, 32, OFF[0]), set_tile(0, 2, 0, 0, 7), load_block(7, 0, 0, 32 * 32 - 1, 256), sync_pipe(),
                  set_tile(0, 2, 8, 0, 0), set_tile_size(0, 0, 0, 31, 31), set_other_modes(cycle=2), tex_rect(0, 100, 100, 132, 132, 0, 0, 1.0, 1.0))
        elif name == 'border':
            to_buffer(s, b)
            if 6 <= k < 9:
                clear(s, rgba16(0, 0, 0))                     # the border, once per buffer (after the buffers are known as scanned out)
            s.add(set_other_modes(cycle=3), set_fill_color(0x00010001 * rgba16(0, 0, 40)), fill_rect(8, 8, 312, 232), sync_pipe())   # the picture is redrawn every frame
            tris(s, 2000 + k, box=(8, 8, 312, 232))
        elif name == 'history':
            for i in range(3):
                s.add(set_color_image(0, 2, 32, OFF[i]), set_scissor(0, 0, 32, 32), *shade_modes(),
                      triangle([(0, 0), (32, 0), (0, 32)], shade=[(30 * k % 256, 60 * i, 255, 255)] * 3), sync_pipe())
            to_buffer(s, b)
            clear(s, rgba16(0, 0, 0))
            tris(s, 3000 + k)
        elif name == 'double':
            s.add(set_color_image(0, 2, 32, OFF[0]), set_scissor(0, 0, 32, 32), *shade_modes(),
                  triangle([(0, 0), (32, 0), (0, 32)], shade=[(30 * k % 256, 200, 20, 255)] * 3), sync_full())
            to_buffer(s, b)
            clear(s, rgba16(0, 0, 0))
            tris(s, 4000 + k)
        elif name == 'overlay':
            to_buffer(s, b)
            if k < 9:
                clear(s, rgba16(10, 20, 30))
                tris(s, 5000)                       # the same scene in every buffer (long enough for the buffers to be known as scanned out and skipped)
            else:
                tris(s, 5001, box=(60, 60, 260, 180), size=10)   # the same overlay every frame, on top of what is there
        s.add(sync_full())
    if name == 'border':
        rnd = random.Random(7)
        for a in BUFS:
            for i in range(320 * 240):
                s.wr16(a + 2 * i, rnd.randrange(65536))
    return s


def run(replay, runner, path, args):
    # the VI shows the buffer of the previous frame (double / triple buffering)
    r = subprocess.run(runner + [replay, path, '--scan', ','.join('%#x' % BUFS[(i + 2) % 3] for i in range(3))] + args, capture_output=True, text=True)
    if r.returncode:
        sys.exit('replay failed: ' + r.stderr)
    out = []
    for l in r.stdout.splitlines():
        m = re.match(r'skipframe (\d+) origin ([0-9a-f]+) hash ([0-9a-f]+) (\w+)', l)
        if m:
            out.append((m.group(3), m.group(4) == 'stale'))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', default='./foir-replay')
    ap.add_argument('--runner', default='')
    ap.add_argument('--expect-fail', default='', help='comma list of scenes that must FAIL (for the mutation runs), with the replay flags in --flags')
    ap.add_argument('--flags', default='', help='extra foir-replay flags (e.g. --skip-legacy)')
    ap.add_argument('--skip', type=int, default=1)
    a = ap.parse_args()
    runner = a.runner.split()
    extra = a.flags.split()
    must_fail = [x for x in a.expect_fail.split(',') if x]
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name in ('offscreen', 'border', 'history', 'double', 'overlay'):
            s = frames(name)
            path = os.path.join(tmp, name + '.fdump')
            s.write(path, vi_320x240(BUFS[0]))
            ref = run(a.replay, runner, path, ['--skip', '0'])
            sk = run(a.replay, runner, path, ['--skip', str(a.skip)] + extra)
            wrong = [i for i, ((h, st), (h0, _)) in enumerate(zip(sk, ref)) if not st and h != h0]
            nstale = sum(st for _, st in sk)
            nshown = len(sk) - nstale
            ok = not wrong and nstale > 0 and nshown * 10 >= len(sk) * 3 and len(set(h for h, _ in ref)) > 1   # and the picture keeps moving: at least 30 % of the frames are shown
            if not ok and not wrong and nstale == 0:
                print('note: %s skipped nothing' % name)
            passed = ok if name not in must_fail else not ok
            bad += not passed
            print('%-4s %-10s %2d frames, %2d held, shown frames that differ from the unskipped run: %s%s' % (
                'ok' if passed else 'FAIL', name, len(sk), nstale, wrong if wrong else 'none', ' (expected to fail)' if name in must_fail else ''))
    print('skiptest: %d scene(s) wrong' % bad)
    sys.exit(1 if bad else 0)


main()
