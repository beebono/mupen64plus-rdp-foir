#!/usr/bin/env python3
"""shapegen.py: the shape-kernel list (src/spec_shape.def) from per-game combiner statistics.

A shape kernel (kspec_shape.inc) is keyed by the cycle count and, per cycle, the *shape* of the RGB and the alpha
equation ((A - B) * C + D): each input is a letter 0 (zero), 1 (constant 1.0), C (COMBINED, cycle 1 of a two-cycle
state) or V (variable: the operand comes from a per-draw table). This script reads the logs of a statistics build
(`SPEC 1 <pixels> COMB(two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide)` lines, see tools/specstats.py),
maps every combiner key to its canonical shape key and prints SHAPE(...) lines.

usage: shapegen.py [options] log-or-dir...
  --min-share P     only shapes with a mean share (percent of combiner pixels, each log weighted equally) >= P
  --one-cycle-product   add every one-cycle (RGB shape, alpha shape) pair of the observed one-cycle halves
  --loo             print leave-one-log-out coverage of the generated list instead of the list
  --cm              (split-operand experiment, FOIR_SK_SPLIT=1) key the kernels on which variable operands are constants

The ext flag (a variable operand can leave 0..255) cannot be seen in the logs except for wide texels: shapes whose
multiply input is the LOD fraction in some log also get an ext = 1 kernel (a sharpened LOD fraction), and so do keys
logged with `wide`.
"""
import argparse
import collections
import glob
import os
import re
import sys

Z, ONE, C, V = 0, 1, 2, 3
LET = '01CV'


def rgb_reg(pos, sel):
    """letter class of an RGB input selector: Z, 1, C (COMBINED rgb), A (COMBINED alpha), or V; plus the register kind"""
    if pos == 0:
        s = sel & 15
        return {0: 'C', 6: '1'}.get(s, 'Z' if s >= 8 else 'V')
    if pos == 1:
        s = sel & 15
        return {0: 'C'}.get(s, 'Z' if s >= 8 else 'V')
    if pos == 2:
        s = sel & 31
        if s == 0:
            return 'C'
        if s == 7:
            return 'A'
        return 'Z' if s >= 16 else 'V'
    s = sel & 7
    return {0: 'C', 6: '1'}.get(s, 'Z' if s == 7 else 'V')


def al_reg(pos, sel):
    s = sel & 7
    if pos == 2:
        return 'Z' if s == 7 else 'V'
    return {0: 'C', 6: '1', 7: 'Z'}.get(s, 'V')


def letter(cls, combact):
    if cls == 'Z':
        return Z
    if cls == '1':
        return ONE
    if cls in 'CA':
        if not combact:
            return Z
        return C if cls == 'C' else V
    return V


def canon(a, b, m, d):
    if m == Z or (a == b and a != V):
        a = b = m = Z
    return a | b << 2 | m << 4 | d << 6


def dec(two, w0, w1):
    c1 = ((w0 >> 5) & 15, (w1 >> 24) & 15, w0 & 31, (w1 >> 6) & 7, (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7)
    if not two:
        return [c1]
    c0 = ((w0 >> 20) & 15, (w1 >> 28) & 15, (w0 >> 15) & 31, (w1 >> 15) & 7, (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7)
    return [c0, c1]


CONST_RGB = {0: {3, 5}, 1: {3, 5, 6, 7}, 2: {3, 5, 6, 10, 12, 14, 15}, 3: {3, 5}}
CONST_AL = {0: {3, 5}, 1: {3, 5}, 2: {3, 5, 6}, 3: {3, 5}}


def shape_of(key):
    """(two, ((rgb0, al0), (rgb1, al1)), ext, cm) of a COMB() key tuple (two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide)"""
    two, w0, w1, wide = key[0], key[1], key[2], key[-1]
    cs = dec(two, w0, w1)
    out = [(0, 0), (0, 0)]
    cm = [[0, 0], [0, 0]]
    texv = lodf = False
    for i, c in enumerate(cs):
        cy = i if two else 1
        combact = two and cy == 1
        L = [letter(rgb_reg(p, c[p]), combact) for p in range(4)]
        rgb = canon(*L)
        La = [letter(al_reg(p, c[4 + p]), combact) for p in range(4)]
        al = canon(*La)
        out[cy] = (rgb, al)
        for p in range(4):
            if (rgb >> (2 * p)) & 3 == V:
                sel = c[p]
                s = sel & (15 if p < 2 else 31 if p == 2 else 7)
                if (p == 0 and s in (1, 2)) or (p == 1 and s in (1, 2)) or (p == 2 and s in (1, 2, 8, 9)) or (p == 3 and s in (1, 2)):
                    texv = True
                if p == 2 and s == 13:
                    lodf = True
                if s in CONST_RGB[p]:
                    cm[cy][0] |= 1 << p
            if (al >> (2 * p)) & 3 == V:
                s = c[4 + p] & 7
                if s in (1, 2):
                    texv = True
                if p == 2 and s == 0:
                    lodf = True
                if s in CONST_AL[p]:
                    cm[cy][1] |= 1 << p
    return (two, tuple(out), bool(texv and wide), lodf, tuple(tuple(x) for x in cm))


def describe(sk, cm=False):
    two, ((r0, a0), (r1, a1)), ext = sk[0], sk[1], sk[2]
    lt = lambda v: ''.join(LET[(v >> (2 * p)) & 3] for p in range(4))
    comment = ('%s %s | %s %s' % (lt(r0), lt(a0), lt(r1), lt(a1))) if two else ('%s %s' % (lt(r1), lt(a1)))
    if cm:
        c = sk[3]
        return 'SHAPEC(%d, 0x%02x, 0x%02x, 0x%02x, 0x%02x, %d, %d, %d, %d, %d)' % (two, r0, a0, r1, a1, ext, c[0][0], c[0][1], c[1][0], c[1][1]), comment
    return 'SHAPE(%d, 0x%02x, 0x%02x, 0x%02x, 0x%02x, %d)' % (two, r0, a0, r1, a1, ext), comment


def load_logs(paths):
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, '*.txt'))) if os.path.isdir(p) else [p]
    logs = {}
    for f in files:
        # windows of one game (NAME_<frame>.txt) form one log
        name = re.sub(r'_\d+\.txt$', '', f) if re.search(r'_\d+\.txt$', f) else f
        cnt = logs.setdefault(name, collections.Counter())
        for l in open(f, errors='replace'):
            m = re.match(r'SPEC 1 (\d+) COMB\((\d), 0x([0-9a-f]+), 0x([0-9a-f]+),\s*([\d, ]+)\)', l)
            if m:
                key = (int(m.group(2)), int(m.group(3), 16), int(m.group(4), 16)) + tuple(int(x) for x in m.group(5).replace(' ', '').split(','))
                cnt[key] += int(m.group(1))
    return {f: c for f, c in logs.items() if c}


