#!/bin/bash
# thor_power.sh <tag> [KEY=VALUE...]: thor_run (SCRIPT env as usual); from "frame 1800 presented" for 30 s: average clock of
# each CPU cluster and the GPU (time_in_state / trans_stat), GPU busy, temps, battery current; slow frames after 1800.
# Report: pw_report.py pw_<tag>.raw pw_<tag>.log <tag>
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; tag=$1; shift
cat > $S/pw_probe.sh <<'EOF'
snap() {
  for p in 0 3 7; do echo "P$p $(cat /sys/devices/system/cpu/cpufreq/policy$p/stats/time_in_state | tr '\n' ' ')"; done
  echo "G $(sed -n '3,11p' /sys/class/kgsl/kgsl-3d0/devfreq/trans_stat | tr -d '*' | awk '{printf "%s %s ", $1, $NF}')"
}
snap
for i in $(seq 15); do
  echo "S $(tr -d ' %' < /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage) $(cat /sys/class/power_supply/battery/current_now) $(cat /sys/class/power_supply/usb/current_now)"
  sleep 2
done
snap
for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type); case $t in cpu-1-*|gpuss-*) echo "T $t $(cat $z/temp)";; esac; done
EOF
cp $S/pw_probe.sh /mnt/c/Users/you/Documents/LAARecomp/transfer/pw_probe.sh
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\transfer\pw_probe.sh' /data/local/tmp/pw_probe.sh > /dev/null
( sleep 15; for i in $(seq 600); do
    [ "$($W -s <serial> shell "grep -c 'frame 1800 presented' $A/log.txt 2>/dev/null" | tr -d "\r")" = "1" ] && break; sleep 0.2
  done
  $W -s <serial> shell sh /data/local/tmp/pw_probe.sh > $S/pw_$tag.raw 2>&1
) &
sleep 3
$T/thor_run.sh $S/pw_$tag.log "$@" > /dev/null 2>&1
wait
python3 $T/pw_report.py $S/pw_$tag.raw $S/pw_$tag.log $tag
