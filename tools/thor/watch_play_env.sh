#!/bin/bash
# watch_play.sh <tag>: the player plays (his own saves); sync trace + frame stats on; the panel recorded in 3-minute pieces
# from when he opens the app until he closes it; then the pieces and log.txt are pulled
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; tag=$1
{ cat /mnt/c/Users/you/laa_env_play.txt; printf 'LAA_FRAME_STATS=1\nLAA_STALL_PROFILE=100\n'; for kv in "${@:2}"; do printf '%s\n' "$kv"; done; } > /mnt/c/Users/you/laa_env.txt
$W -s <serial> push 'C:\Users\you\laa_env.txt' $A/env.txt > /dev/null
$W -s <serial> shell "rm -f /data/local/tmp/play_*.mp4"
echo "waiting for the app to start $(date +%T)"
while [ -z "$($W -s <serial> shell pidof io.github.laarecomp | tr -d '\r')" ]; do sleep 0.3; done
echo "app started $(date +%T)"
$W -s <serial> logcat -c
$W -s <serial> logcat -b all -v time > $S/play_${tag}_logcat.txt 2>&1 &
lc=$!
n=0
while [ -n "$($W -s <serial> shell pidof io.github.laarecomp | tr -d '\r')" ]; do
  $W -s <serial> shell screenrecord --time-limit 180 --bit-rate 8000000 /data/local/tmp/play_$n.mp4
  n=$((n+1))
done
echo "app closed $(date +%T), $n pieces"
kill $lc 2>/dev/null
$W -s <serial> pull $A/log.txt 'C:\Users\you\laa_log.txt' > /dev/null; cp /mnt/c/Users/you/laa_log.txt $S/play_$tag.txt
for i in $(seq 0 $((n-1))); do
  $W -s <serial> pull /data/local/tmp/play_$i.mp4 "C:\\Users\\you\\play_$i.mp4" > /dev/null; mv /mnt/c/Users/you/play_$i.mp4 $S/play_${tag}_$i.mp4
done
$W -s <serial> push 'C:\Users\you\laa_env_play.txt' $A/env.txt > /dev/null
echo done
