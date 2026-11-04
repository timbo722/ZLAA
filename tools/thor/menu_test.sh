#!/bin/bash
# menu_test.sh <out dir> [seconds]: the menus on the Thor without hands. Start screen -> Settings (two rows) -> back ->
# Play; the pause menu opens 45 s after start-up -> Settings -> back -> Resume. LAA_UI_SCRIPT presses the buttons and
# saves the menu's frames (shots/ui_*.png); an adb screencap at the end shows the game running again. Uses the test
# save (testsave); puts the player's env.txt back afterwards.
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
out=$1; secs=${2:-75}; mkdir -p "$out"
{ cat /mnt/c/Users/you/Documents/LAARecomp/env/laa_env_play.txt
  printf "LAA_SAVE_DIR=%s/testsave\nLAA_UI_SHOTS=%s/shots\nLAA_UI_PAUSE_AT=45\n" $A $A
  printf "LAA_UI_SCRIPT=%s\n" "${SCRIPT:-90:SHOT,95:DOWN,110:A,140:SHOT,150:DOWN,165:SHOT,170:R,185:SHOT,190:B,205:UP,215:A,240:SHOT,250:DOWN,260:A,290:SHOT,300:B,310:UP,320:SHOT,330:A}"
} > /mnt/c/Users/you/Documents/LAARecomp/env/laa_env.txt
$W -s <serial> shell input keyevent KEYCODE_WAKEUP
$W -s <serial> shell am force-stop io.github.laarecomp
$W -s <serial> shell "mkdir -p $A/shots; rm -f $A/shots/*"
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\env\laa_env.txt' $A/env.txt > /dev/null
$W -s <serial> shell am start -n io.github.laarecomp/org.libsdl.app.SDLActivity > /dev/null
sleep "$secs"
$W -s <serial> exec-out screencap -p > "$out/end.png"
$W -s <serial> shell am force-stop io.github.laarecomp
$W -s <serial> push 'C:\Users\you\Documents\LAARecomp\env\laa_env_play.txt' $A/env.txt > /dev/null
$W -s <serial> shell "cat $A/log.txt" > "$out/log.txt"
for f in $($W -s <serial> shell "ls $A/shots" | tr -d '\r'); do $W -s <serial> exec-out cat "$A/shots/$f" > "$out/$f"; done
$W -s <serial> shell "rm -f $A/shots/*"
grep -E "^ui:|^menu:|^app:" "$out/log.txt"
