#!/bin/bash
# CI only: run a command with all of its output going to a log file under $RL_OUT/logs (never to the public console),
# and print only the step name, exit code and duration. Usage: ci_step.sh <name> <command...>
name="$1"; shift
mkdir -p "$RL_OUT/logs"
t0=$(date +%s)
"$@" > "$RL_OUT/logs/$name.log" 2>&1
rc=$?
echo "step $name: rc=$rc, $(( $(date +%s) - t0 )) s, $(wc -l < "$RL_OUT/logs/$name.log" | tr -d ' ') log lines"
exit $rc
