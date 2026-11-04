#!/bin/bash
# thor_heat.sh <out prefix> [KEY=VALUE...]: thor_run (stand in Mabe Village), and from frame 1800 for 30 s every 2 s:
# GPU busy % / clock, CPU clocks (little/mid/prime), hottest CPU and GPU zones, battery current; then top -H once.
. "$(dirname "$0")/env.sh"
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; out=$1; shift
( sleep 15; for i in $(seq 600); do
    [ "$($W shell "grep -c 'frame 1800 presented' $A/log.txt 2>/dev/null" | tr -d "\r")" = "1" ] && break; sleep 0.2
  done
  $W shell 'K=/sys/class/kgsl/kgsl-3d0; T=/sys/class/thermal; for i in $(seq 8); do
      cpu=0; gpu=0; for z in $T/thermal_zone*; do t=$(cat $z/type); v=$(cat $z/temp 2>/dev/null); case $t in cpu-*) [ "$v" -gt $cpu ] && cpu=$v;; gpuss-*) [ "$v" -gt $gpu ] && gpu=$v;; esac; done
      echo "$(cat $K/gpu_busy_percentage) $(cat $K/gpuclk) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) $(cat /sys/devices/system/cpu/cpu3/cpufreq/scaling_cur_freq) $(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq) cpu=$cpu gpu=$gpu ibat=$(cat /sys/class/power_supply/battery/current_now)"
      sleep 2; done; p=$(pidof io.github.laarecomp); for t in /proc/$p/task/*; do echo "A $(cat $t/stat)"; done; sleep 5; for t in /proc/$p/task/*; do echo "B $(cat $t/stat)"; done' > $out.heat.txt 2>&1
) &
sleep 3
$T/thor_run.sh $out.log "$@" > /dev/null 2>&1
wait
