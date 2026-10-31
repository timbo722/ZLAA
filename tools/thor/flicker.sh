#!/bin/bash
# flicker.sh <tag> [KEY=VALUE...]: walk through the tall grass east of the house in Mabe Village (testsave), every
# frame 2600-2700 captured, and measure the flicker (flicker_energy.py). Standing still in grass measures ~0.35;
# walking ~2.5 (2026-10-31). Options that change shaders need a second run (cold pipelines skip draws).
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; tag=$1; shift
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
SCRIPT="300:A,400:A,700:L+R,900:L+R,1100:L+R,1300:A,1500:A,1700:A,2200:SDOWN*25,2230:SRIGHT*150,2385:SUP*90,2480:SRIGHT*80,2565:SDOWN*80,2650:SLEFT*80" \
SHOT_EVERY=1 $T/thor_run.sh $S/flicker_$tag.txt LAA_GPU_TIMING_AT=0 LAA_SCREENSHOT_FROM=2599 LAA_SCREENSHOT_TO=2701 "$@" | grep thor_run
X=/mnt/c/Users/you/Documents/LAARecomp/transfer; rm -rf $X/shots $S/flicker_$tag; mkdir -p $X
$W -s <serial> pull $A/shots 'C:\Users\you\Documents\LAARecomp\transfer\shots' > /dev/null; mv $X/shots $S/flicker_$tag
grep -E "skipped" $S/flicker_$tag.txt | sed -n '9,10p'
${LAA_PY:-$HOME/laar-build/venv/bin/python} $T/flicker_energy.py $S/flicker_$tag 2600 2700
