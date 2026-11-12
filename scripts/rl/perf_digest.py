#!/usr/bin/env python3
"""Laptop: digest a performance log (the CSV of DM_PERF_EXPORT or of the panel's "Export performance log") into one line of work counts and times,
and optionally append it to docs/perf/history.tsv.

  perf_digest.py <perf.csv> [--from <s>] [--to <s>] [--label <text>] [--append <history.tsv>]

The counts (draws, passes, render-target MiB loaded and stored, render size) do not depend on the machine; the times do (a CI runner's virtual M1 core,
no GPU on the null GPU). --from / --to cut the window in seconds of the log (the match, not the loading before it)."""
import csv, statistics, sys

def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * len(v)))] if v else 0.0

def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    t0, t1 = float(arg('--from', 0)), float(arg('--to', 1e9))
    rows = [r for r in csv.DictReader(open(sys.argv[1])) if t0 <= float(r['t_s']) <= t1]
    if len(rows) < 2: sys.exit('no rows in that window')
    f = lambda k: [float(r[k]) for r in rows]
    span = float(rows[-1]['t_s']) - float(rows[0]['t_s'])
    gpu = [x for x in f('gpu_ms') if x >= 0]
    sizes = {}
    for r in rows: sizes[(r['render_w'], r['render_h'])] = sizes.get((r['render_w'], r['render_h']), 0) + 1
    size = max(sizes, key=sizes.get)
    d = {
        'label': arg('--label', ''), 'frames': len(rows), 'seconds': f'{span:.1f}', 'fps': f'{(len(rows) - 1) / span:.1f}' if span > 0 else '0',
        'frame_p50_ms': f"{pct(f('frame_ms'), 50):.2f}", 'frame_p95_ms': f"{pct(f('frame_ms'), 95):.2f}", 'frame_p99_ms': f"{pct(f('frame_ms'), 99):.2f}",
        'cpu_p50_ms': f"{pct(f('cpu_ms'), 50):.2f}", 'cpu_p95_ms': f"{pct(f('cpu_ms'), 95):.2f}",
        'gpu_p50_ms': f'{pct(gpu, 50):.2f}' if gpu else '-', 'gpu_p95_ms': f'{pct(gpu, 95):.2f}' if gpu else '-',
        'draws_mean': f"{statistics.mean(f('draws')):.0f}", 'draws_p95': f"{pct(f('draws'), 95):.0f}",
        'passes_mean': f"{statistics.mean(f('passes')):.1f}", 'rt_mb_mean': f"{statistics.mean(f('rt_load_store_mb')):.1f}", 'rt_mb_p95': f"{pct(f('rt_load_store_mb'), 95):.1f}",
        'pso_new': f"{sum(f('pso_new')):.0f}", 'mem_mb_max': f"{max(f('mem_mb')):.0f}", 'render': f'{size[0]}x{size[1]}',
        'render_sizes': ' '.join(f'{w}x{h}:{n}' for (w, h), n in sorted(sizes.items(), key=lambda x: -x[1])[:4]),
    }
    for k, v in d.items(): print(f'{k:14s} {v}')
    out = arg('--append')
    if out:
        try: have = open(out).read()
        except FileNotFoundError: have = ''
        with open(out, 'a') as fo:
            if not have: fo.write('\t'.join(d) + '\n')
            fo.write('\t'.join(str(v) for v in d.values()) + '\n')

main()
