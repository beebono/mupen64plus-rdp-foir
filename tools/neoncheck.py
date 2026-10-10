#!/usr/bin/env python3
"""neoncheck.py: automated bit-exactness check of the optimised renderer against the scalar reference.

For every scene it replays the recorded RDP commands with foir-replay

    reference:  --scalar            (scalar C kernels, parser thread only)
    candidates: default kernels     (NEON on aarch64) with 0, 1, 2 and 3 worker threads
                pool settings       with the max thread count: no helper, 4 classes per worker
                generic kernels     (--nospec: the per-state specialised variants off) inline and with 3 threads
    and, on top of that, the plugin-default knobs (--fast: RelaxedPrecision on, which the scalar kernels do not
    have): specialised against generic, inline and with 3 threads

and requires the final RDRAM images to be byte-identical. Scenes are

    * the captured frames in test-data/dumps/*.fdump(.xz)
    * the synthetic regression scenes of regress.py (generated on the fly)
    * any extra .fdump given on the command line

usage: neoncheck.py [--replay path/to/foir-replay] [--runner "qemu-aarch64-static -L /usr/aarch64-linux-gnu"]
                    [--threads 0,1,2,3] [--no-synthetic] [--no-dumps] [extra.fdump ...]

On an x86_64 host the "candidates" differ from the reference only by the thread count (there are no NEON
kernels there); on aarch64 the check covers NEON, NEON + threads and the scalar path. The script exits
non-zero on the first difference after printing all of them.
"""
import argparse
import hashlib
import lzma
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def replay(args, fdump, out, extra, env=None):
    cmd = shlex.split(args.runner) + [args.replay, fdump, '-o', out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, **env) if env else None)
    if r.returncode:
        raise RuntimeError('%s failed: %s' % (' '.join(cmd), r.stderr.strip()))
    return r.stderr


