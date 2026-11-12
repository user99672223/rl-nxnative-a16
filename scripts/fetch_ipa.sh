#!/bin/bash
# Download the newest encrypted IPA built by the "RL build" workflow and decrypt it on this laptop.
#
#   scripts/fetch_ipa.sh [run id] [output folder]
#   scripts/fetch_ipa.sh --update [run id] [output folder]
#
# --update downloads only the build's update package (about 40 MB: the app without its large game data files) and builds the new IPA from it and the newest IPA
# already in the output folder (RL_OLD_IPA names another). It refuses when the build changed a large file; then fetch the whole IPA without --update.
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
update=0; [ "${1:-}" = "--update" ] && { update=1; shift; }
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
if [ "$update" = 1 ]; then
  old="${RL_OLD_IPA:-$(ls -t "$out"/*.ipa 2>/dev/null | head -1)}"
  [ -f "${old:-}" ] || { echo "no IPA in $out to update from: fetch the whole one first (scripts/fetch_ipa.sh $run)" >&2; exit 1; }
  ipa="$out/RocketLeague-run$run.ipa"; [ "$old" != "$ipa" ] || { echo "$ipa is already this run's"; exit 0; }
  mkdir -p "$tmp"; export TMPDIR="$tmp"
  echo "downloading the update package of run $run ..."
  "$GH" run download "$run" -R "$REPO" -D "$tmp" -p 'rl-enc-iosupdate-*'
  pkg=$(find "$tmp" -name 'RocketLeague.update.zip.age' | head -1)
  [ -n "$pkg" ] || { echo "run $run has no update package (a build from before 2026-11-12 evening, or one that failed)" >&2; exit 1; }
  age -d -i "$KEY" -o "$tmp/update.zip" "$pkg"
  echo "building $ipa from $(basename "$old") ..."
  python3 "$here/rl/ipa_delta.py" apply "$old" "$tmp/update.zip" "$ipa" || { rm -f "$ipa"; rm -rf "$tmp"; exit 1; }
  rm -rf "$tmp"; ls -lh "$ipa"; echo "unsigned IPA ready: $ipa"; exit 0
fi
# gh keeps the artifact's zip in the temporary folder while it downloads: several GB, and /tmp is often memory (tmpfs). It goes beside the output instead
export TMPDIR="$tmp"
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
