#!/bin/bash
# CI: boot the Mac host headless on the null GPU and keep its log. Output goes to $RL_OUT/run (encrypted before upload).
# RL_RUN_SECONDS (default 120), RL_RUN_ENV ("K=V K=V ...", extra host switches), RL_GAME_ARGS (the game's own command line, as Unreal reads it: a map URL).
set -u
B=$(ls -d "$RL_GAME_DIR"/*_build | head -1); R="$RL_OUT/run"; mkdir -p "$R" "$B/run/save" "$B/run/temp"
host="$B/tools/dmhost"; patched=$(ls -d "$B"/patched_* | head -1); romfs=$(ls "$B"/extract/*/RomFS.img 2>/dev/null | head -1)
[ -n "$romfs" ] || romfs=$(ls -d "$B"/extract/*/RomFS 2>/dev/null | head -1)
G="$GITHUB_WORKSPACE/dm/games/$GAME"; [ -f "$G/game.tsv" ] || { export DM_GAMES_DIR="$B/profile"; G="$B/profile/$GAME"; }
echo "host=$host patched=$patched romfs=$romfs profile=$G"; ls -la "$B" "$B/tools" "$B/shader" 2>&1 | head -60
[ -x "$host" ] || { echo "no host binary"; exit 3; }
[ -s "$B/bc_astc.pack" ] && export DM_BCSUB_PACK="$B/bc_astc.pack"   # the ASTC stand-ins of the BC textures (astc_pack.sh): used with NX_FORCE_ASTC_ONLY=1 in RL_RUN_ENV
unset DM_UE3_PKG_KEY
cd "$B/run" || exit 3
# shellcheck disable=SC2086
env DM_ROOT="$B" DM_ROMFS="$romfs" DM_SAVE="$B/run/save" DM_TEMP="$B/run/temp" DM_MSL_DIR="$B/shader/cache" DM_FONT_DIR="$B/font" \
    DM_NVN_SHMAP="$G/shader_map.tsv" DM_LANG=en-US DM_NVN_NULL=1 DM_WATCH_STALL=30 ${RL_RUN_ENV:-} \
    perl -e 'alarm shift; exec @ARGV' "${RL_RUN_SECONDS:-120}" "$host" "$patched" ${RL_GAME_ARGS:+-- $RL_GAME_ARGS} > "$R/host.log" 2>&1
rc=$?
echo "host rc=$rc, $(wc -l < "$R/host.log") lines"
for f in "$B"/run/*.csv "$B"/run/*.json "$B"/run/*.tsv; do [ -f "$f" ] && cp "$f" "$R/"; done   # DM_PERF_EXPORT / DM_NVN_TSV written into the run folder
LC_ALL=C grep -aF "[RED]" "$R/host.log" | sed 's/^\[[^]]*\] //' | sort | uniq -c | sort -nr | head -80 > "$R/red.txt"
ls -t ~/Library/Logs/DiagnosticReports/ 2>/dev/null | head -3 | while read -r f; do cp ~/Library/Logs/DiagnosticReports/"$f" "$R/" 2>/dev/null; done
exit 0
