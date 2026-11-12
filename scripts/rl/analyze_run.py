#!/usr/bin/env python3
"""Laptop: digest a decrypted run folder (../RL/ci/<run>/<name>/run/host.log): how it ended, the red lines, which packages the game opened and when,
frame and NVN call counts, memory. Usage: analyze_run.py <run folder or host.log> [--files] [--nvn]"""
import collections, os, re, sys
p = sys.argv[1]; flags = set(sys.argv[2:])
if os.path.isdir(p):
    c = [os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs if f == 'host.log']
    if not c: sys.exit('no host.log under ' + p)
    p = c[0]
L = open(p, errors='replace').read().split('\n')
print(f'{p}: {len(L)} lines')
def first(pat):
    r = re.compile(pat)
    return next((l for l in L if r.search(l)), None)
end = [l for l in L if re.search(r'FATAL|CRASH sig=|NX_MEMORY_BUDGET_MB exceeded|exit code', l)]
print('ended:', '; '.join(x[:200] for x in end[:3]) or 'no fatal line (alarm or still running)')
bt = first(r'^\[dm\] bt:')
if bt: print('  ', bt[:400])
red = collections.Counter(re.sub(r'\d+', 'N', l.split('[RED]', 1)[1].strip())[:170] for l in L if '[RED]' in l)
print(f'red lines: {sum(red.values())} ({len(red)} kinds)')
for k, v in red.most_common(40): print(f'  {v:5d}  {k}')
frames = [l for l in L if re.search(r'\[nvn\] frame \d+', l)]
if frames: print('frames logged:', len(frames), '| last:', frames[-1][:260])
mem = [l for l in L if 'memory budget:' in l or 'memory up to frame' in l]
for l in mem[-3:]: print('  ', l[:300])
for pat in (r'ue3 settings', r'BC textures', r'WindowInitialize|window texture|SetPresentInterval|present cap', r'oe::', r'account::', r'hid ', r'audio #', r'fs mount', r'null GPU: a message'):
    hits = [l for l in L if re.search(pat, l)]
    if hits:
        print(f'-- {pat}: {len(hits)}')
        for l in hits[:6]: print('  ', l[:330])
opens = [(i, l) for i, l in enumerate(L) if re.search(r'fs (open|Open)', l)]
if opens:
    names = []
    for i, l in opens:
        m = re.search(r'(rom:|save:|persist:|[a-z]+:)/?([^\s"\']+)', l)
        if m: names.append(m.group(0))
    c = collections.Counter(os.path.basename(n) for n in names)
    print(f'-- file opens: {len(opens)}, {len(c)} distinct')
    if '--files' in flags:
        seen = set()
        for n in names:
            b = os.path.basename(n)
            if b not in seen: seen.add(b); print('   ', n)
    else:
        seen = []
        for n in names:
            b = os.path.basename(n)
            if b not in seen: seen.append(b)
        print('   first-open order:', ' '.join(seen[:400]))
if '--nvn' in flags:
    for l in [l for l in L if '[nvn]' in l][:400]: print(l[:400])
