#!/bin/bash
# background_test.sh <out prefix>: HOME in the middle of a game (testsave), back after 15 s, twice. The game must stop
# while hidden (no frames counted), come back on a new window surface and go on (frames counted, a screenshot).
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
X=/mnt/c/Users/you/Documents/LAARecomp; out=$1
{ printf 'LAA_PLAYER_NAME=Link\nLAA_FONT_DIR=%s/fonts\nLAA_SHOW_FPS=1\nLAA_SAVE_DIR=%s/testsave\n' $A $A
  printf 'LAA_INPUT_SCRIPT=300:A,400:A,700:L+R,900:L+R,1100:L+R,1300:A,1500:A,1700:A\n'; } > $X/env/laa_env.txt
$W -s <serial> shell input keyevent KEYCODE_WAKEUP
$W -s <serial> shell am force-stop io.github.laarecomp
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\env\laa_env.txt' $A/env.txt > /dev/null
$W -s <serial> shell am start -n io.github.laarecomp/org.libsdl.app.SDLActivity > /dev/null
frames() { $W -s <serial> shell "grep -c 'presented' $A/log.txt" | tr -d '\r'; }
for i in $(seq 60); do sleep 5; [ "$(frames)" -ge 8 ] && break; done           # in the village (frame 2400)
for round in 1 2; do
  $W -s <serial> shell input keyevent KEYCODE_HOME; sleep 3; a=$(frames); sleep 12; b=$(frames)
  echo "round $round: hidden: frame-count lines $a -> $b; app pid $($W -s <serial> shell pidof io.github.laarecomp | tr -d '\r')"
  $W -s <serial> shell am start -n io.github.laarecomp/org.libsdl.app.SDLActivity > /dev/null; sleep 12; c=$(frames)
  echo "round $round: back: frame-count lines $b -> $c"
  $W -s <serial> exec-out screencap -p > $out-round$round.png
done
$W -s <serial> pull $A/log.txt 'C:\Users\you\Documents\LAARecomp\transfer\laa_log.txt' > /dev/null; cp $X/transfer/laa_log.txt $out.txt
grep -E "app:|present:|vulkan: swapchain|host backtrace|FORTIFY|failed" $out.txt
$W -s <serial> shell am force-stop io.github.laarecomp
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\env\laa_env_play.txt' $A/env.txt > /dev/null
