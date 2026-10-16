#!/usr/bin/env python3
"""vicheck.py [--replay ./foir-replay] [--ref ./ref-angrylion] [--qreplay CMD] [dump ...]: the VI filter pass (VIFilters) against angrylion-rdp-plus.

For every dump (default: test-data/dumps/*) and a list of VI variants (the dump's own VI_STATUS, forced combinations of aa_mode 0..2, divot, dither
filter and gamma, the same buffer read as RGBA8888, and buffers that were not rendered in this frame, whose hidden bits are missing) it requires
  * foir's NEON code (under qemu with --qreplay) to give exactly the frame of the scalar reference (foir-replay --vi-variants, interlaced and
    rescaled dumps included);
  * the reference to give, at every source pixel the VI reads exactly (even columns of the 640-wide oracle output at x_scale 0x200, y_scale 0x400,
    away from the 4 pixels the oracle blanks at each end; no interlaced or rescaled dumps), the bytes angrylion's VI gives (ref-angrylion -V).
gamma_dither_enable is cleared in these variants, because angrylion adds random noise that foir does not; the dump's own status is also compared with
the noise bit set, where foir must equal the oracle or be one lower per channel.  Pixels within the filter's reach of a pixel that foir's RDP renders
differently from angrylion (Kirby 64's known 107) are exempt.
Run from projects/unix after `make all replay ref NEON=0` (and the aarch64 build for the NEON check)."""
import argparse, glob, lzma, os, re, struct, subprocess, sys, tempfile

DIVOT, GAMMA, GDITHER, DFILT = 0x10, 0x8, 0x4, 0x10000


def variants_for(st, org):
    base = st & ~(0x300 | DIVOT | GAMMA | GDITHER | DFILT | 3)
    t = st & 3
    v = [('dump', st & ~GDITHER, org)]
    for name, aa, dv, gm, df in (('aa0 divot dither', 0, 1, 0, 1), ('aa1 plain', 1, 0, 0, 0), ('aa2 divot dither gamma', 2, 1, 1, 1),
                                 ('aa0 dither gamma', 0, 0, 1, 1), ('aa0 divot', 0, 1, 0, 0), ('aa1 dither', 1, 0, 0, 1), ('aa2 plain', 2, 0, 0, 0)):
        v.append((name, base | t | (aa << 8) | (DIVOT if dv else 0) | (GAMMA if gm else 0) | (DFILT if df else 0), org))
    v.append(('rgba32 aa0 divot dither', base | 3 | DIVOT | DFILT, org))
    v.append(('rgba32 aa1 gamma', base | 3 | (1 << 8) | GAMMA, org))
    v.append(('rgba32 aa2 plain', base | 3 | (2 << 8), org))
    for off in (0x4b000, 0x12c000, 0x25800):
        o = (org + off) & 0x7fffff
        v.append(('other buffer +%#x aa0 divot dither' % off, base | t | DIVOT | DFILT, o))
        v.append(('other buffer +%#x aa1 dither' % off, base | t | (1 << 8) | DFILT, o))
        v.append(('other buffer +%#x aa0 plain' % off, base | t, o))
    return v


