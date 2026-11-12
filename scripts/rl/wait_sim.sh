#!/bin/bash
# Laptop: wait for the newest "RL iOS Simulator tests" run (or the run given), print its test lines, download and decrypt its pictures and log to ../RL/ci/sim-<run>/.
GH="${GH:-/home/familybadgujarwilly/.local/bin/gh}"; export GH_CONFIG_DIR="${GH_CONFIG_DIR:-/home/familybadgujarwilly/.config/gh}"
REPO="${RL_REPO:-user99672223/rl-nxnative-a16}"; KEY="${RL_AGE_KEY:-/home/familybadgujarwilly/.config/rl-port/age.key}"
run="${1:-$("$GH" run list -R "$REPO" -w "RL iOS Simulator tests" -L 1 --json databaseId -q '.[0].databaseId')}"
"$GH" run watch "$run" -R "$REPO" -i 20 >/dev/null 2>&1
echo "run $run: $("$GH" run view "$run" -R "$REPO" --json conclusion,headSha -q '.conclusion+" "+.headSha[0:8]')"
"$GH" run view "$run" -R "$REPO" --json jobs -q '.jobs[].steps[]|select(.conclusion=="failure")|"FAILED STEP: "+.name'
"$GH" run view "$run" -R "$REPO" --log 2>/dev/null | cut -f3- | cut -c30-420 | grep -E "error|failed|\*\* TEST|Executed [0-9]+ tests|XCTAssert|no iPhone|\.png" | grep -v "grep -E" | head -80
dest="$(cd "$(dirname "$0")/../.." && pwd)/../RL/ci/sim-$run"; mkdir -p "$dest/enc"
"$GH" run download "$run" -R "$REPO" -D "$dest/enc" >/dev/null 2>&1 && for f in $(find "$dest/enc" -name '*.tar.age'); do age -d -i "$KEY" "$f" | tar -C "$dest" -xf - && echo "decrypted to $dest"; done
rm -rf "$dest/enc"
