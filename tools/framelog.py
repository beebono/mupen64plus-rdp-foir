#!/usr/bin/env python3
"""Decode a FOIR_FRAMELOG file (src/framelog.h): one record per UpdateScreen (= per VI frame).

  framelog.py FILE            summary: frame-time distribution (UpdateScreen exit to exit = swap to swap) and the 10 slowest frames with their breakdown
  framelog.py FILE --csv      every record as CSV (times in ms)
  framelog.py FILE --from N   only frames > N (e.g. 1440 to skip boot)

Per record (ms): cb = time inside ProcessRDPList (parse + setup + worker waits), sync = of that the SYNC_FULL wait, own = cb - sync,
drain / vi / present = the UpdateScreen itself (present includes the buffer swap and its vsync wait), busy = sum over the workers of the
time spent rendering (busy max = busiest worker), helper = time the emulation thread rendered stripe classes itself while it waited (SyncHelper), jobs / prims / cmds / loads = work in the interval since the previous UpdateScreen,
tcpu = CPU time of the emulation thread in the interval (wall >> tcpu: blocked or preempted), flt / csw = page faults / context switches.
"""
import struct, sys

F = ('t_enter', 't_leave', 'frame', 'lists', 'cb', 'cb_max', 'syncfull', 'mtsync', 'drain', 'vi', 'present', 'cmds', 'prims', 'pixels',
     'loads', 'wk_sum', 'wk_max', 'jobs', 'vi_res', 'thr_cpu', 'thr_faults', 'thr_ctxsw', 'helper', 'reserved')

def read(path):
    d = open(path, 'rb').read()
    magic, rb, cap, n = struct.unpack_from('<8sIII', d, 0)
    assert magic == b'FOIRFL01' and rb in (96, 104), 'not a FOIR_FRAMELOG file'      # 96: before the helper_ns field
    fmt = '<QQ17I3I' + ('2I' if rb == 104 else '')
    return [dict(zip(F, struct.unpack_from(fmt, d, 64 + i * rb) + ((0, 0) if rb == 96 else ()))) for i in range(n)]

def main():
    a = sys.argv[1:]
    if not a: print(__doc__); return
    recs = read(a[0])
    lo = int(a[a.index('--from') + 1]) if '--from' in a else 0
    recs = [r for r in recs if r['frame'] > lo]
    if '--csv' in a:
        print('frame,dt_ms,cb,sync,own,cb_max,drain,vi,present,busy_sum,busy_max,jobs,lists,cmds,prims,pixels,loads')
        for p, r in zip(recs, recs[1:]):
            m = lambda k: r[k] / 1e6
            print(','.join(str(x) for x in (r['frame'], (r['t_leave'] - p['t_leave']) / 1e6, m('cb'), m('syncfull'), (r['cb'] - r['syncfull']) / 1e6,
                  m('cb_max'), m('drain'), m('vi'), m('present'), m('wk_sum'), m('wk_max'), r['jobs'], r['lists'], r['cmds'], r['prims'], r['pixels'], r['loads'])))
        return
    dt = [((r['t_leave'] - p['t_leave']) / 1e6, r) for p, r in zip(recs, recs[1:])]
    v = sorted(x for x, _ in dt)
    n = len(v)
    pc = lambda p: v[min(n - 1, int(n * p / 100))]
    print('%d frames: p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms;  >16.7: %.1f%%  >33.3: %.1f%%' % (n, pc(50), pc(95), pc(99), v[-1],
          100.0 * sum(x > 16.7 for x in v) / n, 100.0 * sum(x > 33.3 for x in v) / n))
    print('slowest frames (UpdateScreen exit to exit = swap to swap):')
    print('  frame   dt   cb  own sync  busy(max)  jobs prims loads | drain   vi present |  tcpu  flt  csw | helper')
    for x, r in sorted(dt, key=lambda t: -t[0])[:10]:
        print('%7d %5.1f %4.1f %4.1f %4.1f %5.1f(%4.1f) %5d %5d %5d | %5.1f %4.1f %6.1f | %5.1f %4d %4d | %5.1f' % (r['frame'], x, r['cb'] / 1e6, (r['cb'] - r['syncfull']) / 1e6,
              r['syncfull'] / 1e6, r['wk_sum'] / 1e6, r['wk_max'] / 1e6, r['jobs'], r['prims'], r['loads'], r['drain'] / 1e6, r['vi'] / 1e6, r['present'] / 1e6, r['thr_cpu'] / 1e6, (r['thr_faults'] & 0xffff) + (r['thr_faults'] >> 16), (r['thr_ctxsw'] & 0xffff) + (r['thr_ctxsw'] >> 16), r['helper'] / 1e6))

if __name__ == '__main__':
    main()
