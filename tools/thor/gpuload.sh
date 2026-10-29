#!/bin/bash
# gpuload.sh <tag> [KEY=VALUE...]: thor_heat run; prints GPU busy x clock (Mcycles/s of real work), battery mA, frame stats
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; tag=$1; shift
$T/thor_heat.sh $S/gl_$tag LAA_TRACE_DRS=1 "$@"
awk -v t=$tag '/ % / {w+=$1*$3/1e8; busy+=$1; clk+=$3; split($9,a,"="); ib+=a[2]; cpu+=substr($7,5); n++} END {printf "%-10s GPU %4.0f Mcycles/s (busy %.0f%% at %.0f MHz), battery %4.0f mA, cpu %.1f C", t, w/n, busy/n, clk/n/1e6, ib/n/1000, cpu/n/1000}' $S/gl_$tag.heat.txt
grep "^drs:" $S/gl_$tag.log | awk '$3>=1800 {s+=$7; n++} END {printf ", game-measured %.2f ms", s/n}'
grep "frames:" $S/gl_$tag.log | awk 'NR>=6 {o+=$9} END {printf ", %d frames over 20 ms\n", o}'
