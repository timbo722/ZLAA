#!/bin/bash
# thor_run.sh <out.txt> [extra KEY=VALUE...]: load File 2 (Mabe Village) by script, stats on, until frame 3600
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
out=$1; shift
{ printf 'LAA_PLAYER_NAME=Link\nLAA_FONT_DIR=%s/fonts\nLAA_SHOW_FPS=1\nLAA_FRAME_STATS=1\nLAA_GPU_TIMING_AT=3000\n' $A
  printf "LAA_SCREENSHOT=%s/shots\nLAA_SCREENSHOT_EVERY=${SHOT_EVERY:-3000}\nLAA_INPUT_SCRIPT=%s\n" $A "${SCRIPT:-300:A,400:A,700:L+R,900:L+R,1100:L+R,1300:A,1500:A,1700:A}"
  printf "LAA_SAVE_DIR=%s/testsave\n" $A   # (tests never touch the player's own saves)
  for kv in "$@"; do printf '%s\n' "$kv"; done; } > /mnt/c/Users/you/Documents/LAARecomp/env/laa_env.txt
$W -s <serial> shell input keyevent KEYCODE_WAKEUP
$W -s <serial> shell am force-stop io.github.laarecomp
$W -s <serial> shell "rm -f $A/shots/*"
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\env\laa_env.txt' $A/env.txt > /dev/null
$W -s <serial> logcat -c
TIS=/sys/devices/system/cpu/cpu7/cpufreq/stats/time_in_state   # (the prime core's time per clock: the governor
tis_a=$($W -s <serial> shell cat $TIS | tr -d "\r")                #  varies it between sessions, and ms per frame with it)
$W -s <serial> shell am start -n io.github.laarecomp/org.libsdl.app.SDLActivity > /dev/null
# until frame 3600, the app dies, or 6 minutes pass; the app's log.txt has everything (logcat drops lines)
for i in $(seq 72); do
  sleep 5
  [ "$($W -s <serial> shell "grep -c 'frame 3600 presented' $A/log.txt" | tr -d "\r")" = "1" ] && break
  [ "$($W -s <serial> shell "grep -c 'host backtrace' $A/log.txt" | tr -d "\r")" != "0" ] && { echo CRASHED; break; }
  [ -z "$($W -s <serial> shell pidof io.github.laarecomp | tr -d "\r")" ] && { echo "APP EXITED"; break; }
done
tis_b=$($W -s <serial> shell cat $TIS | tr -d "\r")
$W -s <serial> pull $A/log.txt 'C:\Users\you\Documents\LAARecomp\transfer\laa_log.txt' > /dev/null
cp /mnt/c/Users/you/Documents/LAARecomp/transfer/laa_log.txt $out
paste <(echo "$tis_a") <(echo "$tis_b") | awk '{d=$4-$2; t+=d; f+=d*$1; if (d>top) {top=d}} END {if (t) printf "thor_run: prime core (cpu7) average %.2f GHz over the run\n", f/t/1e6}' | tee -a $out
$W -s <serial> pull $A/shots/frame_003000.png 'C:\Users\you\Documents\LAARecomp\transfer\f3000.png' > /dev/null
# leave the player's own settings behind (FPS counter, no test script), and stop the test run
$W -s <serial> shell am force-stop io.github.laarecomp
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\env\laa_env_play.txt' $A/env.txt > /dev/null
