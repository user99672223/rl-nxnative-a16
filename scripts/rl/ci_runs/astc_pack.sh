#!/bin/bash
# CI: the ASTC pack of the game's BC textures (dm/tools/ue3_astc.py), made once and kept in the build folder (so the encrypted build cache carries it): <build>/bc_astc.pack.
# Needs the Mac host and the rewritten modules of the build (the game's own Oodle decoder) and RL_UE3_PKG_KEY. Everything it prints goes to the encrypted log.
set -u
B=$(ls -d "$RL_GAME_DIR"/*_build | head -1); out="$B/bc_astc.pack"
if [ -s "$out" ] && [ "${RL_ASTC_REBUILD:-0}" != 1 ]; then echo "pack already made: $(du -h "$out" | cut -f1)"; exit 0; fi
host="$B/tools/dmhost"; patched=$(ls -d "$B"/patched_* | head -1); romfs=$(ls "$B"/extract/*/RomFS.img 2>/dev/null | head -1)
[ -n "$romfs" ] || romfs=$(ls -d "$B"/extract/*/RomFS 2>/dev/null | head -1)
[ -n "$romfs" ] || { echo "no extracted RomFS"; ls "$B"/extract/*; exit 3; }
T="$B/tools/astc"; mkdir -p "$T"
if [ ! -x "$T/bc2astc" ]; then
  # Arm's astcenc (Apache 2.0), built here: not part of this repository
  if [ ! -d "$T/astc-encoder" ]; then git clone -q --depth 1 --branch 5.2.0 https://github.com/ARM-software/astc-encoder.git "$T/astc-encoder" || exit 3; fi
  rm -rf "$T/astc-build"
  cmake -S "$T/astc-encoder" -B "$T/astc-build" -DCMAKE_BUILD_TYPE=Release -DASTCENC_ISA_NEON=ON -DASTCENC_UNIVERSAL_BUILD=OFF -DCMAKE_OSX_ARCHITECTURES=arm64 -DASTCENC_CLI=OFF -DASTCENC_SHAREDLIB=OFF >/dev/null || exit 3
  cmake --build "$T/astc-build" -j "$(sysctl -n hw.ncpu)" >/dev/null || exit 3
  lib=$(find "$T/astc-build" -name 'libastcenc*neon*-static.a' | head -1); echo "astcenc library: $lib"   # the NEON one: a universal build also leaves an x86 library there
  c++ -O2 -std=c++17 -I"$T/astc-encoder/Source" "$GITHUB_WORKSPACE/dm/tools/bc_astc/bc2astc.cpp" "$lib" -o "$T/bc2astc" || exit 3
fi
python -m pip -q install pycryptodome >/dev/null 2>&1 || true
G="$GITHUB_WORKSPACE/dm/games/$GAME"; [ -f "$G/game.tsv" ] || export DM_GAMES_DIR="$B/profile"
export DM_GAME="$GAME" DM_OODLE_HOST="$host" DM_OODLE_MODULES="$patched"
time python "$GITHUB_WORKSPACE/dm/tools/ue3_astc.py" "$romfs" "$out.tmp" --bc2astc "$T/bc2astc" --threads "$(sysctl -n hw.ncpu)" || { echo "pack failed"; rm -f "$out.tmp"; exit 4; }
mv "$out.tmp" "$out"; ls -l "$out"
