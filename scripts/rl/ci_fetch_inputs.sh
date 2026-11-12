#!/bin/bash
# CI only: download the age-encrypted game inputs from the draft release and decrypt them into $RL_GAME_DIR / $RL_KEYS.
# Prints nothing derived from the inputs. Needs GH_TOKEN, RL_INPUTS_AGE_KEY, RL_GAME_DIR, RL_KEYS, RL_SECRET_DIR.
set -euo pipefail
: "${RL_INPUTS_AGE_KEY:?}" "${RL_GAME_DIR:?}" "${RL_KEYS:?}" "${RL_SECRET_DIR:?}"
umask 077
mkdir -p "$RL_SECRET_DIR/enc" "$RL_GAME_DIR" "$(dirname "$RL_KEYS")"
printf '%s\n' "$RL_INPUTS_AGE_KEY" > "$RL_SECRET_DIR/inputs.key"
gh release download "${RL_INPUTS_TAG:-inputs-v0}" -R "$GITHUB_REPOSITORY" -D "$RL_SECRET_DIR/enc" >/dev/null 2>&1 || { echo "::error::could not download the encrypted inputs"; exit 1; }
( cd "$RL_SECRET_DIR/enc" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1 ) || { echo "::error::encrypted inputs failed their checksum"; exit 1; }
cat "$RL_SECRET_DIR"/enc/game.nsp.age.part* | age -d -i "$RL_SECRET_DIR/inputs.key" -o "$RL_GAME_DIR/game.nsp" 2>/dev/null || { echo "::error::decrypt failed (game)"; exit 1; }
age -d -i "$RL_SECRET_DIR/inputs.key" -o "$RL_KEYS" "$RL_SECRET_DIR/enc/prod.keys.age" 2>/dev/null || { echo "::error::decrypt failed (keys)"; exit 1; }
rm -rf "$RL_SECRET_DIR/enc"
echo "inputs ready"