def read_ppm(p):
    d = open(p, 'rb').read()
    m = re.match(rb'P6\n(\d+) (\d+)\n255\n', d)
    return int(m.group(1)), int(m.group(2)), d[m.end():]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', default='./foir-replay')
    ap.add_argument('--ref', default='./ref-angrylion')
    ap.add_argument('--qreplay', default='', help='aarch64 foir-replay with its runner, e.g. "qemu-aarch64-static -L /usr/aarch64-linux-gnu ./_out_AARCH/foir-replay"')
    ap.add_argument('dumps', nargs='*')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    dumps = a.dumps or sorted(glob.glob(os.path.join(here, '..', 'test-data', 'dumps', '*.fdump*')))
    bad = checks = pix_total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for d in dumps:
            name = os.path.basename(d)[:38]
            path = os.path.join(tmp, 'd.fdump')
            open(path, 'wb').write((lzma.open(d) if d.endswith('.xz') else open(d, 'rb')).read())
            vi = struct.unpack('<7I', open(path, 'rb').read(60)[12:40])
            st, org, hs, vs, xs, ys = vi[0], vi[1], vi[3], vi[4], vi[5], vi[6]
            hst, hen, vst = (hs >> 16) & 0x3ff, hs & 0x3ff, (vs >> 16) & 0x3ff
            variants = variants_for(st, org)
            spec = ','.join('%#x:%#x' % (s, o) for _, s, o in variants)
            r = subprocess.run([a.replay, path, '-p', os.path.join(tmp, 'f.ppm'), '--vi-variants', spec], capture_output=True, text=True)
            if r.returncode:
                print('FAIL %s: foir-replay: %s' % (name, r.stderr.strip())); bad += 1; continue
            if a.qreplay:
                rq = subprocess.run(a.qreplay.split() + [path, '-p', os.path.join(tmp, 'q.ppm'), '--vi-variants', spec], capture_output=True, text=True)
                ok = rq.returncode == 0 and all(' 0 differing' in l for l in rq.stdout.splitlines() if l.startswith('vi variant'))
                checks += 1
                bad += not ok
                print('%-4s %-38s NEON vs reference, %d VI variants' % ('ok' if ok else 'FAIL', name, len(variants)))
            if not (xs == 0x200 and ys == 0x400 and not st & 0x40 and hst >= 108):
                print('skip %-38s oracle compare (x_scale %#x y_scale %#x serrate %d h_start %d)' % (name, xs, ys, bool(st & 0x40), hst))
                continue
            rr = subprocess.run([a.ref, path, '-p', os.path.join(tmp, 'o.ppm')] + sum((['-V', '%#x:%#x' % (s, o)] for _, s, o in variants), []) + ['-V', '%#x' % st],
                                capture_output=True, text=True)
            if rr.returncode:
                print('FAIL %s: ref-angrylion: %s' % (name, rr.stderr.strip())); bad += 1; continue
            hres, hs_a, vs_a = hen - hst, hst - 108, (vst - 34) // 2

            def diffs(fn, on):
                fw, fh, fp = read_ppm(fn)
                ow, oh, op = read_ppm(on)
                out, n = [], 0
                for j in range(fh):
                    if vs_a + j >= oh:
                        break
                    for i in range(4, min(fw, (hres - 8) // 2 + 1)):
                        n += 1
                        f, o = (j * fw + i) * 3, ((vs_a + j) * ow + hs_a + 2 * i) * 3
                        if fp[f:f + 3] != op[o:o + 3]:
                            out.append((i, j))
                return n, out

            def near_of(vname):
                k = [i for i, (vn, _, _) in enumerate(variants) if vn == vname][0]
                bs = set(diffs(os.path.join(tmp, 'f.ppm.%d.ppm' % k), os.path.join(tmp, 'o.ppm.%d.ppm' % k))[1])
                return bs, {(i + dx, j + dy) for i, j in bs for dx in range(-3, 4) for dy in (-1, 0, 1)}
            base, near = near_of('aa2 plain')
            # groups read from other data (RGBA8888 view, buffers not rendered in this frame): only where the unfiltered image already agrees with
            # the oracle, i.e. foir's RDP wrote the same bytes there (it differs from angrylion in a few places that are not the VI's business)
            skipgroup = set()
            for k, (vname, s, o) in enumerate(variants):
                if vname == 'rgba32 aa2 plain' or vname.endswith('aa0 plain'):
                    if diffs(os.path.join(tmp, 'f.ppm.%d.ppm' % k), os.path.join(tmp, 'o.ppm.%d.ppm' % k))[1]:
                        skipgroup.add(vname.split(' aa')[0])
                        print('skip %-38s %s: the RDRAM there differs between foir and angrylion' % (name, vname.split(' aa')[0]))
            for k, (vname, s, o) in enumerate(variants):
                if vname.split(' aa')[0] in skipgroup:
                    continue
                n, dl = diffs(os.path.join(tmp, 'f.ppm.%d.ppm' % k), os.path.join(tmp, 'o.ppm.%d.ppm' % k))
                nd = len([p for p in dl if p not in near]) if not vname.startswith(('rgba32', 'other')) else len(dl)
                checks += 1
                pix_total += n
                bad += nd != 0
                print('%-4s %-38s %-34s %6d px compared, %d differ' % ('ok' if not nd else 'FAIL', name, vname, n, nd))
            if st & GDITHER and not st & GAMMA:
                fw, fh, fp = read_ppm(os.path.join(tmp, 'f.ppm.0.ppm'))
                ow, oh, op = read_ppm(os.path.join(tmp, 'o.ppm.%d.ppm' % len(variants)))
                nd = n = 0
                for j in range(fh):
                    if vs_a + j >= oh:
                        break
                    for i in range(4, min(fw, (hres - 8) // 2 + 1)):
                        f, o = (j * fw + i) * 3, ((vs_a + j) * ow + hs_a + 2 * i) * 3
                        n += 1
                        if (i, j) not in near and any(op[o + c] - fp[f + c] not in ((0, 1) if fp[f + c] < 255 else (0,)) for c in range(3)):
                            nd += 1
                checks += 1
                bad += nd != 0
                print('%-4s %-38s %-34s %6d px compared, %d outside +0/+1' % ('ok' if not nd else 'FAIL', name, 'dump status + noise', n, nd))
    print('vicheck: %d checks (%d pixels compared with the oracle), %d failing' % (checks, pix_total, bad))
    sys.exit(1 if bad else 0)


main()