def ident_of(sk, cm):
    return (sk[0], sk[1], sk[2]) + ((sk[4],) if cm else ())


def kernel_set(logs, a):
    """the kernels for a set of logs: every shape seen (plus the ext = 1 form of the ones with the LOD fraction as a
    multiply input), and with --one-cycle-product every (RGB, alpha) pair of the one-cycle halves seen"""
    seen = set()
    for cnt in logs:
        for k in cnt:
            sk = shape_of(k)
            seen.add(ident_of(sk, a.cm))
            if sk[3]:      # LOD fraction as a multiply input: a sharpened one needs the extended form
                seen.add(ident_of((sk[0], sk[1], True, sk[3], sk[4]), a.cm))
    chosen = set(seen)
    if a.one_cycle_product:
        r1 = {k[1][1][0] for k in seen if k[0] == 0}
        a1 = {k[1][1][1] for k in seen if k[0] == 0}
        for r in r1:
            for al in a1:
                chosen.add((0, ((0, 0), (r, al)), False) + ((((0, 0), (0, 0)),) if a.cm else ()))
    return chosen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--min-share', type=float, default=0.0)
    ap.add_argument('--one-cycle-product', action='store_true')
    ap.add_argument('--loo', action='store_true')
    ap.add_argument('--cm', action='store_true')
    a = ap.parse_args()
    logs = load_logs(a.logs)
    if not logs:
        sys.exit('no SPEC 1 COMB lines found')
    names = sorted(logs)

    def pixshare(cnt):
        tot = sum(cnt.values())
        pl = collections.Counter()
        for k, n in cnt.items():
            pl[ident_of(shape_of(k), a.cm)] += n / tot
        return pl

    per_log = {f: pixshare(logs[f]) for f in names}
    if a.loo:
        res = []
        for f in names:
            known = kernel_set([logs[g] for g in names if g != f], a)
            res.append(100 * sum(v for k, v in per_log[f].items() if k in known) / (sum(per_log[f].values()) or 1))
        res.sort()
        print('leave-one-log-out coverage of the shape kernels (%d kernels from all logs): mean %.1f%%, median %.1f%%, min %.1f%%, logs < 50%%: %d of %d' %
              (len(kernel_set([logs[g] for g in names], a)), sum(res) / len(res), res[len(res) // 2], res[0], sum(1 for x in res if x < 50), len(res)))
        return
    share = collections.Counter()
    for f in names:
        for k, v in per_log[f].items():
            share[k] += v / len(names)
    chosen = {k for k in kernel_set([logs[g] for g in names], a) if 100 * share.get(k, 0) >= a.min_share}
    print('/* shape-specialised combiner kernels: SHAPE(two, rgb0, alpha0, rgb1, alpha1, ext); see kspec_shape.inc.')
    print(' * Generated by tools/shapegen.py%s from the combiner statistics of %d games (comment: letters A B C D of the RGB and alpha' %
          (' --one-cycle-product' if a.one_cycle_product else '', len(names)))
    print(' * equation, 0 / 1 / C = COMBINED / V = variable operand, and the mean share of the combiner pixels). */')
    for k in sorted(chosen, key=lambda k: -share.get(k, 0)):
        line, comment = describe(k, a.cm)
        print('%s  /* %s  %.2f%% */' % (line, comment, 100 * share.get(k, 0)))


if __name__ == '__main__':
    main()
