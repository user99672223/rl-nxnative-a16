#!/bin/bash
# Laptop: wait for a workflow run, then download and decrypt its artifact. Usage: scripts/rl/wait_run.sh <run id>
GH="${GH:-/home/familybadgujarwilly/.local/bin/gh}"; export GH_CONFIG_DIR="${GH_CONFIG_DIR:-/home/familybadgujarwilly/.config/gh}"
REPO="${RL_REPO:-user99672223/rl-nxnative-a16}"; run="$1"; here="$(cd "$(dirname "$0")" && pwd)"
"$GH" run watch "$run" -R "$REPO" -i 30 >/dev/null 2>&1
"$GH" run view "$run" -R "$REPO" --json conclusion,jobs -q '.conclusion,(.jobs[].steps[]|select(.conclusion!="success" and .conclusion!="skipped")|[.name,.conclusion]|@tsv)'
"$GH" run view "$run" -R "$REPO" --log 2>/dev/null | grep -E "step [a-z0-9_-]+: rc=|##\[error\]|encrypted output" | cut -f3- | cut -c30-220
"$here/../fetch_artifact.sh" "$run" 2>&1 | tail -3
