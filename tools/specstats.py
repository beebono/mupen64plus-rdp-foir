#!/usr/bin/env python3
"""specstats.py: merge the per-stage key statistics of several runs into a ranked list of kernel variants.

A statistics build (`CFLAGS=-DFOIR_STATS make ...`) run with FOIR_STATE_STATS=1 FOIR_STATE_STATS_ALL=1 prints
one `SPEC <stage> <pixels> <KEY(...)>` line per distinct stage key at exit (foir-replay) or, for the plugin,
to $FOIR_STATE_STATS_FILE (rewritten every 120 frames). This script reads any number of such logs; every log
counts equally (a game's share of a stage's pixels is normalised per log), and prints for each stage the keys
ranked by the mean share, the cumulative coverage, and whether the key is already covered by src/spec_*.def.

The combine and blend stages have two kinds of variants (see kspec_comb.inc): COMB / BLEND with every option
literal, and COMBX / BLENDX ("any option") that serve every state with the same selectors. A key is covered when
either exists. The texture stage likewise has exact variants (TEX / TEXL) and "any format" variants (TEXA / TEXAL, the
structure of the texture unit only, see spec.h); the Z compare keys do not contain ANTIALIAS_EN / FORCE_BLEND (read at
run time). Statistics of older builds (TEX with MID_TEXEL, ZCMP with AA / FORCE) are converted when they are read.

usage: specstats.py [-n top] [--def] [--stage comb] [--min-share 0.05] [--exact-share 2] [--loo N] log...
  --def           print the keys to add to src/spec_<stage>.def: for combine / blend a COMBX / BLENDX line per selector
                  set with a mean share >= --min-share percent, plus a COMB / BLEND line for every exact key with a mean
                  share >= --exact-share percent; other stages: keys with a share >= --min-share
  --loo N         leave-one-log-out coverage of a variant list built from the other logs (top N selector sets)
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), 'src')
STAGES = ['tex', 'comb', 'zcmp', 'blend', 'store']
NAMES = {'tex': 'TEX', 'comb': 'COMB', 'zcmp': 'ZCMP', 'blend': 'BLEND', 'store': 'STORE'}


def legacy(k):
    """keys written by builds before the run-time AA / FORCE_BLEND of the Z compare: drop those fields"""
    m = re.match(r'(\w+)\((.*)\)\s*$', k.strip())
    if not m:
        return k
    name, a = m.group(1), [x.strip() for x in m.group(2).split(',')]
    if name == 'ZCMP' and len(a) == 9:
        del a[5:7]                     # aa, force
    else:
        return k
    return fmt_key(name, a)


def fmt_key(name, a):
    """the grouped spelling of the .def files (spec.c foir_spec_describe prints the same)"""
    groups = {'TEX': (1, 3, 2, 2, 2, 2, 1, 2), 'TEXL': (1, 3, 2, 2, 2, 2, 1, 2, 1), 'ZCMP': (1, 2, 2, 1, 1)}.get(name)
    if not groups or sum(groups) != len(a):
        return '%s(%s)' % (name, ', '.join(a))
    out, i = [], 0
    for g in groups:
        out.append(','.join(a[i:i + g]))
        i += g
    return '%s(%s)' % (name, ', '.join(out))


def generic(k):
    """the statistics' bucket of states a stage handles generically (no variant possible)"""
    return k.startswith('GENERIC(') or re.match(r'[A-Z]+\(255\b', k) is not None


def norm(k):
    return re.sub(r'\s+', '', k)


def split_args(k):
    m = re.match(r'(\w+)\((.*)\)\s*$', k.strip())
    return m.group(1), [a.strip() for a in m.group(2).split(',')]


