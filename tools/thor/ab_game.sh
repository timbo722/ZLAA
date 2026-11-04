#!/bin/bash
# ab_game.sh <tag> <rounds> <library A> <library B>: compare two game libraries (files in $LAA_HOST_DIR/game,
# e.g. libgame-AE16F71E002AF8CB.before-x.so) on the village walk (walk.txt), alternating A B A B... so the CPU
# clock's swings hit both: simpleperf from frame 2400 for 18 s; per run the CPU cycles (billions) spent in the game
# library and in libmain.so (the runtime), the prime core's clock and the frame stats. Leaves library B installed.
. "$(dirname "$0")/env.sh"
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}
tag=$1; rounds=$2; shift 2
for r in $(seq $rounds); do
  for v in A B; do
    lib=$([ $v = A ] && echo $1 || echo $2); id=$(echo $lib | grep -o 'libgame-[0-9A-F]*')
    $W push "$(xa game/$lib)" $A/game/$id.so > /dev/null
    SCRIPT="$(cat $T/walk.txt)" $T/thor_perf.sh $S/ab_${tag}_$v$r 2400 18 LAA_GPU_TIMING_AT=0 > /dev/null 2>&1
    $W shell "simpleperf report -i /data/local/tmp/perf.data --sort dso" > $S/ab_${tag}_$v$r.dso.txt 2>&1
    gcyc() { awk -v p="$1" '/^Event count:/ {n=$3} index($0, p) {sub("%", "", $1); printf "%.2f", n * $1 / 1e11; exit}' $S/ab_${tag}_$v$r.dso.txt; }
    game=$(gcyc libgame-); main=$(gcyc libmain.so)
    clk=$(grep -o "average [0-9.]* GHz" $S/ab_${tag}_$v$r.log)
    slow=$(grep "frames:" $S/ab_${tag}_$v$r.log | awk 'NR>=9 && NR<=12 {o+=$8; m+=$14} END {printf "%d over 20 ms, %.1f ms recording", o, m/4}')
    echo "$v$r ($lib): Gcycles in game code $game, runtime $main; cpu7 $clk; frames 2400-3600: $slow"
  done
done
