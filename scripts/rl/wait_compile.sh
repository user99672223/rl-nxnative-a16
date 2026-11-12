#!/bin/bash
# Laptop: wait for the newest "RL compile check" run (or the run given) and print its result and the compiler's errors.
GH="${GH:-/home/familybadgujarwilly/.local/bin/gh}"; export GH_CONFIG_DIR="${GH_CONFIG_DIR:-/home/familybadgujarwilly/.config/gh}"
REPO="${RL_REPO:-user99672223/rl-nxnative-a16}"
run="${1:-$("$GH" run list -R "$REPO" -w "RL compile check" -L 1 --json databaseId -q '.[0].databaseId')}"
"$GH" run watch "$run" -R "$REPO" -i 20 >/dev/null 2>&1
echo "run $run: $("$GH" run view "$run" -R "$REPO" --json conclusion,headSha -q '.conclusion+" "+.headSha[0:8]')"
"$GH" run view "$run" -R "$REPO" --json jobs -q '.jobs[].steps[]|select(.conclusion=="failure")|.name'
"$GH" run view "$run" -R "$REPO" --log 2>/dev/null | cut -f3- | cut -c30-400 | grep -E "error|Error|FATAL|\[RED\]|failed|iOS compile check|null device" | grep -v "grep -E\|PIPESTATUS\|0 failed" | head -60
