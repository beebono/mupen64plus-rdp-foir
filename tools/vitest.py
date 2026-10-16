#!/usr/bin/env python3
"""vitest.py [path/to/foir-replay] (from projects/unix after `make replay`): InterlaceHack on a buffer drawn one field at a time.
  field   a 480i frame drawn one field at a time (the odd field red over a blue frame) shows red on every row with the hack, woven rows without
  bob     the fresh lines stay where they are, the others are the mean of the fresh lines around them (no half-line jump between the fields)
  strided a buffer scanned out with a stride of two rows (it already shows one field) is left alone
  other   a buffer that was not drawn in fields, one that was drawn whole afterwards, and a different buffer are left alone"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rdpgen import *  # noqa

FB, FB2 = 0x200000, 0x400000
BLUE, RED, GREEN = 0x003f, 0xf801, 0x07c1
VI640 = (0x006C02EC, 0x002E020E, 0x400, 0x800)      # h_start, v_start, x_scale, y_scale of a 640x474 frame
VI512 = (0x006C02EC, 0x002E020E, 0x333, 0x400)      # a 512-wide buffer read with a stride of 1024 (two rows)


def fill(c, y0=0, y1=479, w=640):
    return [set_fill_color(0x00010001 * c), fill_rect(0, y0, w - 1, y1)]


def bands(a, b, w=640):
    return fill(a, 0, 239, w) + fill(b, 240, 479, w)


def build(kind):
    s = Scene()
    w = 512 if kind == 'strided' else 640
    s.add(set_color_image(0, 2, w, FB), set_scissor(0, 0, w, 480), set_other_modes(cycle=3))
    s.add(*fill(BLUE, w=w), sync_pipe())
    if kind == 'field':
        s.add(set_scissor(0, 0, 640, 480, field=1, odd=1), *fill(RED), sync_full())
    elif kind == 'bob':
        s.add(set_scissor(0, 0, 640, 480, field=1, odd=1), *bands(RED, GREEN), sync_full())
    elif kind == 'crop':
        s.add(set_scissor(0, 0, 640, 480, field=1, odd=1), *fill(RED, 0, 24), *fill(GREEN, 25, 455), *fill(RED, 456, 479), sync_full())
    elif kind == 'strided':
        s.add(set_scissor(0, 0, 512, 480, field=1, odd=1), *bands(RED, GREEN, 512), sync_full())
    elif kind == 'plain':
        s.add(*bands(RED, GREEN), sync_full())
    elif kind == 'whole':
        s.add(set_scissor(0, 0, 640, 480, field=1, odd=1), *bands(RED, GREEN), sync_pipe(), set_scissor(0, 0, 640, 480), *bands(GREEN, RED), sync_full())
    elif kind == 'other':
        s.add(set_scissor(0, 0, 640, 480, field=1, odd=1), *bands(RED, GREEN), sync_pipe(), set_color_image(0, 2, 640, FB2), set_scissor(0, 0, 640, 480),
              *bands(GREEN, RED), sync_full())
    vi = (2 | 0x40, FB + (1024 if kind == 'strided' else 0), 1024 if kind == 'strided' else 640) + (VI512 if kind == 'strided' else VI640)
    if kind == 'other':
        vi = (2 | 0x40, FB2, 640) + VI640
    return s, vi


def render(replay, scene, vi, extra, tmp, name):
    d = os.path.join(tmp, name + '.fdump')
    scene.write(d, vi)
    out = os.path.join(tmp, name + '.ppm')
    subprocess.run([replay, d, '-p', out] + extra, capture_output=True, check=True)
    with open(out, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); f.readline()
        data = f.read()
    return w, h, data


def main():
    replay = sys.argv[1] if len(sys.argv) > 1 else './foir-replay'
    ok = True

    def report(name, good, info=''):
        nonlocal ok
        print('%-22s %s %s' % (name, 'ok' if good else 'FAIL', info))
        ok &= good

    with tempfile.TemporaryDirectory() as tmp:
        for kind in ('field', 'bob', 'crop', 'strided', 'plain', 'whole', 'other'):
            s, vi = build(kind)
            w, h, wv = render(replay, s, vi, ['--weave'], tmp, kind + 'w')
            w2, h2, hk = render(replay, s, vi, [], tmp, kind + 'h')
            px = lambda data, y: tuple(data[y * w * 3:y * w * 3 + 3])
            if kind == 'field':
                res = [px(wv, y) for y in (10, 11)]
                report('field weave', res[0] != res[1], res)
                res = [px(hk, y) for y in (10, 11, 100, 101, 0, h - 1)]
                report('field hack', all(p[0] > 200 and p[2] < 40 for p in res), res)
            elif kind == 'bob':
                # odd lines are fresh: 239 is red, 241 green; line 240 (even) is their mean, line 238 (even) red, line 0 copies line 1
                res = {y: px(hk, y) for y in (0, 1, 238, 239, 240, 241)}
                good = (res[238] == res[239] == res[0] == res[1] and res[239][0] > 200 and res[241][1] > 200 and res[241][0] < 40
                        and abs(res[240][0] - res[239][0] // 2) <= 8 and abs(res[240][1] - res[241][1] // 2) <= 8)
                report('bob', good, res)
            elif kind == 'crop':
                # --crop-v takes 24 lines off each edge of a 474-line frame: the first kept line (24) is a mean with the cropped line 23
                w3, h3, cr = render(replay, s, vi, ['--crop-v'], tmp, 'cropc')
                report('bob + crop-v', (w3, h3) == (w, h - 48) and cr == hk[24 * w * 3:(h - 24) * w * 3] and px(hk, 24) != px(hk, 25), (w3, h3, px(hk, 24)))
            else:
                report(kind + ' untouched', (w, h, wv) == (w2, h2, hk), '%dx%d' % (w, h))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
