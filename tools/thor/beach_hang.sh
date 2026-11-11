#!/bin/bash
# beach_hang.sh <max runs> [KEY=VAL...]: the beach script with the GPU watch (a mark per pass; LAA_GPU_WATCH_DRAWS=1
# for one per draw, which seems to hide the hang), until the kernel reports a new GPU hang
# (the newest "GPU hang detected" line changes: dmesg's ring drops old lines, so a count can go down).
# ALL=1: don't stop at the first hang; do every run and count them (a hang rate). Each hang's kernel report and the
# GPU driver's snapshot are saved next to the run's log.
. "$(dirname "$0")/env.sh"
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S
last_hang() { $W shell "dmesg 2>/dev/null | grep 'GPU hang detected' | tail -1" | tr -d '\r'; }
before=$(last_hang); hangs=0
for r in $(seq $1); do
  SCRIPT="$(cat $T/beach_script.txt)" $T/thor_run.sh $S/hang$r.txt LAA_SAVE_DIR=$A/testsave2 LAA_GPU_WATCH=100 LAA_STALL_PROFILE=200 LAA_GPU_TIMING_AT=0 "${@:2}" > /dev/null 2>&1
  now=$(last_hang)
  worst=$(grep -o 'max [0-9.]*' $S/hang$r.txt | awk '$2 > m {m = $2} END {print m + 0}')
  echo "run $r: worst frame $worst ms; $(grep -c "didn't finish" $S/hang$r.txt) unfinished submissions reported"
  if [ "$now" != "$before" ]; then
    echo "new GPU hang: $now"; $W shell "dmesg 2>/dev/null | grep -A3 'GPU hang detected' | tail -4" > $S/hang${r}_dmesg.txt
    # the driver's snapshot of the GPU at the hang (kept until read: reading it lets the next hang be captured);
    # tools/thor/kgsl_snapshot.py reads it
    if [ "$($W shell cat /sys/class/kgsl/kgsl-3d0/snapshot/timestamp | tr -d '\r')" != "0" ]; then
      $W exec-out "cat /sys/class/kgsl/kgsl-3d0/snapshot/dump" > $S/hang${r}_snapshot.bin
      echo "snapshot: $S/hang${r}_snapshot.bin ($(stat -c %s $S/hang${r}_snapshot.bin) bytes)"
    fi
    grep -A4 "didn't finish" $S/hang$r.txt | head -20
    hangs=$((hangs + 1)); before=$now
    [ -z "$ALL" ] && break
  fi
done
echo "$hangs hang(s) in $r run(s)"
