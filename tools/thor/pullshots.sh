#!/bin/bash
# pullshots.sh <dest dir>: the Thor's screenshots, downscaled to 480x270 (originals deleted)
W=~/laar-build/android/win/platform-tools/adb.exe; A=/storage/emulated/0/Android/data/io.github.laarecomp/files
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; dest=$1; mkdir -p $dest; rm -rf /mnt/c/Users/you/Documents/LAARecomp/transfer/shots
$W -s <serial> pull $A/shots 'C:\Users\you\Documents\LAARecomp\transfer\shots' > /dev/null
${LAA_PY:-$HOME/laar-build/venv/bin/python} - /mnt/c/Users/you/Documents/LAARecomp/transfer/shots $dest <<'PY'
import sys, os
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for f in sorted(os.listdir(src)):
    if f.endswith('.png'):
        Image.open(os.path.join(src, f)).convert('RGB').resize((480, 270)).save(os.path.join(dst, f))
PY
rm -rf /mnt/c/Users/you/Documents/LAARecomp/transfer/shots
