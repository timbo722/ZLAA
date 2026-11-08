#!/bin/bash
# deploy.sh: build libmain.so + APK, install on the Thor; build the game library and push it
# (only when it changed) to <app folder>/game, where the app picks it up at start-up
# (runtime/game_lib.c copies it to internal storage). Stops at the first failure.
. "$(dirname "$0")/env.sh"
set -e
cd "$(dirname "$0")/../.."
B=~/laar-build/android/build/game
out=$(cmake --build $B --target main game 2>&1) || { echo "$out" | grep -E "error|FAILED" | head; exit 1; }
echo "$out" | grep -E "warning:" | head -5 || true
tools/android/build_apk.sh > ${LAA_OUT:-$HOME/laar-build/thor-out}/apk.log 2>&1 || { tail -5 ${LAA_OUT:-$HOME/laar-build/thor-out}/apk.log; exit 1; }
mkdir -p $X/apk; cp ~/laar-build/android/ZLAA.apk $X/apk/
$W install -r "$(xa apk/ZLAA.apk)" | tail -1
tools/thor/push_game.sh $B      # the game library (from your own dump; never in the APK)
