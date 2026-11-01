# flicker_energy.py <shots dir> <first> <last> [sigma] [-v] [--panel]: how much the picture flickers. Each frame's
# neighbours are shifted onto it (the camera's motion, phase correlation), then mean |f - (prev + next)/2|: steady
# motion, even with depth, cancels out; a flicker doesn't. Also the share of pixels where it exceeds 6 levels. Median
# over frames. --panel: every frame scaled to 1920x1080 first (bilinear, as the present does), sigma in panel pixels:
# compares 720p with 1080p as the Thor's screen shows them.
import os, sys, numpy as np, cv2
args = [x for x in sys.argv[1:] if not x.startswith('-')]; panel = '--panel' in sys.argv; verbose = '-v' in sys.argv
d, a, b = args[0], int(args[1]), int(args[2]); sig = float(args[3]) if len(args) > 3 else 1.0
def L(n):                                              # (frames the screenshots missed are left out)
    p = os.path.join(d, 'frame_%06d.png' % n)
    if not os.path.exists(p): return None
    img = cv2.imread(p, 0).astype(np.float32)
    return cv2.resize(img, (1920, 1080), interpolation=cv2.INTER_LINEAR) if panel else img
f = {n: img for n in range(a - 1, b + 2) if (img := L(n)) is not None}
h, w = next(iter(f.values())).shape; k = h / 720                         # sizes below are for 720p
win = cv2.createHanningWindow((w, h), cv2.CV_32F)
W = lambda img, dx, dy: cv2.warpAffine(img, np.float32([[1, 0, dx], [0, 1, dy]]), (w, h), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
B = lambda x: cv2.GaussianBlur(x, (0, 0), sig) if sig > 0 else x
e = int(60 * k); m = np.zeros((h, w), bool); m[e:-e, e:-e] = True
es, ps = [], []
for n in range(a, b + 1):
    if not (n - 1 in f and n in f and n + 1 in f): continue
    (px, py), _ = cv2.phaseCorrelate(f[n - 1] * win, f[n] * win); (nx, ny), _ = cv2.phaseCorrelate(f[n] * win, f[n + 1] * win)
    if max(abs(px), abs(py), abs(nx), abs(ny)) > 12 * k: continue
    r = np.abs(B(f[n]) - (B(W(f[n - 1], px, py)) + B(W(f[n + 1], -nx, -ny))) / 2)[m]
    es.append(r.mean()); ps.append((r > 6).mean() * 100)
    if verbose: print('%d %.2f %.2f%%' % (n, es[-1], ps[-1]))
print('%s %d-%d: energy median %.2f, pixels over 6 levels %.2f%%  (%d frames)' % (d, a, b, np.median(es), np.median(ps), len(es)))
