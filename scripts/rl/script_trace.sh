#!/bin/bash
# Laptop: wait for a boot run made with DM_UE3_NAMES=names.tsv and DM_PROBE=main:0xb8dd4@x1+48 (UObject::ProcessEvent, the UFunction's name) and list, frame by frame, the
# script functions called that the first probed frame did not call. Usage: script_trace.sh <run id>
id="$1"; here="$(cd "$(dirname "$0")" && pwd)"
[ -d "$here/../../../RL/ci/$id" ] || "$here/wait_run.sh" "$id" 2>&1 | tail -1
d=$(ls -d "$here/../../../RL/ci/$id"/*/run)
grep -a "DM_KEYS" "$d/host.log" | head -3 | cut -c1-120
grep -a "PROBE main+0xb8dd4" "$d/host.log" | python3 -c "
import sys,re,collections
names={}
for l in open('$d/names.tsv'):
    a=l.rstrip('\n').split('\t')
    if len(a)==2: names[int(a[0])]=a[1]
per=collections.OrderedDict()
for l in sys.stdin:
    f=re.search(r' F=(\d+)',l); raw=re.search(r'\]=([0-9a-f]{32})',l)
    if not f or not raw: continue
    per.setdefault(int(f.group(1)),[]).append(names.get(int.from_bytes(bytes.fromhex(raw.group(1))[:4],'little'),'?'))
common=collections.Counter()
for L in per.values():
    for n in set(L): common[n]+=1
usual={n for n,c in common.items() if c>len(per)*0.4}
print('usual every frame:',', '.join(sorted(usual))[:500])
for fr,L in per.items():
    new=[n for n in dict.fromkeys(L) if n not in usual]
    if new: print(fr,', '.join(new)[:900])
"