def xform(stage, k):
    """the "any option" form of a combine / blend key (as text), or None"""
    name, a = split_args(k)
    if stage == 'comb' and name == 'COMB':
        # COMB(two, w0, w1, rgbd, ad, acmp, acs, cta, shrgb, sha, wide)
        return 'COMBX(%s, %s, %s, %s, %s)' % (a[0], a[1], a[2], a[8], a[10])
    if stage == 'blend' and name == 'BLEND':
        # BLEND(two, a0,b0,c0,d0, a1,b1,c1,d1, acmp, dal, aa, force, cvgcol, rgbd)
        return 'BLENDX(%s, %s, %s)' % (a[0], ','.join(a[1:5]), ','.join(a[5:9]))
    if stage == 'tex' and name == 'TEX':
        # TEX(two, t0,t1,nx, fmt0,siz0, fmt1,siz1, smp,mid, tlut,tlt, persp, bl0,bl1) -> TEXA(two, t0,t1,nx, smp, tlut, persp, bl0,bl1)
        return 'TEXA(%s, %s, %s, %s, %s, %s)' % (a[0], ','.join(a[1:4]), a[8], a[10], a[12], ','.join(a[13:15]))
    if stage == 'tex' and name == 'TEXL':
        return 'TEXAL(%s, %s, %s, %s, %s, %s, %s)' % (a[0], ','.join(a[1:4]), a[8], a[10], a[12], ','.join(a[13:15]), a[15])
    return None


def load_def(stage):
    p = os.path.join(SRC, 'spec_%s.def' % stage)
    keys = set()
    if os.path.exists(p):
        for line in open(p):
            m = re.match(r'\s*(%s[XLA]*\(.*?\))\s*(?:/\*.*)?$' % NAMES[stage], line)
            if m:
                keys.add(norm(m.group(1)))
    return keys


def load_fixed(stage):
    """the lines of src/spec_<stage>.def without a comment: the cross-product (matrix) part of the list, which exists
    whatever the statistics say (tools/specmatrix.py)"""
    p = os.path.join(SRC, 'spec_%s.def' % stage)
    keys = set()
    if os.path.exists(p):
        for line in open(p):
            m = re.match(r'\s*(%s[XLA]*\(.*?\))\s*$' % NAMES[stage], line)
            if m:
                keys.add(norm(m.group(1)))
    return keys


def covered_exact(k, have):
    return norm(k) in have


def covered(stage, k, have):
    if norm(k) in have:
        return True
    x = xform(stage, k)
    return x is not None and norm(x) in have


def read_logs(paths):
    per_stage = {s: [] for s in STAGES}
    for path in paths:
        cnt = [collections.Counter() for _ in STAGES]
        for line in open(path, errors='replace'):
            m = re.match(r'SPEC (\d) (\d+) (.*)$', line)
            if m:
                cnt[int(m.group(1))][legacy(m.group(3))] += int(m.group(2))
        for i, s in enumerate(STAGES):
            if cnt[i]:
                per_stage[s].append((os.path.basename(path), cnt[i]))
    return per_stage


def shares(runs, proj=lambda k: k):
    share = collections.defaultdict(float)
    games = collections.defaultdict(set)
    for name, c in runs:
        tot = sum(c.values())
        for k, v in c.items():
            share[proj(k)] += v / tot / len(runs)
            games[proj(k)].add(name)
    return share, games


def loo(stage, runs, top):
    """leave-one-out: variants = the top `top` selector sets (x-forms where they exist) of the other logs, plus the cross-product
    (matrix) variants of the lists"""
    proj = lambda k: xform(stage, k) or k
    fixed = load_fixed(stage)
    res = []
    for i, (name, held) in enumerate(runs):
        others = [r for j, r in enumerate(runs) if j != i]
        sh, _ = shares(others, proj)
        keep = set(k for k, _ in sorted(sh.items(), key=lambda kv: -kv[1])[:top] if not generic(k))
        tot = sum(held.values())
        res.append((sum(v for k, v in held.items() if proj(k) in keep or norm(k) in fixed) / tot, name))
    return res


