#!/usr/bin/env python3
"""vitest.py [path/to/foir-replay] (from projects/unix after `make replay`): a 480i frame rendered one field at a time must show the fresh (red)
field on every row with the InterlaceHack, not the woven blue / red rows"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rdpgen import *  # noqa

FB = 0x200000


def main():
    replay = sys.argv[1] if len(sys.argv) > 1 else './foir-replay'
    s = Scene()
    s.add(set_color_image(0, 2, 640, FB), set_scissor(0, 0, 640, 480))
    blue = (0x003e << 0) | 1
    s.add(set_other_modes(cycle=3), set_fill_color(0x00010001 * blue), fill_rect(0, 0, 639, 479), sync_pipe())
    red = 0xf801
    s.add(set_scissor(0, 0, 640, 480, field=1, odd=1), set_fill_color(0x00010001 * red), fill_rect(0, 0, 639, 479), sync_full())
    vi = (2 | 0x40, FB, 640, 0x006C02EC, 0x002E020E, 0x400, 0x800)
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        d = os.path.join(tmp, 'il.fdump')
        s.write(d, vi)
        for name, extra in (('weave', ['--weave']), ('hack', [])):
            out = os.path.join(tmp, name + '.ppm')
            subprocess.run([replay, d, '-p', out] + extra, capture_output=True, check=True)
            with open(out, 'rb') as f:
                f.readline(); w, h = map(int, f.readline().split()); f.readline()
                data = f.read()
            row = lambda y: data[y * w * 3:(y + 1) * w * 3]
            px = lambda y: tuple(row(y)[0:3])
            res = [px(y) for y in (10, 11, 100, 101)]
            if name == 'weave':
                good = res[0] != res[1] and res[2] != res[3]
            else:
                good = res[0] == res[1] == res[2] == res[3] and res[0][0] > 200
            print('%-6s %dx%d rows 10,11,100,101 -> %s %s' % (name, w, h, res, 'ok' if good else 'FAIL'))
            ok &= good
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
