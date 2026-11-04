#!/bin/bash
# hang_hunt.sh <save folder> <max runs> [KEY=VALUE...]: load the save in the app folder's <save folder> by script
# (SCRIPT as thor_run.sh; default: load the save) again and again, each run from a fresh copy of the save
# (<save folder>.orig, made on the first run), with the GPU watch on, until a submission never finishes (a GPU
# hang); then that run's log is kept as hang<run>.txt with the kernel's report if it can be read.
. "$(dirname "$0")/env.sh"
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S
save=$1; n=$2; shift 2
$W shell "[ -d $A/$save.orig ] || { cp -r $A/$save $A/$save.orig && chmod -R 777 $A/$save.orig; }"
for r in $(seq $n); do
  $W shell "rm -rf $A/$save && cp -r $A/$save.orig $A/$save && chmod -R 777 $A/$save"
  $T/thor_run.sh $S/hang$r.txt LAA_SAVE_DIR=$A/$save LAA_GPU_WATCH=100 LAA_STALL_PROFILE=200 LAA_GPU_TIMING_AT=0 "$@" > $S/hang$r.run 2>&1
  hung=$(grep -c "didn't finish" $S/hang$r.txt)
  echo "run $r: $(grep -c 'over 20 ms' $S/hang$r.txt) stat lines, $hung unfinished submissions; $(grep thor_run $S/hang$r.run | tail -1)"
  if [ "$hung" != "0" ]; then
    grep -A3 "didn't finish" $S/hang$r.txt
    $W shell "dmesg 2>/dev/null | grep -B5 -A40 'GPU hang detected'" > $S/hang${r}_dmesg.txt
    break
  fi
done
