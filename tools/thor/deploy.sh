#!/bin/bash
# deploy.sh: build libmain.so + APK, install on the Thor; stops at the first failure
set -e
cd /home/you/LAARecomp
out=$(cmake --build ~/laar-build/android/build/game --target main 2>&1) || { echo "$out" | grep -E "error|FAILED" | head; exit 1; }
echo "$out" | grep -E "warning:" | head -5 || true
tools/android/build_apk.sh > ${LAA_OUT:-$HOME/laar-build/thor-out}/apk.log 2>&1 || { tail -5 ${LAA_OUT:-$HOME/laar-build/thor-out}/apk.log; exit 1; }
cp ~/laar-build/android/LAARecomp.apk /mnt/c/Users/you/
~/laar-build/android/win/platform-tools/adb.exe -s <serial> install -r 'C:\Users\you\LAARecomp.apk' | tail -1
