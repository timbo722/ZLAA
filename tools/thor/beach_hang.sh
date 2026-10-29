#!/bin/bash
# beach_hang.sh <max runs>: the beach script with the GPU watch, until the kernel reports a new GPU hang
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
before=$($W -s <serial> shell "dmesg 2>/dev/null | grep -c 'GPU hang detected'" | tr -d '\r')
for r in $(seq $1); do
  SCRIPT="$(cat $T/beach_script.txt)" $T/thor_run.sh $S/hang$r.txt LAA_SAVE_DIR=$A/testsave2 LAA_GPU_WATCH=100 LAA_GPU_WATCH_DRAWS=1 LAA_STALL_PROFILE=200 LAA_GPU_TIMING_AT=0 > /dev/null 2>&1
  now=$($W -s <serial> shell "dmesg 2>/dev/null | grep -c 'GPU hang detected'" | tr -d '\r')
  echo "run $r: hangs $before -> $now; $(grep -c "didn't finish" $S/hang$r.txt) unfinished submissions reported"
  if [ "$now" != "$before" ]; then grep -A4 "didn't finish" $S/hang$r.txt | head -20; break; fi
done
