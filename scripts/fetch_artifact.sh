#!/bin/bash
# Download an encrypted artifact of a workflow run and decrypt it on this laptop.
# Usage: scripts/fetch_artifact.sh <run id> [destination folder]   (the folder must be outside the repository)
set -euo pipefail
GH="${GH:-$(command -v gh || echo /home/familybadgujarwilly/.local/bin/gh)}"
export GH_CONFIG_DIR="${GH_CONFIG_DIR:-/home/familybadgujarwilly/.config/gh}"
KEY="${RL_AGE_KEY:-/home/familybadgujarwilly/.config/rl-port/age.key}"
REPO="${RL_REPO:-user99672223/rl-nxnative-a16}"
run="$1"; dest="${2:-$(cd "$(dirname "$0")/../.." && pwd)/RL/ci/$run}"
mkdir -p "$dest/enc"
"$GH" run download "$run" -R "$REPO" -D "$dest/enc"
find "$dest/enc" -name '*.age*' | sort | sed 's/\.part[a-z]*$//' | sort -u | while read -r base; do
  out="$dest/$(basename "$base" .tar.zst.age)"; mkdir -p "$out"
  cat "$base"* | age -d -i "$KEY" | zstd -dc | tar -C "$out" -xf -
  echo "decrypted to $out"
done
rm -rf "$dest/enc"
