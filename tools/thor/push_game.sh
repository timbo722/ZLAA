#!/bin/bash
# push_game.sh [build dir]: strip a game library (libgame-<main build id>.so, default build/game) and push it to
# <app folder>/game/ when it changed; the app copies it to internal storage at start-up (runtime/game_lib.c)
. "$(dirname "$0")/env.sh"
set -e
B=${1:-$HOME/laar-build/android/build/game}
G=$(ls $B/libgame-*.so); N=$(basename $G); S=$X/game/$N
mkdir -p $X/game
if [ ! -f $S ] || [ $G -nt $S ]; then
  STRIP=$(ls ~/laar-build/android/sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip | head -1)
  $STRIP --strip-debug -o $S.tmp $G && touch -r $G $S.tmp && mv $S.tmp $S
  $W shell mkdir -p $A/game
  $W push "$(xa game/$N)" $A/game/ | tail -1
fi
