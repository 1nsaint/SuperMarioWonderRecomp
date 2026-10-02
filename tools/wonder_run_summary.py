#!/usr/bin/env python3
"""Summary of a phone course run (tools/wonder_phone_course.sh; docs/nvn/android-perf/PHASE4.md §2.3).

Course windows are the `per frame over 60` lines with replay >= 8 ms (menus replay 1-4 ms); everything after the first
of them counts as course. Prints medians and IQRs of the timing fields, present intervals and main-thread frames over
33 ms per 600 course frames, hitch lines, thermal headroom, the memory guard's modes and read times (NX_MEMGUARD), and
staging uploads.

usage: wonder_run_summary.py <phone_run_dir> [--json]
"""
import json
import os
import re
import statistics
import sys

FIELDS = [('fps', r'([\d.]+) fps'), ('replay', r'replay ([\d.]+) ms'), ('encode', r'encode\) ([\d.]+) ms'),
          ('fence', r'fence wait ([\d.]+) ms'), ('other', r'other ([\d.]+) ms'), ('present', r'present ([\d.]+) ms'),
          ('upload', r'upload ([\d.]+) MB')]


def quart(v):
    if not v:
        return None
    v = sorted(v)
    q = statistics.quantiles(v, n=4) if len(v) >= 4 else [v[0], statistics.median(v), v[-1]]
    return {'median': round(statistics.median(v), 2), 'q1': round(q[0], 2), 'q3': round(q[-1], 2), 'n': len(v)}


def main():
    out = sys.argv[1]
    lines = open(os.path.join(out, 'nx.log'), errors='replace').read().splitlines()
    windows, course_at = [], None
    for i, l in enumerate(lines):
        if 'per frame over 60:' not in l:
            continue
        w = {k: float(m.group(1)) for k, rx in FIELDS if (m := re.search(rx, l))}
        if w.get('replay', 0) >= 8:
            if course_at is None:
                course_at = i
            w['line'] = i
            windows.append(w)
    r = {'run': os.path.basename(out.rstrip('/')), 'course_windows': len(windows)}
    for k, _ in FIELDS:
        r[k] = quart([w[k] for w in windows if k in w])
    course = lines[course_at:] if course_at is not None else []
    frames = 60 * sum(1 for l in course if 'per frame over 60:' in l)
    r['course_frames'] = frames
    per600 = lambda n: round(600 * n / frames, 1) if frames else None
    over50 = sum(int(m.group(1)) for l in course if (m := re.search(r'present intervals .* more (\d+)', l)))
    r['present_over_50ms_per600'] = per600(over50)
    main_slow = sum(int(m.group(1)) for l in course if (m := re.search(r'main-thread frame time .* more (\d+)', l)))
    r['main_over_33ms_per600'] = per600(main_slow)
    hitches = [l for l in course if l.startswith('hitch:')]
    r['hitch_lines_per600'] = per600(len(hitches))
    heads = [float(m.group(1)) for l in course if (m := re.search(r'thermal headroom ([\d.]+)', l))]
    r['headroom'] = {'min': min(heads), 'mean': round(statistics.mean(heads), 3)} if heads else None
    # NX_MEMGUARD=ab: hitches and frames per guard source, by the log order of the switches.
    mode, per_mode = 'start', {}
    for l in course:
        m = re.search(r'guard source \S+ -> (\S+)', l)
        if m:
            mode = m.group(1)
        d = per_mode.setdefault(mode, {'frames': 0, 'hitches': 0})
        if 'per frame over 60:' in l:
            d['frames'] += 60
        if l.startswith('hitch:'):
            d['hitches'] += 1
    if len(per_mode) > 1:
        r['hitches_per600_by_guard'] = {k: round(600 * v['hitches'] / v['frames'], 1) for k, v in per_mode.items()
                                        if v['frames']}
    reads = [l.split('memory: ', 1)[1] for l in lines if 'memory:' in l and 'read' in l]
    r['guard_reads'] = reads[-3:]
    staging = [l.split('nvn/vk: ', 1)[-1] for l in course if 'staging:' in l]
    r['staging'] = staging[-2:]
    for f in ('thermal_start.txt', 'thermal_end.txt'):
        p = os.path.join(out, f)
        if os.path.exists(p):
            t = open(p).read().strip().splitlines()
            r[f[:-4]] = t[-1] if t else ''
    if '--json' in sys.argv:
        print(json.dumps(r))
        return
    print(f"== {r['run']}: {r['course_windows']} course windows ({frames} frames)")
    for k, _ in FIELDS:
        q = r[k]
        if q:
            print(f'  {k:8s} median {q["median"]:7.2f}  IQR {q["q1"]:.2f}-{q["q3"]:.2f}')
    for k in ('present_over_50ms_per600', 'main_over_33ms_per600', 'hitch_lines_per600', 'headroom',
              'hitches_per600_by_guard', 'thermal_start', 'thermal_end'):
        if r.get(k) is not None:
            print(f'  {k}: {r[k]}')
    for k in ('guard_reads', 'staging'):
        for x in r.get(k) or []:
            print(f'  {k}: {x}')


if __name__ == '__main__':
    main()
