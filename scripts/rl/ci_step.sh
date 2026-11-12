#!/bin/bash
# CI only: run a command with all of its output going to a log file under $RL_OUT/logs (never to the public console),
# and print only the step name, exit code and duration. While it runs, a heartbeat prints the elapsed time and the packing
# script's latest stage line ("[k/n] stage name", no paths: a line with a slash is not printed). Usage: ci_step.sh <name> <command...>
name="$1"; shift
mkdir -p "$RL_OUT/logs"; log="$RL_OUT/logs/$name.log"
t0=$(date +%s)
"$@" > "$log" 2>&1 &
pid=$!
( while kill -0 $pid 2>/dev/null; do
    sleep 120
    kill -0 $pid 2>/dev/null || break
    stage=$(LC_ALL=C grep -a -E '^\[[0-9:]+\] (. )?\[[0-9]+/[0-9]+\] ' "$log" | LC_ALL=C grep -a -v '[/\\]' | tail -1 | cut -c1-90)
    echo "  ... $name: $(( $(date +%s) - t0 )) s, $(wc -l < "$log" | tr -d ' ') log lines; ${stage:-no stage line}"
  done ) &
hb=$!
wait $pid; rc=$?
kill $hb 2>/dev/null; wait $hb 2>/dev/null
echo "step $name: rc=$rc, $(( $(date +%s) - t0 )) s, $(wc -l < "$log" | tr -d ' ') log lines"
exit $rc
