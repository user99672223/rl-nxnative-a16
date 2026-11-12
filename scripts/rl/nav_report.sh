#!/bin/bash
# Laptop: wait for a boot run made with the UI probes (DM_PROBE=main:0xf04c3c@**x3+20,main:0xe2b5c, DM_FS=1, DM_PERF_EXPORT=perf) and say what the game did after second $2 (default 39):
# the UI's calls into the game with their first text argument, the game's log lines, the packages opened, and how draws and passes moved. Usage: nav_report.sh <run id> [from second]
id="$1"; from="${2:-39}"; here="$(cd "$(dirname "$0")" && pwd)"
[ -d "$here/../../../RL/ci/$id" ] || "$here/wait_run.sh" "$id" 2>&1 | tail -1
d=$(ls -d "$here/../../../RL/ci/$id"/*/run); log="$d/host.log"
grep -a "DM_KEYS: [0-9f]" "$log" | sed -E 's/.*DM_KEYS: //' | cut -c1-40 | paste -sd';' | cut -c1-400
echo "-- UI calls after $from s:"
grep -a "PROBE main+0xf04c3c" "$log" | python3 -c "
import sys,re
for l in sys.stdin:
    t=float(re.search(r'\[([0-9.]+)s\]',l).group(1)); m=re.search(r' x2=0x[0-9a-f]+ \"([^\"]*)\"',l); a=re.search(r'\*\* \"([^\"]*)\"',l)
    if t>$from: print('%5.1f %s %s'%(t, m.group(1) if m else '?', a.group(1) if a else ''))
" | uniq -c | head -90
echo "-- game log after $from s:"
grep -a "PROBE main+0xe2b5c" "$log" | awk -F'[][]' -v f="$from" '{t=$4+0; if (t>f) print $0}' | grep -ao '\[[0-9.]*s\]\|U"[^"]*"' | paste -sd' ' | sed 's/\[/\n[/g' | cut -c1-220 | head -40
echo "-- packages opened after $from s:"
grep -a "fs open \[" "$log" | awk -F'[][]' -v f="$from" '{t=$4+0; if (t>f) print $0}' | sed -E 's/.*\[([0-9.]+s)\] rom:\/tagame\/cookednnx\//\1 /; s/ mode=.*//' | head -30 | paste -sd' ' | cut -c1-1200
echo "-- draws / passes:"
python3 - "$d/perf.csv" <<'PY'
import csv,sys,collections
w=collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])): w[int(float(r['t_s'])//4)].append(r)
last=None
for k in sorted(w):
    v=w[k]; d=round(sum(int(r['draws']) for r in v)/len(v),-1); p=round(sum(int(r['passes']) for r in v)/len(v)); m=max(int(r['mem_mb']) for r in v)
    if (d,p)!=last: print(f"{k*4:4d}s draws ~{d:.0f} passes {p} mem {m}"); last=(d,p)
PY
grep -a "FATAL\|CRASH sig\|^\[dm\] bt:" "$log" | head -3 | cut -c1-300
