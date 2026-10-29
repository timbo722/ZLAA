#!/bin/bash
# fade.sh <tag> [KEY=VALUE...]: the load-in: frame times 1640-1660 and each frame's brightness relative to frame 1680
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S; tag=$1; shift
rm -rf $S/fade_$tag
SHOT_EVERY=1 $T/thor_run.sh $S/fade_$tag.txt LAA_SCREENSHOT_FROM=1640 LAA_SCREENSHOT_TO=1680 LAA_SCREENSHOT_SCALE=4 LAA_TRACE_SYNC=1640 LAA_TRACE_SYNC_FRAMES=25 "$@" >/dev/null 2>&1
$T/pullshots.sh $S/fade_$tag >/dev/null
grep "^sync:" $S/fade_$tag.txt | sort -k2 -n | grep presented | awk '{t=$2; if (p) printf "%s:%.0f ", $5, t-p; p=t}'; echo
${LAA_PY:-$HOME/laar-build/venv/bin/python} - $S/fade_$tag <<'PY'
import sys, os, numpy as np
from PIL import Image
d = sys.argv[1]; fs = sorted(f for f in os.listdir(d) if f.startswith('frame_'))
ref = np.asarray(Image.open(os.path.join(d, fs[-1])), dtype=float).mean()
print(' '.join('%s:%.2f' % (f[8:12], np.asarray(Image.open(os.path.join(d, f)), dtype=float).mean() / ref) for f in fs[:22]))
PY
