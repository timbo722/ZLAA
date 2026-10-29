# wavstats.py <wav> [seconds per row]: loudness of a 48 kHz stereo 16-bit WAV over time
# (RMS and peak per window, clipped samples, and how much is exact silence)
import sys, numpy as np
d = open(sys.argv[1], 'rb').read()[44:]
a = np.frombuffer(d[:len(d) // 4 * 4], dtype='<i2').reshape(-1, 2).astype(np.float64)
step = int(48000 * float(sys.argv[2] if len(sys.argv) > 2 else 5))
print(f'{len(a) / 48000:.1f} s')
for i in range(0, len(a), step):
    w = a[i:i + step]
    rms = np.sqrt((w ** 2).mean(axis=0))
    print(f'{i / 48000:6.0f}s  rms L {rms[0]:7.0f} R {rms[1]:7.0f}  peak {np.abs(w).max():6.0f}  '
          f'clipped {(np.abs(w) >= 32767).mean() * 100:5.2f}%  silent {(np.abs(w).max(axis=1) == 0).mean() * 100:5.1f}%')
