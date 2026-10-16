#!/usr/bin/env python3
"""croptest.py [--replay ./foir-replay] [--runner CMD] [dump ...]: CropOverscanH / CropOverscanV screenshot check.

For each dump (default: test-data/dumps/*, plus every distinct VI geometry among them) renders the VI frame with foir-replay
without a crop and with --crop-h / --crop-v / both, with InterlaceHack on and off (--weave), and requires that
  * exactly CROP_H (x2 for a frame >= 512 wide) columns are gone from the left and from the right edge, CROP_V (x2 for >= 360 lines)
    lines from the top and from the bottom, an axis that would leave < 32 pixels is untouched,
  * the remaining pixels are identical to the matching sub-rectangle of the uncropped frame (so the same lines go in each field),
  * the screen aspect fields (what Widescreen and the 4:3 box use) do not change.
Widescreen only changes how gl_out maps the frame to the window, after this stage, so it needs no extra case.
Run from projects/unix after `make replay`."""
import argparse, glob, lzma, os, re, subprocess, sys, tempfile

CROP_H, CROP_V = 8, 12   # keep in step with src/vi.h


def read_ppm(p):
    d = open(p, 'rb').read()
    m = re.match(rb'P6\n(\d+) (\d+)\n255\n', d)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[m.end():]


def render(replay, runner, dump, args, tmp):
    out = os.path.join(tmp, 'o.ppm')
    r = subprocess.run(runner + [replay, dump, '-p', out] + args, capture_output=True, text=True)
    if r.returncode:
        sys.exit('replay failed: %s' % r.stderr)
    m = re.search(r'screen ([\d.]+) x ([\d.]+)', r.stderr)
    return read_ppm(out), m.groups()


def sub(pix, w, x0, y0, nw, nh):
    return b''.join(pix[((y0 + y) * w + x0) * 3:((y0 + y) * w + x0 + nw) * 3] for y in range(nh))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', default='./foir-replay')
    ap.add_argument('--runner', default='')
    ap.add_argument('dumps', nargs='*')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    dumps = a.dumps or sorted(glob.glob(os.path.join(here, '..', 'test-data', 'dumps', '*.fdump*')))
    runner = a.runner.split()
    bad = checks = 0
    seen_geo = {}
    with tempfile.TemporaryDirectory() as tmp:
        for d in dumps:
            path = d
            if d.endswith('.xz'):
                path = os.path.join(tmp, 'd.fdump')
                open(path, 'wb').write(lzma.open(d).read())
            for weave in ([], ['--weave'], ['--field', '0'], ['--field', '1']):
                (w, h, base), sc0 = render(a.replay, runner, path, weave, tmp)
                if weave and weave[0] == '--field' and not os.path.basename(d).startswith('Donkey_Kong_64_800'):
                    continue   # field-rendered frames: the hi-res interlaced dump only
                key = (w, h, tuple(weave))
                if key in seen_geo:
                    continue   # one dump per geometry is enough
                seen_geo[key] = os.path.basename(d)
                if weave and weave[0] == '--field':   # InterlaceHack must actually engage: lines come in equal pairs
                    pairs = all(base[(2 * y * w) * 3:((2 * y + 1) * w) * 3] == base[((2 * y + 1) * w) * 3:((2 * y + 2) * w) * 3] for y in range(h // 2))
                    checks += 1
                    bad += not pairs
                    print('%-4s %s %s: field-hack frame has line-doubled pairs' % ('ok' if pairs else 'FAIL', os.path.basename(d), ' '.join(weave)))
                for flags in (['--crop-h'], ['--crop-v'], ['--crop-h', '--crop-v']):
                    (cw, ch, pix), sc = render(a.replay, runner, path, weave + flags, tmp)
                    cx = CROP_H * (2 if w >= 512 else 1) if '--crop-h' in flags and w - 2 * CROP_H * (2 if w >= 512 else 1) >= 32 else 0
                    cy = CROP_V * (2 if h >= 360 else 1) if '--crop-v' in flags and h - 2 * CROP_V * (2 if h >= 360 else 1) >= 32 else 0
                    ok = (cw, ch) == (w - 2 * cx, h - 2 * cy) and sc == sc0 and pix == sub(base, w, cx, cy, cw, ch)
                    checks += 1
                    if not ok:
                        bad += 1
                    print('%-4s %-44s %dx%d %-18s -> %dx%d (expect %dx%d, %d px/edge H, %d V)' % (
                        'ok' if ok else 'FAIL', os.path.basename(d)[:44] + (' ' + ' '.join(weave) if weave else ''), w, h, ' '.join(flags), cw, ch,
                        w - 2 * cx, h - 2 * cy, cx, cy))
    print('croptest: %d checks, %d failing' % (checks, bad))
    sys.exit(1 if bad else 0)


main()
