#!/bin/bash
# CI only: encrypt everything under $RL_OUT (logs, surveys, screenshots, builds) to the owner's public recipient, so only
# the owner's laptop can read it. Writes $RL_UPLOAD/<name>.tar.zst.age (split below 1.9 GB). Usage: ci_pack_out.sh <name>
set -euo pipefail
name="$1"; here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$RL_UPLOAD"
[ -d "$RL_OUT" ] || mkdir -p "$RL_OUT"
tar -C "$RL_OUT" -cf - . 2>/dev/null | zstd -T0 -6 -q | age -r "$(cat "$here/recipient.txt")" | split -b 1900m - "$RL_UPLOAD/$name.tar.zst.age.part"
n=$(ls "$RL_UPLOAD" | wc -l | tr -d ' ')
[ "$n" = 1 ] && mv "$RL_UPLOAD/$name.tar.zst.age.partaa" "$RL_UPLOAD/$name.tar.zst.age"
echo "encrypted output: $n file(s), $(du -sh "$RL_UPLOAD" | cut -f1)"