def collect_scenes(args, tmp):
    scenes = []
    if not args.no_dumps:
        d = os.path.join(ROOT, 'test-data', 'dumps')
        if os.path.isdir(d):
            for fn in sorted(os.listdir(d)):
                p = os.path.join(d, fn)
                if fn.endswith('.fdump.xz'):
                    q = os.path.join(tmp, fn[:-3])
                    with lzma.open(p) as src, open(q, 'wb') as dst:
                        dst.write(src.read())
                    scenes.append((fn[:-9], q))
                elif fn.endswith('.fdump'):
                    scenes.append((fn[:-6], p))
    if not args.no_synthetic:
        import regress
        for name, fn in list(regress.SCENES.items()) + list(regress.XSCENES.items()):
            s, vi = fn()
            q = os.path.join(tmp, 'syn_' + name + '.fdump')
            s.write(q, vi)
            scenes.append(('syn/' + name, q))
    for p in args.extra:
        scenes.append((os.path.basename(p), p))
    return scenes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--replay', default=os.path.join(ROOT, 'projects', 'unix', '_out_AARCH', 'foir-replay'))
    ap.add_argument('--runner', default='')
    ap.add_argument('--threads', default='0,1,2,3')
    ap.add_argument('--no-synthetic', action='store_true')
    ap.add_argument('--no-dumps', action='store_true')
    ap.add_argument('extra', nargs='*')
    args = ap.parse_args()
    if not os.path.exists(args.replay):
        alt = os.path.join(ROOT, 'projects', 'unix', 'foir-replay')
        if os.path.exists(alt):
            args.replay = alt
        else:
            sys.exit('foir-replay not found; build it with `make replay` or pass --replay')
    threads = [int(t) for t in args.threads.split(',') if t != '']

    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        scenes = collect_scenes(args, tmp)
        for name, path in scenes:
            ref = os.path.join(tmp, 'ref.rdram')
            replay(args, path, ref, ['--scalar'])
            href = sha(ref)
            line = []
            cov = ''
            for t in threads:
                out = os.path.join(tmp, 'cand.rdram')
                info = replay(args, path, out, ['-t', str(t)] if t else [])
                kern = 'neon' if 'kernels=neon' in info else 'scalar'
                ok = sha(out) == href
                line.append('%s%s:%s' % (kern, '+%dt' % t if t else '', 'ok' if ok else 'DIFF'))
                if not ok:
                    bad += 1
                os.remove(out)
                if not cov and 'specialised pixels' in info:
                    cov = info.split('specialised pixels:')[1].strip().split('\n')[0]
            if max(threads) > 0:
                # the worker pool's settings: the emulation thread as a helper at its barriers (default, above), no helper, 3 / 4 stripe
                # classes per worker
                for tag, e in (('nohelper', {'FOIR_HELPER': '0'}), ('g4', {'FOIR_MT_CLASSES': '4'})):
                    out = os.path.join(tmp, 'cand.rdram')
                    replay(args, path, out, ['-t', str(max(threads))], env=e)
                    ok = sha(out) == href
                    line.append('+%dt/%s:%s' % (max(threads), tag, 'ok' if ok else 'DIFF'))
                    if not ok:
                        bad += 1
                    os.remove(out)
            if kern == 'neon':
                # the combiner dispatch: shape kernels only (FOIR_SPEC_COMB=shape: exact variants off, shape kernels where
                # one exists, else generic), exact variants only, generic only
                for mode in ('shape', 'exact', 'generic'):
                    for t in sorted(set([0, max(threads)])):
                        out = os.path.join(tmp, 'cand.rdram')
                        replay(args, path, out, ['-t', str(t)] if t else [], env={'FOIR_SPEC_COMB': mode})
                        ok = sha(out) == href
                        line.append('comb=%s%s:%s' % (mode, '+%dt' % t if t else '', 'ok' if ok else 'DIFF'))
                        bad += not ok
                        os.remove(out)
                # the same for the texture unit: "any format" kernels only, exact variants only, generic kernel only
                for mode in ('shape', 'exact', 'generic'):
                    for t in sorted(set([0, max(threads)])):
                        out = os.path.join(tmp, 'cand.rdram')
                        replay(args, path, out, ['-t', str(t)] if t else [], env={'FOIR_SPEC_TEX': mode})
                        ok = sha(out) == href
                        line.append('tex=%s%s:%s' % (mode, '+%dt' % t if t else '', 'ok' if ok else 'DIFF'))
                        bad += not ok
                        os.remove(out)
                # generic kernels (variants off) must give the same bytes as the reference too
                for t in sorted(set([0, max(threads)])):
                    out = os.path.join(tmp, 'cand.rdram')
                    replay(args, path, out, ['--nospec'] + (['-t', str(t)] if t else []))
                    ok = sha(out) == href
                    line.append('generic%s:%s' % ('+%dt' % t if t else '', 'ok' if ok else 'DIFF'))
                    bad += not ok
                    os.remove(out)
                # plugin-default knobs: variants vs generic kernels (no scalar equivalent of RelaxedPrecision)
                out = os.path.join(tmp, 'cand.rdram')
                replay(args, path, out, ['--fast', '--nospec'])
                hfast = sha(out)
                os.remove(out)
                for t in sorted(set([0, max(threads)])):
                    replay(args, path, out, ['--fast'] + (['-t', str(t)] if t else []))
                    ok = sha(out) == hfast
                    line.append('fast%s:%s' % ('+%dt' % t if t else '', 'ok' if ok else 'DIFF'))
                    bad += not ok
                    os.remove(out)
                replay(args, path, out, ['--fast'], env={'FOIR_SPEC_COMB': 'shape'})
                ok = sha(out) == hfast
                line.append('fast,shape:%s' % ('ok' if ok else 'DIFF'))
                bad += not ok
                os.remove(out)
                replay(args, path, out, ['--fast'], env={'FOIR_SPEC_TEX': 'shape'})
                ok = sha(out) == hfast
                line.append('fast,texshape:%s' % ('ok' if ok else 'DIFF'))
                bad += not ok
                os.remove(out)
            os.remove(ref)
            print('%-44s %s' % (name, '  '.join(line)))
            if cov and kern == 'neon':
                print('%-44s   specialised pixels: %s' % ('', cov))
            sys.stdout.flush()
    print('neoncheck: %d scene(s), %d difference(s)' % (len(scenes), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
