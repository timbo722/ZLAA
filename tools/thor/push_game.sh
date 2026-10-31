#!/bin/bash
# push_game.sh [build dir]: strip a game library (libgame-<main build id>.so, default build/game) and push it to
# <app folder>/game/ when it changed; the app copies it to internal storage at start-up (runtime/game_lib.c)
set -e
B=${1:-$HOME/laar-build/android/build/game}; W=~/laar-build/android/win/platform-tools/adb.exe
A=/storage/emulated/0/Android/data/io.github.laarecomp/files
G=$(ls $B/libgame-*.so); N=$(basename $G); S=/mnt/c/Users/you/Documents/LAARecomp/game/$N
mkdir -p /mnt/c/Users/you/Documents/LAARecomp/game
if [ ! -f $S ] || [ $G -nt $S ]; then
  STRIP=$(ls ~/laar-build/android/sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip | head -1)
  $STRIP --strip-debug -o $S.tmp $G && touch -r $G $S.tmp && mv $S.tmp $S
  $W -s <serial> shell mkdir -p $A/game
  $W -s <serial> push "C:\\Users\\you\\Documents\\LAARecomp\\game\\$N" $A/game/ | tail -1
fi
