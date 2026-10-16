#!/usr/bin/env python3
"""vicheck.py [--replay ./foir-replay] [--ref ./ref-angrylion] [--qreplay CMD] [dump ...]: the VI filter pass (VIAntiAliasing, VIDivot, VIDedither, VIGamma) against angrylion-rdp-plus.

For every dump (default: test-data/dumps/*) and a list of VI variants (the dump's own VI_STATUS, forced combinations of aa_mode 0..2, divot, dither
filter and gamma, the same buffer read as RGBA8888, buffers that were not rendered in this frame, whose hidden bits are missing, and all 16 option
combinations for a status with every filter on, as 16-bit and RGBA8888 buffers) it requires
  * a filter runs when the game bit and the option are both set: angrylion is given the status with the other filters' bits cleared (AA off = aa_mode 2);
    foir's "filter pass" flag must be set exactly when a filter is left (otherwise the plain conversion is taken, which expands 5 bits by replication,
    so the 16-bit oracle value o is compared as o | o >> 5);
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


OPT_AA, OPT_DIVOT, OPT_DEDITHER, OPT_GAMMA = 1, 2, 4, 8


def group(vname):
    return 'rgba32' if vname.startswith('rgba32') else vname.split(' aa')[0]


def effective(st, mask):
    """VI_STATUS as angrylion should see it for the filters the options allow, and whether a filter is left (the pass runs)"""
    aa = (st >> 8) & 3
    if not mask & OPT_AA and aa <= 1:
        aa = 2
    st &= ~(0x300 | (DIVOT if not mask & OPT_DIVOT else 0) | (GAMMA if not mask & OPT_GAMMA else 0) | (DFILT if not mask & OPT_DEDITHER else 0))
    return st | aa << 8


def variants_for(st, org):
    base = st & ~(0x300 | DIVOT | GAMMA | GDITHER | DFILT | 3)
    t = st & 3
    v = [('dump', st & ~GDITHER, org, 0xf)]
    for name, aa, dv, gm, df in (('aa0 divot dither', 0, 1, 0, 1), ('aa1 plain', 1, 0, 0, 0), ('aa2 divot dither gamma', 2, 1, 1, 1),
                                 ('aa0 dither gamma', 0, 0, 1, 1), ('aa0 divot', 0, 1, 0, 0), ('aa1 dither', 1, 0, 0, 1), ('aa2 plain', 2, 0, 0, 0)):
        v.append((name, base | t | (aa << 8) | (DIVOT if dv else 0) | (GAMMA if gm else 0) | (DFILT if df else 0), org, 0xf))
    v.append(('rgba32 aa0 divot dither', base | 3 | DIVOT | DFILT, org, 0xf))
    v.append(('rgba32 aa1 gamma', base | 3 | (1 << 8) | GAMMA, org, 0xf))
    v.append(('rgba32 aa2 plain', base | 3 | (2 << 8), org, 0xf))
    for off in (0x4b000, 0x12c000, 0x25800):
        o = (org + off) & 0x7fffff
        v.append(('other buffer +%#x aa0 divot dither' % off, base | t | DIVOT | DFILT, o, 0xf))
        v.append(('other buffer +%#x aa1 dither' % off, base | t | (1 << 8) | DFILT, o, 0xf))
        v.append(('other buffer +%#x aa0 plain' % off, base | t, o, 0xf))
    full = DIVOT | GAMMA | DFILT
    for m in range(16):
        v.append(('options %x all on' % m, base | t | full, org, m))
        v.append(('rgba32 options %x all on aa1' % m, base | 3 | (1 << 8) | full, org, m))
    for m in (1, 2, 4, 8, 5, 0xa):
        v.append(('options %x partly on aa1 dither' % m, base | t | (1 << 8) | DFILT, org, m))
        v.append(('options %x aa0 gamma' % m, base | t | GAMMA, org, m))
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
            spec = ','.join('%#x:%#x:%#x' % (s, o, m) for _, s, o, m in variants)
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
            pass_flags = [l.split('filter pass ')[1].split(',')[0] == 'yes' for l in r.stdout.splitlines() if l.startswith('vi variant')]
            exp_pass = []
            for _, s, o, m in variants:
                e = effective(s, m)
                exp_pass.append(bool(m) and (((e >> 8) & 3) <= 1 or bool(e & (DIVOT | GAMMA | DFILT))))
            checks += 1
            if pass_flags != exp_pass:
                bad += 1
                print('FAIL %-38s filter pass taken where it should not be / missing: %s' % (name, [variants[i][0] for i in range(len(variants)) if pass_flags[i] != exp_pass[i]]))
            else:
                print('ok   %-38s filter pass taken exactly when a filter is left (%d variants, %d with the plain conversion)' % (name, len(variants), exp_pass.count(False)))
            rr = subprocess.run([a.ref, path, '-p', os.path.join(tmp, 'o.ppm')] + sum((['-V', '%#x:%#x' % (effective(s, m), o)] for _, s, o, m in variants), []) + ['-V', '%#x' % st],
                                capture_output=True, text=True)
            if rr.returncode:
                print('FAIL %s: ref-angrylion: %s' % (name, rr.stderr.strip())); bad += 1; continue
            hres, hs_a, vs_a = hen - hst, hst - 108, (vst - 34) // 2

            def diffs(fn, on, expand=False):
                fw, fh, fp = read_ppm(fn)
                ow, oh, op = read_ppm(on)
                out, n = [], 0
                for j in range(fh):
                    if vs_a + j >= oh:
                        break
                    for i in range(4, min(fw, (hres - 8) // 2 + 1)):
                        n += 1
                        f, o = (j * fw + i) * 3, ((vs_a + j) * ow + hs_a + 2 * i) * 3
                        if (fp[f:f + 3] != bytes(x | (x >> 5) for x in op[o:o + 3])) if expand else (fp[f:f + 3] != op[o:o + 3]):
                            out.append((i, j))
                return n, out

            def plain16(k):
                return not exp_pass[k] and variants[k][1] & 3 == 2

            def near_of(vname):
                k = [i for i, (vn, _, _, _) in enumerate(variants) if vn == vname][0]
                bs = set(diffs(os.path.join(tmp, 'f.ppm.%d.ppm' % k), os.path.join(tmp, 'o.ppm.%d.ppm' % k), plain16(k))[1])
                return bs, {(i + dx, j + dy) for i, j in bs for dx in range(-3, 4) for dy in (-1, 0, 1)}
            base, near = near_of('aa2 plain')
            # groups read from other data (RGBA8888 view, buffers not rendered in this frame): only where the unfiltered image already agrees with
            # the oracle, i.e. foir's RDP wrote the same bytes there (it differs from angrylion in a few places that are not the VI's business)
            skipgroup = set()
            for k, (vname, s, o, m) in enumerate(variants):
                if vname == 'rgba32 aa2 plain' or vname.endswith('aa0 plain'):
                    if diffs(os.path.join(tmp, 'f.ppm.%d.ppm' % k), os.path.join(tmp, 'o.ppm.%d.ppm' % k), plain16(k))[1]:
                        skipgroup.add(group(vname))
                        print('skip %-38s %s: the RDRAM there differs between foir and angrylion' % (name, group(vname)))
            for k, (vname, s, o, m) in enumerate(variants):
                if group(vname) in skipgroup:
                    continue
                n, dl = diffs(os.path.join(tmp, 'f.ppm.%d.ppm' % k), os.path.join(tmp, 'o.ppm.%d.ppm' % k), plain16(k))
                nd = len([p for p in dl if p not in near]) if not vname.startswith(('rgba32', 'other')) else len(dl)
                checks += 1
                pix_total += n
                bad += nd != 0
                print('%-4s %-38s %-34s %6d px compared, %d differ' % ('ok' if not nd else 'FAIL', name, vname, n, nd))
            if st & GDITHER and not st & GAMMA and exp_pass[0]:
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
