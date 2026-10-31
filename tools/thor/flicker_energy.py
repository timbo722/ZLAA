# flicker_energy.py <shots dir> <first> <last> [sigma] [-v]: how much the picture flickers. Each frame's neighbours are
# shifted onto it (the camera's motion, phase correlation), then mean |f - (prev + next)/2|: steady motion, even
# with depth, cancels out; a flicker doesn't. Also the share of pixels where it exceeds 6 levels. Median over frames.
import os, sys, numpy as np, cv2
d, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); sig = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
win = cv2.createHanningWindow((1280, 720), cv2.CV_32F)
L = lambda n: cv2.imread(os.path.join(d, 'frame_%06d.png' % n), 0).astype(np.float32)
W = lambda img, dx, dy: cv2.warpAffine(img, np.float32([[1, 0, dx], [0, 1, dy]]), (1280, 720), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
B = lambda x: cv2.GaussianBlur(x, (0, 0), sig) if sig > 0 else x
f = {n: L(n) for n in range(a - 1, b + 2)}
m = np.zeros((720, 1280), bool); m[60:-60, 60:-60] = True
es, ps = [], []
for n in range(a, b + 1):
    (px, py), _ = cv2.phaseCorrelate(f[n - 1] * win, f[n] * win); (nx, ny), _ = cv2.phaseCorrelate(f[n] * win, f[n + 1] * win)
    if max(abs(px), abs(py), abs(nx), abs(ny)) > 12: continue
    r = np.abs(B(f[n]) - (B(W(f[n - 1], px, py)) + B(W(f[n + 1], -nx, -ny))) / 2)[m]
    es.append(r.mean()); ps.append((r > 6).mean() * 100)
    if len(sys.argv) > 5: print('%d %.2f %.2f%%' % (n, es[-1], ps[-1]))
print('%s %d-%d: energy median %.2f, pixels over 6 levels %.2f%%  (%d frames)' % (d, a, b, np.median(es), np.median(ps), len(es)))
