#!/bin/bash
# deploy.sh: build libmain.so + APK, install on the Thor; build the game library and push it
# (only when it changed) to <app folder>/game, where the app picks it up at start-up
# (runtime/game_lib.c copies it to internal storage). Stops at the first failure.
set -e
cd /home/you/LAARecomp
B=~/laar-build/android/build/game; W=~/laar-build/android/win/platform-tools/adb.exe
A=/storage/emulated/0/Android/data/io.github.laarecomp/files
out=$(cmake --build $B --target main game 2>&1) || { echo "$out" | grep -E "error|FAILED" | head; exit 1; }
echo "$out" | grep -E "warning:" | head -5 || true
tools/android/build_apk.sh > ${LAA_OUT:-$HOME/laar-build/thor-out}/apk.log 2>&1 || { tail -5 ${LAA_OUT:-$HOME/laar-build/thor-out}/apk.log; exit 1; }
cp ~/laar-build/android/LAARecomp.apk /mnt/c/Users/you/Documents/LAARecomp/apk/
$W -s <serial> install -r 'C:\Users\you\Documents\LAARecomp\apk\LAARecomp.apk' | tail -1
tools/thor/push_game.sh $B      # the game library (from your own dump; never in the APK)