def loo_policy(stage, runs, min_share, exact_share):
    """leave-one-out with the policy of --def: the variants built from the other logs (selector sets with a mean share >=
    min_share, exact keys >= exact_share, every key for the other stages) plus the cross-product lines of the list"""
    fixed = load_fixed(stage)
    res = []
    for i, (name, held) in enumerate(runs):
        others = [r for j, r in enumerate(runs) if j != i]
        keep = set()
        sh, _ = shares(others)
        for k, v in sh.items():
            if not generic(k) and 100 * v >= (exact_share if stage in ('comb', 'blend') else min_share):
                keep.add(norm(k))
        if stage in ('comb', 'blend'):
            xs, _ = shares(others, lambda k: xform(stage, k) or k)
            for x, v in xs.items():
                if not generic(x) and 100 * v >= min_share:
                    keep.add(norm(x))
        tot = sum(held.values())
        cov = 0
        for k, v in held.items():
            x = xform(stage, k)
            if norm(k) in keep or norm(k) in fixed or (x is not None and norm(x) in keep):
                cov += v
        res.append((cov / tot, name))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('-n', type=int, default=30)
    ap.add_argument('--def', dest='emit', action='store_true')
    ap.add_argument('--stage', choices=STAGES)
    ap.add_argument('--min-share', type=float, default=0.05)
    ap.add_argument('--exact-share', type=float, default=2.0)
    ap.add_argument('--shapes', action='store_true', help='with --def --stage tex: also print the TEXA / TEXAL "any format" kernels of every structure seen')
    ap.add_argument('--loo', type=int, default=0)
    ap.add_argument('--loo-policy', action='store_true', help='leave-one-out coverage with the policy of --def (see --min-share / --exact-share)')
    args = ap.parse_args()

    per_stage = read_logs(args.logs)
    for s in STAGES:
        if args.stage and s != args.stage:
            continue
        runs = per_stage[s]
        if not runs:
            continue
        have = load_def(s)
        if args.loo_policy:
            r = loo_policy(s, runs, args.min_share, args.exact_share)
            print('%-8s leave-one-log-out: mean %5.1f %%  min %5.1f %% (%s)   per log: %s' % (s, 100 * sum(x for x, _ in r) / len(r), 100 * min(x for x, _ in r), min(r)[1],
                  ' '.join('%.0f' % (100 * x) for x, _ in r)))
            continue
        if args.loo:
            r = loo(s, runs, args.loo)
            print('== %s: leave-one-out coverage with the top %d variants of the other logs: mean %.1f%%, min %.1f%% (%s)' %
                  (s, args.loo, 100 * sum(x for x, _ in r) / len(r), 100 * min(x for x, _ in r), min(r)[1]))
            continue
        share, games = shares(runs)
        ranked = sorted(share.items(), key=lambda kv: -kv[1])
        if args.emit:
            kinds = collections.defaultdict(float)
            if s in ('comb', 'blend'):
                xs, _ = shares(runs, lambda k: xform(s, k) or k)
                for x, v in sorted(xs.items(), key=lambda kv: -kv[1]):
                    if generic(x):
                        continue
                    if 100 * v >= args.min_share and norm(x) not in have:
                        print('%s  /* %.2f%% mean */' % (x, 100 * v))
                for k, v in ranked:
                    if 100 * v >= args.exact_share and norm(k) not in have and not generic(k):
                        print('%s  /* %.2f%% mean, %d logs */' % (k, 100 * v, len(games[k])))
            else:
                done = {}
                for k, v in ranked:
                    if 100 * v >= args.min_share and norm(k) not in have and not generic(k):
                        print('%s  /* %.2f%% mean, %d logs */' % (k, 100 * v, len(games[k])))
                        done[norm(k)] = k
                if s == 'tex' and args.shapes:
                    # the "any format" kernels: one per distinct structure seen in any log (every log counts, however small the share)
                    seen = collections.OrderedDict()
                    for k, v in ranked:
                        x = xform(s, k)
                        if x and not generic(k):
                            seen[norm(x)] = (x, seen.get(norm(x), (None, 0.0))[1] + v)
                    for nx, (x, v) in seen.items():
                        if nx not in have:
                            print('%s  /* %.2f%% mean */' % (x, 100 * v))
                if s == 'zcmp':
                    # the exact-knobs twin (RelaxedPrecision off) of every variant with RelaxedPrecision on
                    for nk, k in done.items():
                        if nk.endswith(',1)'):
                            twin = k[:k.rindex(',')] + ', 0)'
                            if norm(twin) not in done and norm(twin) not in have:
                                print('%s  /* RelaxedPrecision off */' % twin)
            continue
        print('== %s: %d logs, %d distinct keys' % (s, len(runs), len(ranked)))
        cum = 0.0
        for i, (k, v) in enumerate(ranked):
            cum += v
            if i < args.n:
                print('%s %5.1f%% (cum %5.1f%%, %2d logs)  %s' % ('*' if covered(s, k, have) else ' ', 100 * v, 100 * cum, len(games[k]), k))
        cov = sum(v for k, v in ranked if covered(s, k, have))
        print('   spec_%s.def covers %.1f%% of the mean share' % (s, 100 * cov))


if __name__ == '__main__':
    main()
