#!/bin/bash
# CI only: the build folder (rewritten modules, shader libraries, host; not the extracted game, which is re-extracted in seconds) kept between runs as one age-encrypted
# archive in the Actions cache, so nothing derived from the game is stored in plaintext. Usage: ci_cache.sh save|restore   (RL_GAME_DIR, RL_CACHE_DIR, RL_SECRET_DIR)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; mkdir -p "$RL_CACHE_DIR"
case "$1" in
  save)
    B=$(ls -d "$RL_GAME_DIR"/*_build 2>/dev/null | head -1) || true
    [ -n "${B:-}" ] || { echo "no build folder to cache"; exit 0; }
    rm -f "$RL_CACHE_DIR"/build.tar.zst.age
    tar -C "$RL_GAME_DIR" --exclude='*/extract' --exclude='*/run' --exclude='*/bench' -cpf - "$(basename "$B")" 2>/dev/null | zstd -T0 -3 -q | age -r "$(cat "$here/inputs_recipient.txt")" -o "$RL_CACHE_DIR/build.tar.zst.age"
    echo "cache archive: $(du -sh "$RL_CACHE_DIR" | cut -f1)";;
  restore)
    [ -f "$RL_CACHE_DIR/build.tar.zst.age" ] || { echo "no cached build"; exit 0; }
    mkdir -p "$RL_GAME_DIR"
    age -d -i "$RL_SECRET_DIR/inputs.key" "$RL_CACHE_DIR/build.tar.zst.age" 2>/dev/null | zstd -dc | tar -C "$RL_GAME_DIR" -xpf - && echo "cached build restored" || echo "::warning::cached build could not be restored";;
esac
