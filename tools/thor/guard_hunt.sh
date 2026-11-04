#!/bin/bash
# guard_hunt.sh <max runs> [KEY=VALUE...]: the beach script (testsave2) with LAA_SHADER_GUARD=1, until a shader reports
# hitting the translator's dispatch guard (the shaders that used to hang the GPU); also counts kernel GPU hangs
. "$(dirname "$0")/env.sh"
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S
n=$1; shift
before=$($W shell "dmesg 2>/dev/null | grep -c 'GPU hang detected'" | tr -d '\r')
for r in $(seq $n); do
  SCRIPT="$(cat $T/beach_script.txt)" $T/thor_run.sh $S/guard$r.txt LAA_SAVE_DIR=$A/testsave2 LAA_SHADER_GUARD=1 LAA_GPU_TIMING_AT=0 "$@" > /dev/null 2>&1
  now=$($W shell "dmesg 2>/dev/null | grep -c 'GPU hang detected'" | tr -d '\r')
  hits=$(grep -c 'hit the dispatch guard' $S/guard$r.txt)
  echo "run $r: $hits guard reports; kernel GPU hangs $before -> $now"
  [ "$now" != "$before" ] && $W shell "dmesg 2>/dev/null | grep -B5 -A40 'GPU hang detected'" > $S/guard${r}_dmesg.txt   # (the kernel's own report)
  if [ "$hits" != "0" ]; then grep 'hit the dispatch guard' $S/guard$r.txt | sed -E 's/frame [0-9]+: //' | sort | uniq -c; break; fi
done
