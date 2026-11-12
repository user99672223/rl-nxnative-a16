#!/bin/bash
# Download the newest encrypted IPA built by the "RL build" workflow and decrypt it on this laptop.
#
#   scripts/fetch_ipa.sh [run id] [output folder]
#
# The IPA is unsigned: sign and install it with your sideloading tool (turn on Increased Memory Limit if the tool offers it).
# Needs: gh (logged in), age, and the private key this laptop made for the port (~/.config/rl-port/age.key; RL_AGE_KEY names another).
# Nothing is decrypted anywhere but here: the workflow encrypts the IPA to that key's public half before it uploads it.
set -euo pipefail
GH="${GH:-$(command -v gh || true)}"
for c in "$GH" "$HOME/.local/bin/gh" /home/familybadgujarwilly/.local/bin/gh /usr/bin/gh /usr/local/bin/gh /snap/bin/gh; do [ -n "$c" ] && [ -x "$c" ] && { GH="$c"; break; }; done
[ -x "${GH:-}" ] || { echo "gh not found" >&2; exit 1; }
[ -d "${GH_CONFIG_DIR:-$HOME/.config/gh}" ] || export GH_CONFIG_DIR=/home/familybadgujarwilly/.config/gh
KEY="${RL_AGE_KEY:-$HOME/.config/rl-port/age.key}"; [ -f "$KEY" ] || KEY=/home/familybadgujarwilly/.config/rl-port/age.key
[ -f "$KEY" ] || { echo "no private key at $KEY (it was made on the laptop that set the port up)" >&2; exit 1; }
REPO="${RL_REPO:-user99672223/rl-nxnative-a16}"
here="$(cd "$(dirname "$0")" && pwd)"
out="${2:-$(cd "$here/../.." && pwd)/RL/ipa}"
run="${1:-}"
if [ -z "$run" ]; then
  # the newest finished run that has an iPhone artifact
  for r in $("$GH" run list -R "$REPO" -w "RL build" -L 40 --json databaseId,status -q '.[]|select(.status=="completed")|.databaseId'); do
    if "$GH" api "repos/$REPO/actions/runs/$r/artifacts" -q '.artifacts[]|select(.expired==false)|.name' 2>/dev/null | grep -q '^rl-enc-ios-'; then run=$r; break; fi
  done
fi
[ -n "$run" ] || { echo "no finished run with an iPhone build artifact (rl-enc-ios-*) was found in $REPO" >&2; exit 1; }
tmp="$out/.fetch-$run"; rm -rf "$tmp"; mkdir -p "$tmp"
echo "downloading the encrypted artifact of run $run ..."
"$GH" run download "$run" -R "$REPO" -D "$tmp" -p 'rl-enc-ios-*'
parts=$(find "$tmp" -name 'RocketLeague.ipa.age.part*' | sort)
[ -n "$parts" ] || { echo "run $run has no IPA in its artifact (the build did not get that far; its log is in the same artifact: scripts/fetch_artifact.sh $run)" >&2; exit 1; }
ipa="$out/RocketLeague-run$run.ipa"
echo "decrypting to $ipa ..."
# shellcheck disable=SC2086
cat $parts | age -d -i "$KEY" -o "$ipa"
sum=$(find "$tmp" -name 'ipa.sha256.age' | head -1)
if [ -n "$sum" ]; then
  want=$(age -d -i "$KEY" "$sum" | tr -d ' \n'); got=$(sha256sum "$ipa" | cut -d' ' -f1)
  [ "$want" = "$got" ] || { echo "the decrypted IPA does not match the checksum the build recorded (got $got, want $want)" >&2; exit 1; }
  echo "checksum matches the build's"
fi
rm -rf "$tmp"
ls -lh "$ipa"
echo "unsigned IPA ready: $ipa"
